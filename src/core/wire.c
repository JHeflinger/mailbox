#include "wire.h"
#include "data/defaults.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define PACKET_HEADER_SIZE 5
#define PACKET_OVERFLOW ((size_t)-1)

BOOL SendAll(EZ_SOCKET socket, const void* data, size_t size) {
    const char* bytes = (const char*)data;
    while (size > 0) {
        int chunk = size > (1 << 30) ? (1 << 30) : (int)size;
        #ifdef __linux__
        int sent = (int)send(socket, bytes, chunk, MSG_NOSIGNAL);
        #else
        int sent = (int)send(socket, bytes, chunk, 0);
        #endif
        if (sent < 0) {
            #ifndef _WIN32
            if (errno == EINTR) continue;
            #endif
            return FALSE;
        }
        if (sent == 0) return FALSE;
        bytes += sent;
        size -= (size_t)sent;
    }
    return TRUE;
}

BOOL ReceiveAll(EZ_SOCKET socket, void* data, size_t size) {
    char* bytes = (char*)data;
    while (size > 0) {
        int chunk = size > (1 << 30) ? (1 << 30) : (int)size;
        int got = (int)recv(socket, bytes, chunk, 0);
        if (got < 0) {
            #ifndef _WIN32
            if (errno == EINTR) continue;
            #endif
            return FALSE;
        }
        if (got == 0) return FALSE;
        bytes += got;
        size -= (size_t)got;
    }
    return TRUE;
}

BOOL SetSocketTimeout(EZ_SOCKET socket, uint32_t ms) {
    #ifdef _WIN32
    DWORD timeout = (DWORD)ms;
    BOOL ok = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) == 0;
    ok = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout)) == 0 && ok;
    #else
    struct timeval timeout;
    timeout.tv_sec = ms / 1000;
    timeout.tv_usec = (ms % 1000) * 1000;
    BOOL ok = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
    ok = setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 && ok;
    #ifdef __APPLE__
    int nosigpipe = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
    #endif
    #endif
    return ok;
}

BOOL PeerAddress(EZ_SOCKET socket, char* out, size_t size) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (getpeername(socket, (struct sockaddr*)&addr, &len) != 0 || addr.sin_family != AF_INET) {
        snprintf(out, size, "unknown");
        return FALSE;
    }
    ez_Ipv4 ip;
    memcpy(ip.address, &addr.sin_addr.s_addr, 4);
    FormatAddress(ip, out, size);
    return TRUE;
}

BOOL ParseAddress(const char* str, ez_Ipv4* out) {
    unsigned int parts[4];
    char tail;
    if (sscanf(str, "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3], &tail) != 4) return FALSE;
    for (int i = 0; i < 4; i++) {
        if (parts[i] > 255) return FALSE;
        out->address[i] = (uint8_t)parts[i];
    }
    return TRUE;
}

void FormatAddress(ez_Ipv4 address, char* out, size_t size) {
    snprintf(out, size, "%u.%u.%u.%u",
        address.address[0], address.address[1], address.address[2], address.address[3]);
}

BOOL ResolveHost(const char* host, ez_Ipv4* out) {
    if (ParseAddress(host, out)) return TRUE;
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || result == NULL) return FALSE;
    struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
    memcpy(out->address, &addr->sin_addr.s_addr, 4);
    freeaddrinfo(result);
    return TRUE;
}

static void PacketAppend(ez_Buffer* buffer, const void* data, size_t size) {
    if (buffer->current_length == PACKET_OVERFLOW) return;
    if (buffer->current_length + size > buffer->max_length) {
        buffer->current_length = PACKET_OVERFLOW;
        return;
    }
    memcpy(buffer->bytes + buffer->current_length, data, size);
    buffer->current_length += size;
}

void PacketBegin(ez_Buffer* buffer, MessageType type) {
    buffer->current_length = 0;
    uint8_t header[PACKET_HEADER_SIZE] = { (uint8_t)type, 0, 0, 0, 0 };
    PacketAppend(buffer, header, sizeof(header));
}

void PacketU8(ez_Buffer* buffer, uint8_t value) {
    PacketAppend(buffer, &value, 1);
}

void PacketU16(ez_Buffer* buffer, uint16_t value) {
    uint8_t bytes[2] = { (uint8_t)(value >> 8), (uint8_t)value };
    PacketAppend(buffer, bytes, 2);
}

void PacketU32(ez_Buffer* buffer, uint32_t value) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (24 - i * 8));
    PacketAppend(buffer, bytes, 4);
}

