#ifndef HISTORY_H
#define HISTORY_H

#include "data/defaults.h"
#include <easyobjects.h>
#include <stdint.h>

typedef enum {
    SENT_DELIVERED = 0,
    SENT_BAD_PASSWORD,
    SENT_NO_SPACE,
    SENT_REJECTED,
    SENT_UNREACHABLE,
    SENT_FAILED,
    SENT_DELIVERING
} SentStatus;

typedef struct {
    int64_t timestamp;
    uint64_t size;
    SentStatus status;
    char recipient[MAX_ID_SIZE];
    char address[16];
    uint16_t port;
    char filename[MAX_FILENAME_SIZE];
    char saved[MAX_FILENAME_SIZE];
    char tracking[TRACKING_SIZE + 1];
    char via[MAX_ID_SIZE];
    char detail[MAX_DETAIL_SIZE];
} Sent;

DECLARE_ARRLIST(Sent);

BOOL LoadHistory(const char* path, ARRLIST_Sent* history);

BOOL RecordSent(const char* path, const Sent* sent);

BOOL UpdateSent(const char* path, const char* tracking, SentStatus status, const char* detail, const char* saved, BOOL* found);

const char* SentStatusName(SentStatus status);

void CleanHistory(ARRLIST_Sent* history);

#endif
