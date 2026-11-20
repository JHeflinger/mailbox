#define _FILE_OFFSET_BITS 64
#include "transfer.h"
#include "core/platform.h"
#include "core/wire.h"
#include "data/dns.h"
#include "data/state.h"
#include "util/sha256.h"
#include "util/output.h"
#include <easymemory.h>
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define PACKET_BUFFER_SIZE (MAX_PACKET_SIZE + 16)

typedef struct {
    char host[MAX_LINE_SIZE];
    ez_Ipv4 address;
    char addressstr[16];
    uint16_t port;
    BOOL known;
} Recipient;

static BOOL ResolveRecipient(const Config* config, const char* target, Recipient* recipient) {
    memset(recipient, 0, sizeof(Recipient));
    if (strlen(target) >= sizeof(recipient->host)) {
        PrintError("Recipient \"%s\" is too long", target);
        return FALSE;
    }
    strcpy(recipient->host, target);
    uint16_t explicit_port = 0;
    char* colon = strrchr(recipient->host, ':');
    if (colon != NULL && colon[1] != '\0') {
        char* end = NULL;
        long port = strtol(colon + 1, &end, 10);
        if (*end == '\0' && port >= 1024 && port <= 65535) {
            explicit_port = (uint16_t)port;
            *colon = '\0';
        }
    }
    recipient->port = config->port;
    if (ParseAddress(recipient->host, &recipient->address)) {
        // raw ip
    } else {
        ARRLIST_Contact contacts = { 0 };
        if (!LoadDNS(config->dns, &contacts)) PrintWarn("Unable to read DNS file \"%s\"", config->dns);
        Contact* contact = FindContact(&contacts, recipient->host);
        if (contact != NULL && ParseAddress(contact->address, &recipient->address)) {
            recipient->known = TRUE;
            if (contact->port != 0) recipient->port = contact->port;
        }
        CleanDNS(&contacts);
        if (!recipient->known && !ResolveHost(recipient->host, &recipient->address)) {
            PrintError("Unknown recipient \"%s\" - it isn't an IP address, an ID in your DNS file (%s), or a resolvable hostname",
                recipient->host, config->dns);
            return FALSE;
        }
    }
    if (explicit_port != 0) recipient->port = explicit_port;
    FormatAddress(recipient->address, recipient->addressstr, sizeof(recipient->addressstr));
    return TRUE;
}

static BOOL ObtainPassword(const char* server, char* out, size_t size) {
    const char* env = getenv(PASSWORD_ENV_VARIABLE);
    if (env != NULL && env[0] != '\0') {
        snprintf(out, size, "%s", env);
        return TRUE;
    }
    char prompt[MAX_ID_SIZE + 64] = { 0 };
    snprintf(prompt, sizeof(prompt), "Password for %s's mailbox: ", server);
    return ReadPassword(prompt, out, size);
}

static void ReportVerdict(uint8_t code, const char* server, uint64_t filesize, uint64_t available, uint64_t limit, const char* message) {
    char need[32] = { 0 }, free_space[32] = { 0 }, cap[32] = { 0 };
    switch (code) {
        case VERDICT_BAD_PASSWORD:
            PrintError("Incorrect password for %s's mailbox - transfer rejected", server);
            break;
        case VERDICT_NO_SPACE:
            FormatSize(need, sizeof(need), filesize);
            FormatSize(free_space, sizeof(free_space), available);
            FormatSize(cap, sizeof(cap), limit);
            PrintError("Not enough room in %s's mailbox - the file needs %s but only %s of %s is free", server, need, free_space, cap);
            break;
        case VERDICT_VERSION:
            PrintError("%s is running an incompatible version of mailbox", server);
            break;
        case VERDICT_CORRUPT:
            PrintError("%s received corrupted data - transfer discarded, please try again", server);
            break;
        default:
            PrintError("%s rejected the transfer%s%s", server, message[0] ? " - " : "", message);
            break;
    }
}

