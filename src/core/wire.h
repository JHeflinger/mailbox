#ifndef WIRE_H
#define WIRE_H

#include <easynet.h>
#include <stdint.h>

typedef enum {
    MSG_NONE = 0,
    MSG_HELLO,
    MSG_WELCOME,
    MSG_AUTH,
    MSG_VERDICT,
    MSG_COMPLETE,
    MSG_RESULT,
    MSG_SHUTDOWN,
    MSG_STATUS,
    MSG_CHAT_QUEUE
} MessageType;

typedef enum {
    VERDICT_ACCEPT = 0,
    VERDICT_BAD_PASSWORD,
    VERDICT_NO_SPACE,
    VERDICT_INVALID,
    VERDICT_VERSION,
    VERDICT_FAILED,
    VERDICT_CORRUPT,
    VERDICT_NOT_PROXY,
    VERDICT_NO_ROUTE,
    VERDICT_DUPLICATE,
    VERDICT_TOO_MANY_HOPS
} VerdictCode;

typedef enum {
    DELIVERY_DIRECT = 0,
    DELIVERY_PROXY
} DeliveryKind;

typedef enum {
    CONTENT_FILE = 0,
    CONTENT_CHAT
} ContentKind;

typedef enum {
    OUTCOME_DELIVERED = 0,
    OUTCOME_FAILED
} DeliveryOutcome;

typedef struct {
    ez_Buffer* buffer;
    size_t cursor;
    BOOL ok;
} PacketReader;

BOOL SendAll(EZ_SOCKET socket, const void* data, size_t size);

BOOL ReceiveAll(EZ_SOCKET socket, void* data, size_t size);

EZ_SOCKET ConnectQuick(ez_Ipv4 address, uint16_t port, uint32_t timeout_ms);

void CloseSocket(EZ_SOCKET socket);

BOOL SetSocketTimeout(EZ_SOCKET socket, uint32_t ms);

BOOL PeerAddress(EZ_SOCKET socket, char* out, size_t size);

BOOL ParseAddress(const char* str, ez_Ipv4* out);

void FormatAddress(ez_Ipv4 address, char* out, size_t size);

BOOL ResolveHost(const char* host, ez_Ipv4* out);

void PacketBegin(ez_Buffer* buffer, MessageType type);

void PacketU8(ez_Buffer* buffer, uint8_t value);

void PacketU16(ez_Buffer* buffer, uint16_t value);

void PacketU32(ez_Buffer* buffer, uint32_t value);

void PacketU64(ez_Buffer* buffer, uint64_t value);

void PacketBytes(ez_Buffer* buffer, const void* data, size_t size);

void PacketString(ez_Buffer* buffer, const char* str);

BOOL SendPacket(EZ_SOCKET socket, ez_Buffer* buffer);

BOOL ReceivePacket(EZ_SOCKET socket, ez_Buffer* buffer, MessageType* type);

PacketReader BeginReading(ez_Buffer* buffer);

uint8_t ReadU8(PacketReader* reader);

uint16_t ReadU16(PacketReader* reader);

uint32_t ReadU32(PacketReader* reader);

uint64_t ReadU64(PacketReader* reader);

void ReadBytes(PacketReader* reader, void* out, size_t size);

void ReadString(PacketReader* reader, char* out, size_t size);

BOOL FinishReading(PacketReader* reader);

#endif
