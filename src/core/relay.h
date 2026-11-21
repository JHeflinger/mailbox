#ifndef RELAY_H
#define RELAY_H

#include "data/config.h"
#include <easyobjects.h>

typedef enum {
    JOB_FORWARD = 0, // holding a file in PROXYDIR, trying to pass it on
    JOB_AWAIT,       // handed the file to the next proxy, waiting for its status report
    JOB_REPORT       // have a final status, trying to tell the machine that gave us the file
} JobKind;

typedef struct {
    JobKind kind;
    char tracking[TRACKING_SIZE + 1];
    char origin[MAX_ID_SIZE];
    char target[MAX_ID_SIZE];
    char upstream[16];
    uint16_t upstreamport;
    char filename[MAX_FILENAME_SIZE];
    uint64_t size;
    uint8_t hops;
    uint8_t outcome;
    char detail[MAX_DETAIL_SIZE];
    char saved[MAX_FILENAME_SIZE];
    int64_t due;
    uint32_t attempts;
} RelayJob;

DECLARE_ARRLIST(RelayJob);

typedef struct {
    const Config* config;
    ARRLIST_RelayJob jobs;
    char queuepath[MAX_PATH_SIZE];
} Relay;

void InitializeRelay(Relay* relay, const Config* config);

void CleanRelay(Relay* relay);

int64_t RelayWaitMs(Relay* relay);

void RunRelayJobs(Relay* relay);

BOOL RelayHasJob(Relay* relay, const char* tracking);

BOOL RelayStoragePath(Relay* relay, const char* tracking, BOOL part, char* out, size_t size);

void QueueForward(Relay* relay, const RelayJob* job);

void HandleStatus(Relay* relay, const char* tracking, uint8_t outcome, const char* detail, const char* saved);

BOOL ValidTracking(const char* tracking);

BOOL GenerateTracking(char* out, size_t size);

#endif