static BOOL StreamFile(EZ_SOCKET socket, const char* filepath, uint64_t filesize, uint8_t digest[SHA256_SIZE]) {
    FILE* file = fopen(filepath, "rb");
    if (file == NULL) {
        PrintError("Unable to open \"%s\" for reading", filepath);
        return FALSE;
    }
    uint8_t* chunk = EZ_ALLOC(CHUNK_SIZE, sizeof(uint8_t));
    Sha256 sha;
    Sha256Begin(&sha);
    Progress progress;
    BeginProgress(&progress, filesize);
    uint64_t sent = 0;
    BOOL ok = TRUE;
    while (sent < filesize) {
        size_t want = (filesize - sent) > CHUNK_SIZE ? CHUNK_SIZE : (size_t)(filesize - sent);
        size_t got = fread(chunk, 1, want, file);
        if (got != want) {
            EndProgress(&progress);
            PrintError("Unable to read \"%s\" - did it change while sending?", filepath);
            ok = FALSE;
            break;
        }
        Sha256Feed(&sha, chunk, got);
        if (!SendAll(socket, chunk, got)) {
            EndProgress(&progress);
            PrintError("Connection lost while sending");
            ok = FALSE;
            break;
        }
        sent += got;
        UpdateProgress(&progress, sent);
    }
    if (ok) EndProgress(&progress);
    Sha256Finish(&sha, digest);
    EZ_FREE(chunk);
    fclose(file);
    return ok;
}

static BOOL Deliver(const Config* config, EZ_SOCKET socket, ez_Buffer* buffer, const Recipient* recipient,
        const char* filepath, const char* filename, uint64_t filesize) {
    SetSocketTimeout(socket, HANDSHAKE_TIMEOUT_MS);

    PacketBegin(buffer, MSG_HELLO);
    PacketU32(buffer, PROTOCOL_MAGIC);
    PacketU16(buffer, PROTOCOL_VERSION);
    PacketString(buffer, config->id);
    PacketString(buffer, filename);
    PacketU64(buffer, filesize);
    if (!SendPacket(socket, buffer)) {
        PrintError("Connection lost while greeting %s", recipient->addressstr);
        return FALSE;
    }

    MessageType type;
    char server[MAX_ID_SIZE] = { 0 };
    char message[MAX_LINE_SIZE] = { 0 };
    if (!ReceivePacket(socket, buffer, &type)) {
        PrintError("%s:%u closed the connection - it may not be a mailbox", recipient->addressstr, (unsigned int)recipient->port);
        return FALSE;
    }
    PacketReader reader = BeginReading(buffer);
    if (type == MSG_VERDICT) {
        uint8_t code = ReadU8(&reader);
        ReadU64(&reader);
        ReadU64(&reader);
        ReadString(&reader, message, sizeof(message));
        ReportVerdict(code, recipient->addressstr, filesize, 0, 0, message);
        return FALSE;
    }
    if (type != MSG_WELCOME) {
        PrintError("Unexpected reply from %s - it may not be a mailbox", recipient->addressstr);
        return FALSE;
    }
    ReadString(&reader, server, sizeof(server));
    uint8_t protected = ReadU8(&reader);
    uint8_t nonce[NONCE_SIZE];
    ReadBytes(&reader, nonce, sizeof(nonce));
    if (!FinishReading(&reader) || !ValidID(server)) {
        PrintError("Malformed greeting from %s", recipient->addressstr);
        return FALSE;
    }

    if (protected) {
        char password[MAX_PASSWORD_SIZE] = { 0 };
        if (!ObtainPassword(server, password, sizeof(password))) {
            PrintError("No password entered");
            return FALSE;
        }
        uint8_t digest[SHA256_SIZE];
        Sha256Password(nonce, sizeof(nonce), password, digest);
        memset(password, 0, sizeof(password));
        PacketBegin(buffer, MSG_AUTH);
        PacketBytes(buffer, digest, sizeof(digest));
        if (!SendPacket(socket, buffer)) {
            PrintError("Connection lost while authenticating");
            return FALSE;
        }
    }

    if (!ReceivePacket(socket, buffer, &type) || type != MSG_VERDICT) {
        PrintError("Connection to %s lost before the transfer was accepted", server);
        return FALSE;
    }
    reader = BeginReading(buffer);
    uint8_t code = ReadU8(&reader);
    uint64_t available = ReadU64(&reader);
    uint64_t limit = ReadU64(&reader);
    ReadString(&reader, message, sizeof(message));
    if (code != VERDICT_ACCEPT) {
        ReportVerdict(code, server, filesize, available, limit, message);
        return FALSE;
    }

    SetSocketTimeout(socket, TRANSFER_TIMEOUT_MS);
    uint8_t digest[SHA256_SIZE];
    if (!StreamFile(socket, filepath, filesize, digest)) return FALSE;
    PacketBegin(buffer, MSG_COMPLETE);
    PacketBytes(buffer, digest, sizeof(digest));
    if (!SendPacket(socket, buffer) ||
        !ReceivePacket(socket, buffer, &type) || type != MSG_RESULT) {
        PrintError("Connection to %s lost before delivery was confirmed", server);
        return FALSE;
    }
    reader = BeginReading(buffer);
    code = ReadU8(&reader);
    char saved[MAX_FILENAME_SIZE] = { 0 };
    ReadString(&reader, saved, sizeof(saved));
    if (code != VERDICT_ACCEPT) {
        ReportVerdict(code, server, filesize, 0, 0, saved);
        return FALSE;
    }

    if (!RecordContact(config->dns, server, recipient->addressstr, recipient->port)) {
        PrintWarn("Delivered, but unable to update DNS file \"%s\"", config->dns);
    }
    if (strcmp(saved, filename) == 0) {
        PrintSuccess("Delivered \"%s\" to %s", filename, server);
    } else {
        PrintSuccess("Delivered \"%s\" to %s (saved as \"%s\")", filename, server, saved);
    }
    return TRUE;
}

