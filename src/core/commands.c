#include "commands.h"
#include "core/daemon.h"
#include "core/transfer.h"
#include "data/config.h"
#include "data/state.h"
#include "util/output.h"
#include <easylogger.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void PrintUsage(const char* usage) {
    printf("usage: %s\n", usage);
}

BOOL HelpCommand(char** argv, int argc) {
    (void)argv;
    (void)argc;
    char config[MAX_PATH_SIZE] = { 0 };
    if (!LocateConfig(config, sizeof(config))) strcpy(config, "(unable to locate a home directory)");
    printf("%smailbox%s v%s - send files between machines\n\n", Paint(EZ_CYAN), Paint(EZ_RESET), MAILBOX_VERSION);
    printf("commands:\n");
    printf("  mailbox help                         lists all commands and usages\n");
    printf("  mailbox version | -v                 outputs the version\n");
    printf("  mailbox up                           starts the background process that receives mail\n");
    printf("  mailbox down                         shuts down the background process\n");
    printf("  mailbox inbox [all]                  lists unread mail and who sent it (\"all\" includes read mail)\n");
    printf("  mailbox send <ip|id>[:port] <file>   sends a file to an IP address or a known ID\n");
    printf("  mailbox clear <all|id...>            marks all mail, or the given mail IDs, as read\n\n");
    printf("config: %s\n", config);
    printf("  set %s to use a different config file\n", CONFIG_ENV_VARIABLE);
    printf("  set %s to skip the password prompt when sending\n\n", PASSWORD_ENV_VARIABLE);
    printf("config format:\n");
    printf("  PORT: %d          port to receive on and send to\n", DEFAULT_PORT);
    printf("  ID: my-machine      name this machine introduces itself as\n");
    printf("  INBOXSIZE: 1G       maximum total size of the mailbox directory (bytes, or K/M/G/T)\n");
    printf("  DIRECTORY: inbox    the mailbox directory files are received into\n");
    printf("  DNS: dns            ID -> IP lookup file, filled in automatically after successful sends\n");
    printf("  STATE: state        record of received mail and whether it has been read\n");
    printf("  PASSWORD:           optional - senders must enter it before a transfer is accepted\n");
    return TRUE;
}

BOOL VersionCommand(char** argv, int argc) {
    (void)argv;
    (void)argc;
    printf("mailbox v%s (%s)\n", MAILBOX_VERSION, OPSYS);
    return TRUE;
}

BOOL UpCommand(char** argv, int argc) {
    (void)argv;
    if (argc != 2) {
        PrintUsage("mailbox up");
        return FALSE;
    }
    Config config;
    if (!LoadConfig(&config)) return FALSE;
    return StartDaemon(&config);
}

BOOL DownCommand(char** argv, int argc) {
    (void)argv;
    if (argc != 2) {
        PrintUsage("mailbox down");
        return FALSE;
    }
    Config config;
    if (!LoadConfig(&config)) return FALSE;
    return StopDaemon(&config);
}

static int Max(int a, int b) {
    return a > b ? a : b;
}

