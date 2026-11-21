#define _FILE_OFFSET_BITS 64
#include "transfer.h"
#include "core/platform.h"
#include "core/wire.h"
#include "core/daemon.h"
#include "data/dns.h"
#include "data/state.h"
#include "data/history.h"
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
#define RELAY_HANDSHAKE_TIMEOUT_MS 15000
#define STATUS_TIMEOUT_MS 10000

typedef struct {
    char host[MAX_LINE_SIZE];
    ez_Ipv4 address;
    char addressstr[16];
    uint16_t port;
    BOOL proxy;
} Recipient;

typedef enum {
    STREAM_OK = 0,
    STREAM_LOCAL,
    STREAM_LOST
} StreamResult;

static void CopyField(char* out, size_t size, const char* str) {
    size_t n = 0;
    for (; str[n] != '\0' && n < size - 1; n++) {
        out[n] = (str[n] == '\t' || str[n] == '\n' || str[n] == '\r') ? ' ' : str[n];
    }
    out[n] = '\0';
}

BOOL LookupRoute(const Config* config, const char* id, Route* route) {
    memset(route, 0, sizeof(Route));
    ARRLIST_Contact contacts = { 0 };
    BOOL found = FALSE;
    // known machines first - a direct hop always beats a proxy
    if (LoadDNS(config->dns, &contacts)) {
        Contact* contact = FindContact(&contacts, id);
        if (contact != NULL && ParseAddress(contact->address, &route->address)) {
            route->port = contact->port != 0 ? contact->port : config->port;
            route->proxy = FALSE;
            found = TRUE;
        }
    }
    if (!found && LoadDNS(config->proxypath, &contacts)) {
        Contact* contact = FindContact(&contacts, id);
        if (contact != NULL && ParseAddress(contact->address, &route->address)) {
            route->port = contact->port != 0 ? contact->port : config->port;
            route->proxy = TRUE;
            found = TRUE;
        }
    }
    CleanDNS(&contacts);
    if (found) FormatAddress(route->address, route->addressstr, sizeof(route->addressstr));
    return found;
}

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
    Route route;
    if (ParseAddress(recipient->host, &recipient->address)) {
        // raw ip
    } else if (LookupRoute(config, recipient->host, &route)) {
        recipient->address = route.address;
        recipient->port = route.port;
        recipient->proxy = route.proxy;
    } else if (!ResolveHost(recipient->host, &recipient->address)) {
        PrintError("Unknown recipient \"%s\" - it isn't an IP address, an ID in your DNS (%s) or proxy (%s) files, or a resolvable hostname",
            recipient->host, config->dns, config->proxypath);
        return FALSE;
    }
    if (recipient->proxy && !ValidID(recipient->host)) {
        PrintError("\"%s\" is not a valid mailbox ID", recipient->host);
        return FALSE;
    }
    if (explicit_port != 0 && !recipient->proxy) recipient->port = explicit_port;
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

void VerdictReason(uint8_t code, const char* message, char* out, size_t size) {
    const char* fallback = "rejected the transfer";
    switch (code) {
        case VERDICT_BAD_PASSWORD: fallback = "incorrect password"; break;
        case VERDICT_NO_SPACE: fallback = "not enough space"; break;
        case VERDICT_VERSION: fallback = "incompatible mailbox version"; break;
        case VERDICT_CORRUPT: fallback = "data was corrupted in transit"; break;
        case VERDICT_NOT_PROXY: fallback = "does not relay mail"; break;
        case VERDICT_NO_ROUTE: fallback = "no route to the recipient"; break;
        case VERDICT_TOO_MANY_HOPS: fallback = "too many proxy hops"; break;
        default: break;
    }
    snprintf(out, size, "%s", (message != NULL && message[0] != '\0') ? message : fallback);
}

