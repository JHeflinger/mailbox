#include "daemon.h"
#include "core/platform.h"
#include "core/transfer.h"
#include "core/wire.h"
#include "util/output.h"
#include <easynet.h>
#include <easytime.h>
#include <easyfile.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
#include <signal.h>
#endif

#define DAEMON_POLL_MS 50
#define ACCEPT_BACKOFF_MS 250

static volatile int g_terminate = 0;

#ifndef _WIN32
static void HandleTerminate(int signal) {
    (void)signal;
    g_terminate = 1;
}
#endif

static void InstallHandlers() {
    #ifndef _WIN32
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = HandleTerminate;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // no SA_RESTART - lets select() wake up on signals
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    #endif
}

static BOOL WriteRuntime(const Config* config, const Runtime* runtime) {
    char path[MAX_PATH_SIZE] = { 0 };
    char temp[MAX_PATH_SIZE + 16] = { 0 };
    if (!RuntimePath(config, RUNTIME_FILE_NAME, path, sizeof(path))) return FALSE;
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE* file = fopen(temp, "w");
    if (file == NULL) return FALSE;
    fprintf(file, "%lld\n%u\n", (long long)runtime->pid, (unsigned int)runtime->port);
    for (int i = 0; i < TOKEN_SIZE; i++) fprintf(file, "%02x", runtime->token[i]);
    fprintf(file, "\n");
    BOOL ok = fclose(file) == 0;
    RestrictFile(temp);
    return ok && MoveReplace(temp, path);
}

static void RemoveRuntime(const Config* config) {
    char path[MAX_PATH_SIZE] = { 0 };
    if (RuntimePath(config, RUNTIME_FILE_NAME, path, sizeof(path))) RemoveFile(path);
}

BOOL ReadRuntime(const Config* config, Runtime* runtime) {
    memset(runtime, 0, sizeof(Runtime));
    char path[MAX_PATH_SIZE] = { 0 };
    if (!RuntimePath(config, RUNTIME_FILE_NAME, path, sizeof(path))) return FALSE;
    FILE* file = fopen(path, "r");
    if (file == NULL) return FALSE;
    long long pid = 0;
    unsigned int port = 0;
    char hex[TOKEN_SIZE * 2 + 8] = { 0 };
    int fields = fscanf(file, "%lld %u %66s", &pid, &port, hex);
    fclose(file);
    if (fields != 3 || pid <= 0 || port > 65535 || strlen(hex) != TOKEN_SIZE * 2) return FALSE;
    for (int i = 0; i < TOKEN_SIZE; i++) {
        unsigned int byte = 0;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return FALSE;
        runtime->token[i] = (uint8_t)byte;
    }
    runtime->pid = (int64_t)pid;
    runtime->port = (uint16_t)port;
    return TRUE;
}

BOOL DaemonRunning(const Config* config, Runtime* runtime) {
    if (!ReadRuntime(config, runtime)) return FALSE;
    if (ProcessAlive(runtime->pid)) return TRUE;
    RemoveRuntime(config);
    return FALSE;
}

static void PrintLogTail(const char* logpath) {
    ez_File* file = ez_file_exists(logpath) ? ez_load_file(logpath) : NULL;
    if (file == NULL) return;
    size_t start = file->size;
    int lines = 0;
    while (start > 0 && lines < 4) {
        start--;
        if (file->data[start] == '\n' && start != file->size - 1) lines++;
    }
    if (start > 0) start++;
    printf("Last log lines (%s):\n%.*s", logpath, (int)(file->size - start), file->data + start);
    ez_free_file(file);
}

BOOL StartDaemon(const Config* config) {
    Runtime runtime;
    if (DaemonRunning(config, &runtime)) {
        PrintNote("mailbox is already up (pid %lld, port %u)", (long long)runtime.pid, (unsigned int)runtime.port);
        return TRUE;
    }
    char logpath[MAX_PATH_SIZE] = { 0 };
    RuntimePath(config, LOG_FILE_NAME, logpath, sizeof(logpath));
    DetachResult result = DetachDaemon();
    if (result == DETACH_FAILED) {
        PrintError("Unable to start the mailbox background process");
        return FALSE;
    }
    if (result == DETACH_CHILD) {
        int code = RunDaemon(config);
        EZ_CLEAN_NETWORK();
        exit(code);
    }
    for (int waited = 0; waited < DAEMON_STARTUP_WAIT_MS; waited += DAEMON_POLL_MS) {
        if (DaemonRunning(config, &runtime)) {
            char limit[32] = { 0 };
            FormatSize(limit, sizeof(limit), config->inboxsize);
            PrintSuccess("mailbox is up as \"%s\" on port %u (pid %lld)", config->id, (unsigned int)runtime.port, (long long)runtime.pid);
            printf("  directory: %s (%s limit%s)\n", config->inbox, limit, config->protected ? ", password protected" : "");
            return TRUE;
        }
        ez_sleep_ms(DAEMON_POLL_MS);
    }
    PrintError("The mailbox background process failed to start");
    PrintLogTail(logpath);
    return FALSE;
}