BOOL SendMail(const Config* config, const char* target, const char* filepath) {
    uint64_t filesize = 0;
    if (ez_directory_exists(filepath)) {
        PrintError("\"%s\" is a directory - only files can be sent", filepath);
        return FALSE;
    }
    if (!QueryFileSize(filepath, &filesize)) {
        PrintError("Unable to find file \"%s\"", filepath);
        return FALSE;
    }
    char filename[MAX_FILENAME_SIZE] = { 0 };
    if (!SanitizeFilename(PathBasename(filepath), filename, sizeof(filename))) {
        PrintError("\"%s\" has a file name that can't be sent", filepath);
        return FALSE;
    }
    Recipient recipient;
    if (!ResolveRecipient(config, target, &recipient)) return FALSE;

    char sizestr[32] = { 0 };
    FormatSize(sizestr, sizeof(sizestr), filesize);
    PrintNote("Sending \"%s\" (%s) to %s (%s:%u)...", filename, sizestr, recipient.host, recipient.addressstr, (unsigned int)recipient.port);

    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    ez_Client* client = EZ_GENERATE_CLIENT();
    BOOL success = FALSE;
    if (!EZ_CONNECT_CLIENT(client, recipient.address, recipient.port)) {
        PrintError("Unable to reach %s:%u - is their mailbox up?", recipient.addressstr, (unsigned int)recipient.port);
    } else {
        success = Deliver(config, client->socket, buffer, &recipient, filepath, filename, filesize);
    }
    EZ_CLEAN_CLIENT(client);
    EZ_CLEAN_BUFFER(buffer);
    return success;
}

BOOL RequestShutdown(const Config* config, const uint8_t* token) {
    ez_Ipv4 loopback = { { 127, 0, 0, 1 } };
    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    ez_Client* client = EZ_GENERATE_CLIENT();
    BOOL ok = FALSE;
    if (EZ_CONNECT_CLIENT(client, loopback, config->port)) {
        SetSocketTimeout(client->socket, 5000);
        PacketBegin(buffer, MSG_SHUTDOWN);
        PacketBytes(buffer, token, TOKEN_SIZE);
        MessageType type;
        if (SendPacket(client->socket, buffer) && ReceivePacket(client->socket, buffer, &type) && type == MSG_VERDICT) {
            PacketReader reader = BeginReading(buffer);
            ok = ReadU8(&reader) == VERDICT_ACCEPT;
        }
    }
    EZ_CLEAN_CLIENT(client);
    EZ_CLEAN_BUFFER(buffer);
    return ok;
}