static void ReportVerdict(uint8_t code, const char* server, BOOL relay, uint64_t filesize, uint64_t available, uint64_t limit, const char* message) {
    char need[32] = { 0 }, free_space[32] = { 0 }, cap[32] = { 0 };
    char reason[MAX_DETAIL_SIZE - MAX_ID_SIZE - 2] = { 0 };
    switch (code) {
        case VERDICT_BAD_PASSWORD:
            PrintError("Incorrect password for %s's mailbox - transfer rejected", server);
            break;
        case VERDICT_NO_SPACE:
            FormatSize(need, sizeof(need), filesize);
            FormatSize(free_space, sizeof(free_space), available);
            FormatSize(cap, sizeof(cap), limit);
            PrintError("Not enough room in %s's %s - the file needs %s but only %s of %s is free",
                server, relay ? "proxy storage" : "mailbox", need, free_space, cap);
            break;
        case VERDICT_VERSION:
            PrintError("%s is running an incompatible version of mailbox", server);
            break;
        case VERDICT_CORRUPT:
            PrintError("%s received corrupted data - transfer discarded, please try again", server);
            break;
        default:
            VerdictReason(code, message, reason, sizeof(reason));
            PrintError("%s rejected the transfer - %s", server, reason);
            break;
    }
}

static SentStatus VerdictStatus(uint8_t code) {
    switch (code) {
        case VERDICT_ACCEPT: return SENT_DELIVERED;
        case VERDICT_BAD_PASSWORD: return SENT_BAD_PASSWORD;
        case VERDICT_NO_SPACE: return SENT_NO_SPACE;
        case VERDICT_CORRUPT: return SENT_FAILED;
        default: return SENT_REJECTED;
    }
}

static StreamResult StreamFile(EZ_SOCKET socket, const char* filepath, uint64_t filesize, BOOL interactive, uint8_t digest[SHA256_SIZE]) {
    FILE* file = fopen(filepath, "rb");
    if (file == NULL) return STREAM_LOCAL;
    uint8_t* chunk = EZ_ALLOC(CHUNK_SIZE, sizeof(uint8_t));
    Sha256 sha;
    Sha256Begin(&sha);
    Progress progress;
    if (interactive) BeginProgress(&progress, filesize);
    uint64_t sent = 0;
    StreamResult result = STREAM_OK;
    while (sent < filesize) {
        size_t want = (filesize - sent) > CHUNK_SIZE ? CHUNK_SIZE : (size_t)(filesize - sent);
        size_t got = fread(chunk, 1, want, file);
        if (got != want) {
            result = STREAM_LOCAL;
            break;
        }
        Sha256Feed(&sha, chunk, got);
        if (!SendAll(socket, chunk, got)) {
            result = STREAM_LOST;
            break;
        }
        sent += got;
        if (interactive) UpdateProgress(&progress, sent);
    }
    if (interactive) EndProgress(&progress);
    Sha256Finish(&sha, digest);
    EZ_FREE(chunk);
    fclose(file);
    return result;
}