void PacketU64(ez_Buffer* buffer, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (56 - i * 8));
    PacketAppend(buffer, bytes, 8);
}

void PacketBytes(ez_Buffer* buffer, const void* data, size_t size) {
    PacketAppend(buffer, data, size);
}

void PacketString(ez_Buffer* buffer, const char* str) {
    size_t len = strlen(str);
    if (len > 0xFFFF) {
        buffer->current_length = PACKET_OVERFLOW;
        return;
    }
    PacketU16(buffer, (uint16_t)len);
    PacketAppend(buffer, str, len);
}

BOOL SendPacket(EZ_SOCKET socket, ez_Buffer* buffer) {
    if (buffer->current_length == PACKET_OVERFLOW || buffer->current_length < PACKET_HEADER_SIZE) return FALSE;
    uint32_t length = (uint32_t)(buffer->current_length - PACKET_HEADER_SIZE);
    for (int i = 0; i < 4; i++) buffer->bytes[1 + i] = (uint8_t)(length >> (24 - i * 8));
    return SendAll(socket, buffer->bytes, buffer->current_length);
}

BOOL ReceivePacket(EZ_SOCKET socket, ez_Buffer* buffer, MessageType* type) {
    uint8_t header[PACKET_HEADER_SIZE];
    *type = MSG_NONE;
    buffer->current_length = 0;
    if (!ReceiveAll(socket, header, sizeof(header))) return FALSE;
    uint32_t length = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                      ((uint32_t)header[3] << 8) | (uint32_t)header[4];
    if (length > buffer->max_length || length > MAX_PACKET_SIZE) return FALSE;
    if (length > 0 && !ReceiveAll(socket, buffer->bytes, length)) return FALSE;
    buffer->current_length = length;
    *type = (MessageType)header[0];
    return TRUE;
}

PacketReader BeginReading(ez_Buffer* buffer) {
    PacketReader reader = { buffer, 0, TRUE };
    return reader;
}

static const uint8_t* ReadRaw(PacketReader* reader, size_t size) {
    if (!reader->ok || reader->cursor + size > reader->buffer->current_length) {
        reader->ok = FALSE;
        return NULL;
    }
    const uint8_t* data = reader->buffer->bytes + reader->cursor;
    reader->cursor += size;
    return data;
}

uint8_t ReadU8(PacketReader* reader) {
    const uint8_t* data = ReadRaw(reader, 1);
    return data ? data[0] : 0;
}

uint16_t ReadU16(PacketReader* reader) {
    const uint8_t* data = ReadRaw(reader, 2);
    return data ? (uint16_t)((data[0] << 8) | data[1]) : 0;
}

uint32_t ReadU32(PacketReader* reader) {
    const uint8_t* data = ReadRaw(reader, 4);
    if (!data) return 0;
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) value = (value << 8) | data[i];
    return value;
}

uint64_t ReadU64(PacketReader* reader) {
    const uint8_t* data = ReadRaw(reader, 8);
    if (!data) return 0;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | data[i];
    return value;
}

void ReadBytes(PacketReader* reader, void* out, size_t size) {
    const uint8_t* data = ReadRaw(reader, size);
    if (data) memcpy(out, data, size);
    else memset(out, 0, size);
}

void ReadString(PacketReader* reader, char* out, size_t size) {
    memset(out, 0, size);
    uint16_t len = ReadU16(reader);
    if (!reader->ok) return;
    if ((size_t)len >= size) {
        reader->ok = FALSE;
        return;
    }
    const uint8_t* data = ReadRaw(reader, len);
    if (!data) return;
    memcpy(out, data, len);
    if (memchr(out, '\0', len) != NULL) reader->ok = FALSE;
}

BOOL FinishReading(PacketReader* reader) {
    return reader->ok && reader->cursor == reader->buffer->current_length;
}