static void SendVerdict(EZ_SOCKET socket, ez_Buffer* buffer, VerdictCode code, uint64_t available, uint64_t limit, const char* message) {
    PacketBegin(buffer, MSG_VERDICT);
    PacketU8(buffer, (uint8_t)code);
    PacketU64(buffer, available);
    PacketU64(buffer, limit);
    PacketString(buffer, message);
    SendPacket(socket, buffer);
}

static void SendResult(EZ_SOCKET socket, ez_Buffer* buffer, VerdictCode code, const char* text) {
    PacketBegin(buffer, MSG_RESULT);
    PacketU8(buffer, (uint8_t)code);
    PacketString(buffer, text);
    SendPacket(socket, buffer);
}

static BOOL ReceiveFile(EZ_SOCKET socket, const char* partpath, uint64_t filesize, uint8_t digest[SHA256_SIZE]) {
    FILE* file = fopen(partpath, "wb");
    if (file == NULL) return FALSE;
    uint8_t* chunk = EZ_ALLOC(CHUNK_SIZE, sizeof(uint8_t));
    Sha256 sha;
    Sha256Begin(&sha);
    uint64_t received = 0;
    BOOL ok = TRUE;
    while (received < filesize) {
        size_t want = (filesize - received) > CHUNK_SIZE ? CHUNK_SIZE : (size_t)(filesize - received);
        if (!ReceiveAll(socket, chunk, want) || fwrite(chunk, 1, want, file) != want) {
            ok = FALSE;
            break;
        }
        Sha256Feed(&sha, chunk, want);
        received += want;
    }
    Sha256Finish(&sha, digest);
    EZ_FREE(chunk);
    if (fclose(file) != 0) ok = FALSE;
    return ok;
}