static TransmitResult Converse(EZ_SOCKET socket, ez_Buffer* buffer, const Transmission* t, TransmitOutcome* o, const char* addressstr) {
    SetSocketTimeout(socket, t->interactive ? HANDSHAKE_TIMEOUT_MS : RELAY_HANDSHAKE_TIMEOUT_MS);
    PacketBegin(buffer, MSG_HELLO);
    PacketU32(buffer, PROTOCOL_MAGIC);
    PacketU16(buffer, PROTOCOL_VERSION);
    PacketString(buffer, t->sender);
    PacketString(buffer, t->filename);
    PacketU64(buffer, t->size);
    PacketU8(buffer, t->proxy ? DELIVERY_PROXY : DELIVERY_DIRECT);
    if (t->proxy) {
        PacketString(buffer, t->target);
        PacketString(buffer, t->tracking);
        PacketU16(buffer, t->replyport);
        PacketU8(buffer, t->hops);
    }
    if (!SendPacket(socket, buffer)) {
        snprintf(o->message, sizeof(o->message), "connection to %s lost while greeting", addressstr);
        return TRANSMIT_LOST;
    }

    MessageType type;
    if (!ReceivePacket(socket, buffer, &type)) {
        snprintf(o->message, sizeof(o->message), "%s closed the connection - it may not be a mailbox", addressstr);
        return TRANSMIT_LOST;
    }
    PacketReader reader = BeginReading(buffer);
    if (type == MSG_VERDICT) {
        char id[MAX_ID_SIZE] = { 0 };
        o->verdict = ReadU8(&reader);
        o->available = ReadU64(&reader);
        o->limit = ReadU64(&reader);
        ReadString(&reader, o->message, sizeof(o->message));
        ReadString(&reader, id, sizeof(id));
        snprintf(o->server, sizeof(o->server), "%s", ValidID(id) ? id : addressstr);
        return TRANSMIT_REJECTED;
    }
    if (type != MSG_WELCOME) {
        snprintf(o->message, sizeof(o->message), "unexpected reply from %s - it may not be a mailbox", addressstr);
        return TRANSMIT_LOST;
    }
    ReadString(&reader, o->server, sizeof(o->server));
    uint8_t protected = ReadU8(&reader);
    uint8_t nonce[NONCE_SIZE];
    ReadBytes(&reader, nonce, sizeof(nonce));
    if (!FinishReading(&reader) || !ValidID(o->server)) {
        snprintf(o->server, sizeof(o->server), "%s", addressstr);
        snprintf(o->message, sizeof(o->message), "malformed greeting from %s", addressstr);
        return TRANSMIT_LOST;
    }

    if (protected) {
        if (!t->interactive) return TRANSMIT_PASSWORD;
        char password[MAX_PASSWORD_SIZE] = { 0 };
        if (!ObtainPassword(o->server, password, sizeof(password))) {
            snprintf(o->message, sizeof(o->message), "no password entered");
            return TRANSMIT_LOCAL;
        }
        uint8_t digest[SHA256_SIZE];
        Sha256Password(nonce, sizeof(nonce), password, digest);
        memset(password, 0, sizeof(password));
        PacketBegin(buffer, MSG_AUTH);
        PacketBytes(buffer, digest, sizeof(digest));
        if (!SendPacket(socket, buffer)) {
            snprintf(o->message, sizeof(o->message), "connection lost while authenticating");
            return TRANSMIT_LOST;
        }
    }

    if (!ReceivePacket(socket, buffer, &type) || type != MSG_VERDICT) {
        snprintf(o->message, sizeof(o->message), "connection to %s lost before the transfer was accepted", o->server);
        return TRANSMIT_LOST;
    }
    reader = BeginReading(buffer);
    o->verdict = ReadU8(&reader);
    o->available = ReadU64(&reader);
    o->limit = ReadU64(&reader);
    ReadString(&reader, o->message, sizeof(o->message));
    if (o->verdict == VERDICT_DUPLICATE) return TRANSMIT_RELAYED;
    if (o->verdict != VERDICT_ACCEPT) return TRANSMIT_REJECTED;
    o->message[0] = '\0';

    SetSocketTimeout(socket, TRANSFER_TIMEOUT_MS);
    uint8_t digest[SHA256_SIZE];
    StreamResult streamed = StreamFile(socket, t->filepath, t->size, t->interactive, digest);
    if (streamed == STREAM_LOCAL) {
        snprintf(o->message, sizeof(o->message), "unable to read \"%s\" - did it change while sending?", t->filepath);
        return TRANSMIT_LOCAL;
    }
    if (streamed == STREAM_LOST) {
        snprintf(o->message, sizeof(o->message), "connection to %s lost while sending", o->server);
        return TRANSMIT_LOST;
    }
    PacketBegin(buffer, MSG_COMPLETE);
    PacketBytes(buffer, digest, sizeof(digest));
    if (!SendPacket(socket, buffer) ||
        !ReceivePacket(socket, buffer, &type) || type != MSG_RESULT) {
        snprintf(o->message, sizeof(o->message), "connection to %s lost before delivery was confirmed", o->server);
        return TRANSMIT_LOST;
    }
    reader = BeginReading(buffer);
    uint8_t code = ReadU8(&reader);
    ReadString(&reader, o->saved, sizeof(o->saved));
    uint8_t final = ReadU8(&reader);
    if (code != VERDICT_ACCEPT) {
        o->verdict = code;
        snprintf(o->message, sizeof(o->message), "%.*s", (int)sizeof(o->message) - 1, o->saved);
        o->saved[0] = '\0';
        return TRANSMIT_REJECTED;
    }
    return final ? TRANSMIT_DELIVERED : TRANSMIT_RELAYED;
}

