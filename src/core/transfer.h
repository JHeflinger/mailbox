#ifndef TRANSFER_H
#define TRANSFER_H

#include "core/relay.h"
#include <easynet.h>

typedef enum {
    TRANSMIT_DELIVERED = 0, // landed in the receiver's inbox
    TRANSMIT_RELAYED,       // a proxy took it and will pass it on
    TRANSMIT_REJECTED,      // receiver said no (password, space, route, ...)
    TRANSMIT_UNREACHABLE,   // couldn't connect
    TRANSMIT_PASSWORD,      // receiver wants a password and we can't ask for one
    TRANSMIT_LOST,          // connection broke partway
    TRANSMIT_LOCAL          // couldn't read our own file
} TransmitResult;

typedef struct {
    const char* sender;
    const char* filepath;
    const char* filename;
    uint64_t size;
    ez_Ipv4 address;
    uint16_t port;
    BOOL proxy;
    const char* target;
    const char* tracking;
    uint16_t replyport;
    uint8_t hops;
    BOOL interactive;
} Transmission;

typedef struct {
    uint8_t verdict;
    char server[MAX_ID_SIZE];
    char saved[MAX_FILENAME_SIZE];
    char message[MAX_DETAIL_SIZE];
    uint64_t available;
    uint64_t limit;
} TransmitOutcome;

typedef struct {
    ez_Ipv4 address;
    char addressstr[16];
    uint16_t port;
    BOOL proxy;
} Route;

BOOL LookupRoute(const Config* config, const char* id, Route* route);

TransmitResult Transmit(const Transmission* transmission, TransmitOutcome* outcome);

BOOL SendStatus(const char* address, uint16_t port, const char* tracking, uint8_t outcome, const char* detail, const char* saved);

BOOL SendMail(const Config* config, const char* target, const char* filepath);

void ReceiveMail(const Config* config, Relay* relay, EZ_SOCKET socket, const uint8_t* token, BOOL* shutdown);

BOOL RequestShutdown(const Config* config, const uint8_t* token);

BOOL SanitizeFilename(const char* name, char* out, size_t size);

BOOL UniqueFilename(const char* directory, const char* name, char* out, size_t size);

void VerdictReason(uint8_t code, const char* message, char* out, size_t size);

#endif