static void Serve(const Config* config, EZ_SOCKET socket, ez_Buffer* buffer, const char* peer,
        const uint8_t* token, BOOL* shutdown, char* partpath, size_t partsize) {
    MessageType type;

    if (!ReceivePacket(socket, buffer, &type)) {
        LogEvent("Dropped connection from %s - no valid greeting", peer);
        return;
    }
    PacketReader reader = BeginReading(buffer);

    if (type == MSG_SHUTDOWN) {
        uint8_t given[TOKEN_SIZE];
        ReadBytes(&reader, given, sizeof(given));
        if (FinishReading(&reader) && strncmp(peer, "127.", 4) == 0 && DigestsMatch(given, token, TOKEN_SIZE)) {
            SendVerdict(socket, buffer, VERDICT_ACCEPT, 0, 0, "");
            *shutdown = TRUE;
        } else {
            LogEvent("Rejected an invalid shutdown request from %s", peer);
            SendVerdict(socket, buffer, VERDICT_INVALID, 0, 0, "invalid shutdown request");
        }
        return;
    }
    if (type != MSG_HELLO) {
        LogEvent("Dropped connection from %s - unexpected message %d", peer, (int)type);
        return;
    }

    uint32_t magic = ReadU32(&reader);
    uint16_t version = ReadU16(&reader);
    char sender[MAX_ID_SIZE] = { 0 };
    char requested[MAX_FILENAME_SIZE] = { 0 };
    ReadString(&reader, sender, sizeof(sender));
    ReadString(&reader, requested, sizeof(requested));
    uint64_t filesize = ReadU64(&reader);
    if (magic != PROTOCOL_MAGIC) {
        LogEvent("Dropped connection from %s - not a mailbox client", peer);
        return;
    }
    if (version != PROTOCOL_VERSION) {
        SendVerdict(socket, buffer, VERDICT_VERSION, 0, 0, "incompatible protocol version");
        LogEvent("Rejected %s - protocol version %u", peer, (unsigned int)version);
        return;
    }
    char filename[MAX_FILENAME_SIZE] = { 0 };
    if (!FinishReading(&reader) || !ValidID(sender) || !SanitizeFilename(requested, filename, sizeof(filename))) {
        SendVerdict(socket, buffer, VERDICT_INVALID, 0, 0, "malformed request (bad sender ID or file name)");
        LogEvent("Rejected malformed request from %s", peer);
        return;
    }

    uint8_t nonce[NONCE_SIZE] = { 0 };
    if (!RandomBytes(nonce, sizeof(nonce))) {
        SendVerdict(socket, buffer, VERDICT_FAILED, 0, 0, "receiver could not generate a challenge");
        return;
    }
    PacketBegin(buffer, MSG_WELCOME);
    PacketString(buffer, config->id);
    PacketU8(buffer, config->protected ? 1 : 0);
    PacketBytes(buffer, nonce, sizeof(nonce));
    if (!SendPacket(socket, buffer)) return;

    if (config->protected) {
        if (!ReceivePacket(socket, buffer, &type) || type != MSG_AUTH) {
            LogEvent("%s (%s) disconnected during authentication", sender, peer);
            return;
        }
        reader = BeginReading(buffer);
        uint8_t given[SHA256_SIZE];
        uint8_t expected[SHA256_SIZE];
        ReadBytes(&reader, given, sizeof(given));
        Sha256Password(nonce, sizeof(nonce), config->password, expected);
        if (!FinishReading(&reader) || !DigestsMatch(given, expected, SHA256_SIZE)) {
            SendVerdict(socket, buffer, VERDICT_BAD_PASSWORD, 0, 0, "incorrect password");
            LogEvent("Rejected \"%s\" from %s (%s) - incorrect password", filename, sender, peer);
            return;
        }
    }

    uint64_t used = DirectoryUsage(config->inbox);
    uint64_t available = used >= config->inboxsize ? 0 : config->inboxsize - used;
    if (filesize > available) {
        SendVerdict(socket, buffer, VERDICT_NO_SPACE, available, config->inboxsize, "not enough space");
        LogEvent("Rejected \"%s\" from %s (%s) - %llu bytes needed, %llu free", filename, sender, peer,
            (unsigned long long)filesize, (unsigned long long)available);
        return;
    }

    if (!JoinPath(partpath, partsize, config->inbox, ".incoming" PART_EXTENSION)) {
        SendVerdict(socket, buffer, VERDICT_FAILED, 0, 0, "receiver path too long");
        return;
    }
    SendVerdict(socket, buffer, VERDICT_ACCEPT, available, config->inboxsize, "");
    SetSocketTimeout(socket, TRANSFER_TIMEOUT_MS);

    uint8_t digest[SHA256_SIZE];
    if (!ReceiveFile(socket, partpath, filesize, digest)) {
        LogEvent("Transfer of \"%s\" from %s (%s) failed midway", filename, sender, peer);
        RemoveFile(partpath);
        partpath[0] = '\0';
        return;
    }
    if (!ReceivePacket(socket, buffer, &type) || type != MSG_COMPLETE) {
        LogEvent("Transfer of \"%s\" from %s (%s) was not confirmed", filename, sender, peer);
        RemoveFile(partpath);
        partpath[0] = '\0';
        return;
    }
    reader = BeginReading(buffer);
    uint8_t claimed[SHA256_SIZE];
    ReadBytes(&reader, claimed, sizeof(claimed));
    if (!FinishReading(&reader) || !DigestsMatch(claimed, digest, SHA256_SIZE)) {
        SendResult(socket, buffer, VERDICT_CORRUPT, "checksum mismatch");
        LogEvent("Discarded \"%s\" from %s (%s) - checksum mismatch", filename, sender, peer);
        RemoveFile(partpath);
        partpath[0] = '\0';
        return;
    }

    char finalname[MAX_FILENAME_SIZE] = { 0 };
    char finalpath[MAX_PATH_SIZE] = { 0 };
    if (!UniqueFilename(config->inbox, filename, finalname, sizeof(finalname)) ||
        !JoinPath(finalpath, sizeof(finalpath), config->inbox, finalname) ||
        !MoveReplace(partpath, finalpath)) {
        SendResult(socket, buffer, VERDICT_FAILED, "receiver could not store the file");
        LogEvent("Unable to store \"%s\" from %s (%s)", filename, sender, peer);
        RemoveFile(partpath);
        partpath[0] = '\0';
        return;
    }
    partpath[0] = '\0';

    Mail mail = { 0 };
    mail.read = FALSE;
    mail.timestamp = (int64_t)time(NULL);
    mail.size = filesize;
    strcpy(mail.sender, sender);
    snprintf(mail.address, sizeof(mail.address), "%s", peer);
    strcpy(mail.filename, finalname);
    if (!RecordMail(config->state, &mail)) {
        LogEvent("Received \"%s\" but could not update state file \"%s\"", finalname, config->state);
    }
    SendResult(socket, buffer, VERDICT_ACCEPT, finalname);
    LogEvent("Received \"%s\" (%llu bytes) from %s (%s)", finalname, (unsigned long long)filesize, sender, peer);
}