TransmitResult Transmit(const Transmission* transmission, TransmitOutcome* outcome) {
    const Transmission* t = transmission;
    TransmitOutcome* o = outcome;
    memset(o, 0, sizeof(TransmitOutcome));
    char addressstr[16] = { 0 };
    FormatAddress(t->address, addressstr, sizeof(addressstr));
    snprintf(o->server, sizeof(o->server), "%s", addressstr);
    EZ_SOCKET socket = ConnectQuick(t->address, t->port, t->interactive ? CONNECT_TIMEOUT_MS : RELAY_CONNECT_TIMEOUT_MS);
    if (socket == EZ_INVALID_SOCK) {
        snprintf(o->message, sizeof(o->message), "unable to reach %s:%u", addressstr, (unsigned int)t->port);
        return TRANSMIT_UNREACHABLE;
    }
    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    TransmitResult result = Converse(socket, buffer, t, o, addressstr);
    EZ_CLEAN_BUFFER(buffer);
    CloseSocket(socket);
    return result;
}

BOOL SendStatus(const char* address, uint16_t port, const char* tracking, uint8_t outcome, const char* detail, const char* saved) {
    ez_Ipv4 ip;
    if (!ParseAddress(address, &ip)) return FALSE;
    EZ_SOCKET socket = ConnectQuick(ip, port, RELAY_CONNECT_TIMEOUT_MS);
    if (socket == EZ_INVALID_SOCK) return FALSE;
    SetSocketTimeout(socket, STATUS_TIMEOUT_MS);
    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    PacketBegin(buffer, MSG_STATUS);
    PacketString(buffer, tracking);
    PacketU8(buffer, outcome);
    PacketString(buffer, detail);
    PacketString(buffer, saved);
    BOOL ok = FALSE;
    MessageType type;
    if (SendPacket(socket, buffer) && ReceivePacket(socket, buffer, &type) && type == MSG_VERDICT) {
        PacketReader reader = BeginReading(buffer);
        ok = ReadU8(&reader) == VERDICT_ACCEPT;
    }
    EZ_CLEAN_BUFFER(buffer);
    CloseSocket(socket);
    return ok;
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
    char tracking[TRACKING_SIZE + 1] = { 0 };
    if (recipient.proxy && !GenerateTracking(tracking, sizeof(tracking))) {
        PrintError("Unable to generate a tracking ID");
        return FALSE;
    }

    char sizestr[32] = { 0 };
    FormatSize(sizestr, sizeof(sizestr), filesize);
    if (recipient.proxy) {
        PrintNote("Sending \"%s\" (%s) to %s through proxy %s:%u...", filename, sizestr, recipient.host,
            recipient.addressstr, (unsigned int)recipient.port);
    } else {
        PrintNote("Sending \"%s\" (%s) to %s (%s:%u)...", filename, sizestr, recipient.host,
            recipient.addressstr, (unsigned int)recipient.port);
    }

    Transmission transmission = { 0 };
    transmission.sender = config->id;
    transmission.filepath = filepath;
    transmission.filename = filename;
    transmission.size = filesize;
    transmission.address = recipient.address;
    transmission.port = recipient.port;
    transmission.proxy = recipient.proxy;
    transmission.target = recipient.host;
    transmission.tracking = tracking;
    transmission.replyport = config->port;
    transmission.hops = 1;
    transmission.interactive = TRUE;
    TransmitOutcome outcome;
    TransmitResult result = Transmit(&transmission, &outcome);

    Sent sent = { 0 };
    sent.timestamp = (int64_t)time(NULL);
    sent.size = filesize;
    sent.status = SENT_FAILED;
    sent.port = recipient.port;
    CopyField(sent.recipient, sizeof(sent.recipient), recipient.proxy ? recipient.host :
        (ValidID(outcome.server) ? outcome.server : recipient.host));
    CopyField(sent.address, sizeof(sent.address), recipient.addressstr);
    CopyField(sent.filename, sizeof(sent.filename), filename);
    CopyField(sent.tracking, sizeof(sent.tracking), tracking);
    if (recipient.proxy) CopyField(sent.via, sizeof(sent.via), outcome.server);

    char reason[MAX_DETAIL_SIZE - MAX_ID_SIZE - 2] = { 0 };
    switch (result) {
        case TRANSMIT_DELIVERED:
            if (!recipient.proxy && !RecordContact(config->dns, outcome.server, recipient.addressstr, recipient.port)) {
                PrintWarn("Delivered, but unable to update DNS file \"%s\"", config->dns);
            }
            if (strcmp(outcome.saved, filename) == 0) PrintSuccess("Delivered \"%s\" to %s", filename, sent.recipient);
            else PrintSuccess("Delivered \"%s\" to %s (saved as \"%s\")", filename, sent.recipient, outcome.saved);
            sent.status = SENT_DELIVERED;
            CopyField(sent.saved, sizeof(sent.saved), outcome.saved);
            break;
        case TRANSMIT_RELAYED: {
            PrintSuccess("Handed \"%s\" to proxy %s for delivery to %s - follow it with \"mailbox history\"",
                filename, outcome.server, recipient.host);
            sent.status = SENT_DELIVERING;
            snprintf(sent.detail, sizeof(sent.detail), "held by %s", outcome.server);
            Runtime runtime;
            if (!DaemonRunning(config, &runtime)) {
                PrintWarn("Your mailbox is down - run \"mailbox up\" so the delivery result can reach you");
            }
            break;
        }
        case TRANSMIT_REJECTED:
            ReportVerdict(outcome.verdict, outcome.server, recipient.proxy, filesize, outcome.available, outcome.limit, outcome.message);
            sent.status = VerdictStatus(outcome.verdict);
            VerdictReason(outcome.verdict, outcome.message, reason, sizeof(reason));
            snprintf(sent.detail, sizeof(sent.detail), "%s: %s", outcome.server, reason);
            break;
        case TRANSMIT_UNREACHABLE:
            PrintError("Unable to reach %s:%u - is their mailbox up?", recipient.addressstr, (unsigned int)recipient.port);
            sent.status = SENT_UNREACHABLE;
            break;
        default:
            PrintError("%s", outcome.message[0] ? outcome.message : "transfer failed");
            sent.status = SENT_FAILED;
            snprintf(sent.detail, sizeof(sent.detail), "%s", outcome.message);
            break;
    }
    if (!RecordSent(config->history, &sent)) {
        PrintWarn("Unable to update history file \"%s\"", config->history);
    }
    return result == TRANSMIT_DELIVERED || result == TRANSMIT_RELAYED;
}