BOOL StopDaemon(const Config* config) {
    Runtime runtime;
    if (!DaemonRunning(config, &runtime)) {
        PrintNote("mailbox is not running");
        return TRUE;
    }
    Config target = *config;
    target.port = runtime.port;
    BOOL graceful = RequestShutdown(&target, runtime.token);
    for (int waited = 0; graceful && waited < DAEMON_STARTUP_WAIT_MS; waited += DAEMON_POLL_MS) {
        if (!ProcessAlive(runtime.pid)) break;
        ez_sleep_ms(DAEMON_POLL_MS);
    }
    if (ProcessAlive(runtime.pid)) {
        if (!KillProcess(runtime.pid)) {
            PrintError("Unable to stop the mailbox background process (pid %lld)", (long long)runtime.pid);
            return FALSE;
        }
        for (int waited = 0; waited < DAEMON_STARTUP_WAIT_MS && ProcessAlive(runtime.pid); waited += DAEMON_POLL_MS) {
            ez_sleep_ms(DAEMON_POLL_MS);
        }
    }
    RemoveRuntime(config);
    PrintSuccess("mailbox is down");
    return TRUE;
}

static BOOL WaitForConnection(ez_Server* server) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server->socket, &readfds);
    // blocks with no timeout - the process sleeps until a connection (or signal) arrives
    int found = select((int)server->socket + 1, &readfds, NULL, NULL, NULL);
    return found > 0;
}

int RunDaemon(const Config* config) {
    char logpath[MAX_PATH_SIZE] = { 0 };
    RuntimePath(config, LOG_FILE_NAME, logpath, sizeof(logpath));
    if (!RedirectOutput(logpath)) return 1;
    InstallHandlers();
    EZ_INIT_NETWORK();
    PurgeExtension(config->inbox, PART_EXTENSION);

    Runtime runtime = { 0 };
    runtime.pid = CurrentProcess();
    runtime.port = config->port;
    if (!RandomBytes(runtime.token, sizeof(runtime.token))) {
        LogEvent("Unable to generate a control token");
        return 1;
    }
    ez_Server* server = EZ_GENERATE_SERVER();
    if (!EZ_OPEN_SERVER(server, config->port)) {
        LogEvent("Unable to listen on port %u - is it already in use?", (unsigned int)config->port);
        EZ_CLEAN_SERVER(server);
        return 1;
    }
    if (!WriteRuntime(config, &runtime)) {
        LogEvent("Unable to write runtime file in \"%s\"", config->home);
        EZ_CLEAN_SERVER(server);
        return 1;
    }
    LogEvent("mailbox %s up as \"%s\" on port %u (pid %lld), receiving into \"%s\"",
        MAILBOX_VERSION, config->id, (unsigned int)config->port, (long long)runtime.pid, config->inbox);

    BOOL shutdown = FALSE;
    while (!shutdown && !g_terminate) {
        if (!WaitForConnection(server)) {
            if (g_terminate) break;
            ez_sleep_ms(ACCEPT_BACKOFF_MS);
            continue;
        }
        ez_Connection* connection = EZ_SERVER_ACCEPT(server);
        if (connection == NULL) {
            ez_sleep_ms(ACCEPT_BACKOFF_MS);
            continue;
        }
        ReceiveMail(config, connection->socket, runtime.token, &shutdown);
        EZ_CLOSE_CONNECTION(connection);
    }

    Runtime current;
    if (ReadRuntime(config, &current) && current.pid == runtime.pid) RemoveRuntime(config);
    EZ_CLEAN_SERVER(server);
    LogEvent("mailbox down");
    return 0;
}