void ReceiveMail(const Config* config, EZ_SOCKET socket, const uint8_t* token, BOOL* shutdown) {
    char peer[16] = { 0 };
    PeerAddress(socket, peer, sizeof(peer));
    SetSocketTimeout(socket, HANDSHAKE_TIMEOUT_MS);
    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    char partpath[MAX_PATH_SIZE] = { 0 };
    Serve(config, socket, buffer, peer, token, shutdown, partpath, sizeof(partpath));
    if (partpath[0] != '\0' && ez_file_exists(partpath)) RemoveFile(partpath);
    EZ_CLEAN_BUFFER(buffer);
}

BOOL SanitizeFilename(const char* name, char* out, size_t size) {
    const char* base = name;
    for (const char* c = name; *c; c++) {
        if (*c == '/' || *c == '\\') base = c + 1;
    }
    size_t n = 0;
    for (const char* c = base; *c && n < size - 1; c++) {
        unsigned char u = (unsigned char)*c;
        if (u < 0x20 || u == 0x7F) return FALSE;
        if (strchr("<>:\"|?*", *c) != NULL) out[n++] = '_';
        else out[n++] = *c;
    }
    out[n] = '\0';
    if (base[n] != '\0') return FALSE;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = '\0';
    if (n == 0) return FALSE;
    size_t extlen = strlen(PART_EXTENSION);
    if (n >= extlen && strcmp(out + n - extlen, PART_EXTENSION) == 0) {
        if (n + 1 >= size) return FALSE;
        out[n++] = '_';
        out[n] = '\0';
    }
    static const char* reserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6",
        "LPT7", "LPT8", "LPT9" };
    char stem[MAX_FILENAME_SIZE] = { 0 };
    size_t s = 0;
    while (out[s] != '\0' && out[s] != '.' && s < sizeof(stem) - 1) {
        stem[s] = (char)toupper((unsigned char)out[s]);
        s++;
    }
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(stem, reserved[i]) == 0) {
            if (n + 1 >= size) return FALSE;
            memmove(out + 1, out, n + 1);
            out[0] = '_';
            break;
        }
    }
    return TRUE;
}

BOOL UniqueFilename(const char* directory, const char* name, char* out, size_t size) {
    char path[MAX_PATH_SIZE] = { 0 };
    if (!JoinPath(path, sizeof(path), directory, name)) return FALSE;
    if (!ez_file_exists(path) && !ez_directory_exists(path)) {
        return snprintf(out, size, "%s", name) < (int)size;
    }
    const char* dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    size_t stemlen = dot ? (size_t)(dot - name) : strlen(name);
    const char* extension = dot ? dot : "";
    for (int copy = 1; copy < MAX_COPY_INDEX; copy++) {
        int written = snprintf(out, size, "%.*s (%d)%s", (int)stemlen, name, copy, extension);
        if (written < 0 || (size_t)written >= size) return FALSE;
        if (!JoinPath(path, sizeof(path), directory, out)) return FALSE;
        if (!ez_file_exists(path) && !ez_directory_exists(path)) return TRUE;
    }
    return FALSE;
}