BOOL RequestShutdown(const Config* config, const uint8_t* token) {
    ez_Ipv4 loopback = { { 127, 0, 0, 1 } };
    EZ_SOCKET socket = ConnectQuick(loopback, config->port, RELAY_CONNECT_TIMEOUT_MS);
    if (socket == EZ_INVALID_SOCK) return FALSE;
    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    BOOL ok = FALSE;
    SetSocketTimeout(socket, 5000);
    PacketBegin(buffer, MSG_SHUTDOWN);
    PacketBytes(buffer, token, TOKEN_SIZE);
    MessageType type;
    if (SendPacket(socket, buffer) && ReceivePacket(socket, buffer, &type) && type == MSG_VERDICT) {
        PacketReader reader = BeginReading(buffer);
        ok = ReadU8(&reader) == VERDICT_ACCEPT;
    }
    EZ_CLEAN_BUFFER(buffer);
    CloseSocket(socket);
    return ok;
}

static void SendVerdict(EZ_SOCKET socket, ez_Buffer* buffer, const char* id, VerdictCode code, uint64_t available, uint64_t limit, const char* message) {
    PacketBegin(buffer, MSG_VERDICT);
    PacketU8(buffer, (uint8_t)code);
    PacketU64(buffer, available);
    PacketU64(buffer, limit);
    PacketString(buffer, message);
    PacketString(buffer, id);
    SendPacket(socket, buffer);
}

