#ifndef DAEMON_H
#define DAEMON_H

#include "data/config.h"

typedef struct {
    int64_t pid;
    uint16_t port;
    uint8_t token[TOKEN_SIZE];
} Runtime;

BOOL ReadRuntime(const Config* config, Runtime* runtime);

BOOL DaemonRunning(const Config* config, Runtime* runtime);

BOOL StartDaemon(const Config* config);

BOOL StopDaemon(const Config* config);

int RunDaemon(const Config* config);

#endif
