#include "core/commands.h"
#include "data/defaults.h"
#include "util/output.h"
#include <easymemory.h>
#include <easylogger.h>
#include <easynet.h>
#include <string.h>

typedef BOOL (*CommandFunction)(char**, int);

typedef struct {
    const char* name;
    CommandFunction function;
} Command;

static const Command g_commands[] = {
    { "help",         HelpCommand },
    { "-h",           HelpCommand },
    { "--help",       HelpCommand },
    { "version",      VersionCommand },
    { "-v",           VersionCommand },
    { "--version",    VersionCommand },
    { "up",           UpCommand },
    { "down",         DownCommand },
    { "inbox",        InboxCommand },
    { "send",         SendCommand },
    { "clear",        ClearCommand },
    { "history",      HistoryCommand },
    { DAEMON_COMMAND, DaemonCommand },
};

int main(int argc, char** argv) {
    size_t memcheck = EZ_ALLOCATED();
    InitializeOutput();
    if (argc < 2) {
        HelpCommand(argv, argc);
        return 1;
    }
    CommandFunction function = NULL;
    for (size_t i = 0; i < sizeof(g_commands) / sizeof(g_commands[0]); i++) {
        if (strcmp(argv[1], g_commands[i].name) == 0) {
            function = g_commands[i].function;
            break;
        }
    }
    if (function == NULL) {
        PrintError("Unknown command \"%s\" - run \"mailbox help\" for a list of commands", argv[1]);
        return 1;
    }
    EZ_INIT_NETWORK();
    BOOL success = function(argv, argc);
    EZ_CLEAN_NETWORK();
    EZ_ASSERT(memcheck == EZ_ALLOCATED(), "Memory cleanup revealed a leak of %d bytes", (int)(EZ_ALLOCATED() - memcheck));
    return success ? 0 : 1;
}