BOOL InboxCommand(char** argv, int argc) {
    BOOL all = FALSE;
    if (argc == 3 && strcmp(argv[2], "all") == 0) {
        all = TRUE;
    } else if (argc != 2) {
        PrintUsage("mailbox inbox [all]");
        return FALSE;
    }
    Config config;
    if (!LoadConfig(&config)) return FALSE;
    ARRLIST_Mail mail = { 0 };
    if (!LoadState(config.state, &mail)) {
        PrintError("Unable to read state file \"%s\"", config.state);
        return FALSE;
    }

    size_t unread = 0;
    int wid = 2, wfrom = 4, wfile = 4;
    for (size_t i = 0; i < mail.size; i++) {
        Mail* m = &mail.data[i];
        if (!m->read) unread++;
        if (!all && m->read) continue;
        char buffer[32];
        wid = Max(wid, snprintf(buffer, sizeof(buffer), "%u", (unsigned int)m->id));
        wfrom = Max(wfrom, (int)(strlen(m->sender) + strlen(m->address) + 3));
        wfile = Max(wfile, (int)strlen(m->filename));
    }
    if (wfile > 48) wfile = 48;

    printf("Inbox for %s%s%s (%s) - %llu unread\n", Paint(EZ_CYAN), config.id, Paint(EZ_RESET), config.inbox, (unsigned long long)unread);
    size_t shown = 0;
    for (size_t i = 0; i < mail.size; i++) {
        Mail* m = &mail.data[i];
        if (!all && m->read) continue;
        if (shown == 0) {
            printf("\n  %-*s  %-*s  %-*s  %-10s  %s%s\n", wid, "ID", wfrom, "FROM", wfile, "FILE", "SIZE", "RECEIVED", all ? "          STATUS" : "");
        }
        char from[MAX_ID_SIZE + 24] = { 0 };
        char size[32] = { 0 };
        char when[32] = { 0 };
        snprintf(from, sizeof(from), "%s (%s)", m->sender, m->address);
        FormatSize(size, sizeof(size), m->size);
        FormatTimestamp(when, sizeof(when), m->timestamp);
        printf("  %s%-*u%s  %-*s  %-*.*s  %-10s  %s",
            Paint(m->read ? EZ_RESET : EZ_GREEN), wid, (unsigned int)m->id, Paint(EZ_RESET),
            wfrom, from, wfile, wfile, m->filename, size, when);
        if (all) printf("  %s", m->read ? "read" : "unread");
        printf("\n");
        shown++;
    }
    if (shown == 0) {
        printf("\n  %s\n", all ? "No mail has been received yet." : "No new mail.");
    } else if (unread > 0) {
        printf("\nMark mail as read with \"mailbox clear <id>\" or \"mailbox clear all\".\n");
    }
    Runtime runtime;
    if (!DaemonRunning(&config, &runtime)) {
        printf("\n");
        PrintWarn("mailbox is down - run \"mailbox up\" to receive mail");
    }
    CleanState(&mail);
    return TRUE;
}

BOOL SendCommand(char** argv, int argc) {
    if (argc != 4) {
        PrintUsage("mailbox send <ip|id>[:port] <file>");
        return FALSE;
    }
    Config config;
    if (!LoadConfig(&config)) return FALSE;
    return SendMail(&config, argv[2], argv[3]);
}

BOOL ClearCommand(char** argv, int argc) {
    if (argc < 3) {
        PrintUsage("mailbox clear <all|id...>");
        return FALSE;
    }
    Config config;
    if (!LoadConfig(&config)) return FALSE;
    if (argc == 3 && strcmp(argv[2], "all") == 0) {
        size_t cleared = 0;
        if (ClearMail(config.state, TRUE, 0, &cleared) != CLEAR_OK) {
            PrintError("Unable to update state file \"%s\"", config.state);
            return FALSE;
        }
        if (cleared == 0) PrintNote("No unread mail to clear");
        else PrintSuccess("Marked %llu %s as read", (unsigned long long)cleared, cleared == 1 ? "item" : "items");
        return TRUE;
    }
    BOOL ok = TRUE;
    for (int i = 2; i < argc; i++) {
        char* end = NULL;
        unsigned long id = strtoul(argv[i], &end, 10);
        if (argv[i][0] == '\0' || *end != '\0' || id == 0 || id > UINT32_MAX) {
            PrintError("\"%s\" is not a mail ID - use an ID from \"mailbox inbox\" or \"all\"", argv[i]);
            ok = FALSE;
            continue;
        }
        size_t cleared = 0;
        switch (ClearMail(config.state, FALSE, (uint32_t)id, &cleared)) {
            case CLEAR_OK:
                PrintSuccess("Marked mail %lu as read", id);
                break;
            case CLEAR_ALREADY_READ:
                PrintNote("Mail %lu was already read", id);
                break;
            case CLEAR_NOT_FOUND:
                PrintError("No mail with ID %lu", id);
                ok = FALSE;
                break;
            default:
                PrintError("Unable to update state file \"%s\"", config.state);
                ok = FALSE;
                break;
        }
    }
    return ok;
}

BOOL DaemonCommand(char** argv, int argc) {
    (void)argv;
    (void)argc;
    Config config;
    if (!LoadConfig(&config)) return FALSE;
    return RunDaemon(&config) == 0;
}