static void SendResult(EZ_SOCKET socket, ez_Buffer* buffer, VerdictCode code, const char* text, BOOL final) {
    PacketBegin(buffer, MSG_RESULT);
    PacketU8(buffer, (uint8_t)code);
    PacketString(buffer, text);
    PacketU8(buffer, final ? 1 : 0);
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

static void ServeStatus(Relay* relay, EZ_SOCKET socket, ez_Buffer* buffer, PacketReader* reader, const char* peer) {
    const Config* config = relay->config;
    char tracking[TRACKING_SIZE + 2] = { 0 };
    char detail[MAX_DETAIL_SIZE] = { 0 };
    char saved[MAX_FILENAME_SIZE] = { 0 };
    ReadString(reader, tracking, sizeof(tracking));
    uint8_t outcome = ReadU8(reader);
    ReadString(reader, detail, sizeof(detail));
    ReadString(reader, saved, sizeof(saved));
    if (!FinishReading(reader) || !ValidTracking(tracking) || outcome > OUTCOME_FAILED) {
        LogEvent("Dropped a malformed status report from %s", peer);
        SendVerdict(socket, buffer, config->id, VERDICT_INVALID, 0, 0, "malformed status report");
        return;
    }
    HandleStatus(relay, tracking, outcome, detail, saved);
    SendVerdict(socket, buffer, config->id, VERDICT_ACCEPT, 0, 0, "");
}

static void Serve(const Config* config, Relay* relay, EZ_SOCKET socket, ez_Buffer* buffer, const char* peer,
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
            SendVerdict(socket, buffer, config->id, VERDICT_ACCEPT, 0, 0, "");
            *shutdown = TRUE;
        } else {
            LogEvent("Rejected an invalid shutdown request from %s", peer);
            SendVerdict(socket, buffer, config->id, VERDICT_INVALID, 0, 0, "invalid shutdown request");
        }
        return;
    }
    if (type == MSG_STATUS) {
        ServeStatus(relay, socket, buffer, &reader, peer);
        return;
    }
    if (type != MSG_HELLO) {
        LogEvent("Dropped connection from %s - unexpected message %d", peer, (int)type);
        return;
    }

    uint32_t magic = ReadU32(&reader);
    uint16_t version = ReadU16(&reader);
    if (magic != PROTOCOL_MAGIC) {
        LogEvent("Dropped connection from %s - not a mailbox client", peer);
        return;
    }
    if (version != PROTOCOL_VERSION) {
        SendVerdict(socket, buffer, config->id, VERDICT_VERSION, 0, 0, "incompatible protocol version");
        LogEvent("Rejected %s - protocol version %u", peer, (unsigned int)version);
        return;
    }
    char sender[MAX_ID_SIZE] = { 0 };
    char requested[MAX_FILENAME_SIZE] = { 0 };
    char target[MAX_ID_SIZE] = { 0 };
    char tracking[TRACKING_SIZE + 2] = { 0 };
    ReadString(&reader, sender, sizeof(sender));
    ReadString(&reader, requested, sizeof(requested));
    uint64_t filesize = ReadU64(&reader);
    uint8_t kind = ReadU8(&reader);
    uint16_t replyport = 0;
    uint8_t hops = 0;
    if (kind == DELIVERY_PROXY) {
        ReadString(&reader, target, sizeof(target));
        ReadString(&reader, tracking, sizeof(tracking));
        replyport = ReadU16(&reader);
        hops = ReadU8(&reader);
    }
    char filename[MAX_FILENAME_SIZE] = { 0 };
    BOOL valid = FinishReading(&reader) && ValidID(sender) && SanitizeFilename(requested, filename, sizeof(filename)) && kind <= DELIVERY_PROXY;
    if (valid && kind == DELIVERY_PROXY) valid = ValidID(target) && ValidTracking(tracking) && replyport >= 1024;
    if (!valid) {
        SendVerdict(socket, buffer, config->id, VERDICT_INVALID, 0, 0, "malformed request (bad sender ID, file name or proxy header)");
        LogEvent("Rejected malformed request from %s", peer);
        return;
    }

    // a proxy deliverable addressed to us is just mail - only relay it if it's for someone else
    BOOL relaying = kind == DELIVERY_PROXY && strcmp(target, config->id) != 0;
    char message[MAX_DETAIL_SIZE] = { 0 };
    if (relaying) {
        Route route;
        if (!config->proxy) {
            SendVerdict(socket, buffer, config->id, VERDICT_NOT_PROXY, 0, 0, "does not relay mail (PROXY is off)");
            LogEvent("Refused to relay \"%s\" from %s (%s) to %s - PROXY is off", filename, sender, peer, target);
            return;
        }
        if (hops > MAX_HOPS) {
            SendVerdict(socket, buffer, config->id, VERDICT_TOO_MANY_HOPS, 0, 0, "too many proxy hops - check for a proxy loop");
            LogEvent("Refused to relay \"%s\" from %s (%s) to %s - %u hops", filename, sender, peer, target, (unsigned int)hops);
            return;
        }
        if (RelayHasJob(relay, tracking)) {
            SendVerdict(socket, buffer, config->id, VERDICT_DUPLICATE, 0, 0, "already relaying this delivery");
            return;
        }
        if (!LookupRoute(config, target, &route)) {
            snprintf(message, sizeof(message), "no route to %s", target);
            SendVerdict(socket, buffer, config->id, VERDICT_NO_ROUTE, 0, 0, message);
            LogEvent("Refused to relay \"%s\" from %s (%s) - no route to %s", filename, sender, peer, target);
            return;
        }
    }

    uint8_t nonce[NONCE_SIZE] = { 0 };
    if (!RandomBytes(nonce, sizeof(nonce))) {
        SendVerdict(socket, buffer, config->id, VERDICT_FAILED, 0, 0, "receiver could not generate a challenge");
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
            SendVerdict(socket, buffer, config->id, VERDICT_BAD_PASSWORD, 0, 0, "incorrect password");
            LogEvent("Rejected \"%s\" from %s (%s) - incorrect password", filename, sender, peer);
            return;
        }
    }

    const char* directory = relaying ? config->proxydir : config->inbox;
    uint64_t limit = relaying ? config->proxydirsize : config->inboxsize;
    uint64_t used = DirectoryUsage(directory);
    uint64_t available = used >= limit ? 0 : limit - used;
    if (filesize > available) {
        SendVerdict(socket, buffer, config->id, VERDICT_NO_SPACE, available, limit, relaying ? "proxy storage full" : "mailbox full");
        LogEvent("Rejected \"%s\" from %s (%s) - %llu bytes needed, %llu free in %s", filename, sender, peer,
            (unsigned long long)filesize, (unsigned long long)available, relaying ? "proxy storage" : "mailbox");
        return;
    }

    BOOL pathed = relaying ? RelayStoragePath(relay, tracking, TRUE, partpath, partsize)
                           : JoinPath(partpath, partsize, config->inbox, ".incoming" PART_EXTENSION);
    if (!pathed) {
        partpath[0] = '\0';
        SendVerdict(socket, buffer, config->id, VERDICT_FAILED, 0, 0, "receiver path too long");
        return;
    }
    SendVerdict(socket, buffer, config->id, VERDICT_ACCEPT, available, limit, "");
    SetSocketTimeout(socket, TRANSFER_TIMEOUT_MS);

    uint8_t digest[SHA256_SIZE];
    if (!ReceiveFile(socket, partpath, filesize, digest)) {
        LogEvent("Transfer of \"%s\" from %s (%s) failed midway", filename, sender, peer);
        return;
    }
    if (!ReceivePacket(socket, buffer, &type) || type != MSG_COMPLETE) {
        LogEvent("Transfer of \"%s\" from %s (%s) was not confirmed", filename, sender, peer);
        return;
    }
    reader = BeginReading(buffer);
    uint8_t claimed[SHA256_SIZE];
    ReadBytes(&reader, claimed, sizeof(claimed));
    if (!FinishReading(&reader) || !DigestsMatch(claimed, digest, SHA256_SIZE)) {
        SendResult(socket, buffer, VERDICT_CORRUPT, "checksum mismatch", FALSE);
        LogEvent("Discarded \"%s\" from %s (%s) - checksum mismatch", filename, sender, peer);
        return;
    }

    if (relaying) {
        char stored[MAX_PATH_SIZE] = { 0 };
        if (!RelayStoragePath(relay, tracking, FALSE, stored, sizeof(stored)) || !MoveReplace(partpath, stored)) {
            SendResult(socket, buffer, VERDICT_FAILED, "proxy could not store the file", FALSE);
            LogEvent("Unable to store relayed \"%s\" from %s (%s)", filename, sender, peer);
            return;
        }
        partpath[0] = '\0';
        RelayJob job = { 0 };
        job.kind = JOB_FORWARD;
        snprintf(job.tracking, sizeof(job.tracking), "%.*s", TRACKING_SIZE, tracking);
        snprintf(job.origin, sizeof(job.origin), "%s", sender);
        snprintf(job.target, sizeof(job.target), "%s", target);
        snprintf(job.upstream, sizeof(job.upstream), "%s", peer);
        job.upstreamport = replyport;
        snprintf(job.filename, sizeof(job.filename), "%s", filename);
        job.size = filesize;
        job.hops = hops;
        job.due = (int64_t)time(NULL);
        QueueForward(relay, &job);
        SendResult(socket, buffer, VERDICT_ACCEPT, filename, FALSE);
        LogEvent("Holding \"%s\" (%llu bytes) from %s (%s) for %s [%.8s]", filename, (unsigned long long)filesize,
            sender, peer, target, tracking);
        return;
    }

    char finalname[MAX_FILENAME_SIZE] = { 0 };
    char finalpath[MAX_PATH_SIZE] = { 0 };
    if (!UniqueFilename(config->inbox, filename, finalname, sizeof(finalname)) ||
        !JoinPath(finalpath, sizeof(finalpath), config->inbox, finalname) ||
        !MoveReplace(partpath, finalpath)) {
        SendResult(socket, buffer, VERDICT_FAILED, "receiver could not store the file", FALSE);
        LogEvent("Unable to store \"%s\" from %s (%s)", filename, sender, peer);
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
    SendResult(socket, buffer, VERDICT_ACCEPT, finalname, TRUE);
    LogEvent("Received \"%s\" (%llu bytes) from %s (%s)", finalname, (unsigned long long)filesize, sender, peer);
}

void ReceiveMail(const Config* config, Relay* relay, EZ_SOCKET socket, const uint8_t* token, BOOL* shutdown) {
    char peer[16] = { 0 };
    PeerAddress(socket, peer, sizeof(peer));
    SetSocketTimeout(socket, HANDSHAKE_TIMEOUT_MS);
    ez_Buffer* buffer = EZ_GENERATE_BUFFER(PACKET_BUFFER_SIZE);
    char partpath[MAX_PATH_SIZE] = { 0 };
    Serve(config, relay, socket, buffer, peer, token, shutdown, partpath, sizeof(partpath));
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
