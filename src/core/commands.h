#ifndef COMMANDS_H
#define COMMANDS_H

#include <easybool.h>

BOOL HelpCommand(char** argv, int argc);

BOOL VersionCommand(char** argv, int argc);

BOOL UpCommand(char** argv, int argc);

BOOL DownCommand(char** argv, int argc);

BOOL InboxCommand(char** argv, int argc);

BOOL SendCommand(char** argv, int argc);

BOOL ClearCommand(char** argv, int argc);

BOOL HistoryCommand(char** argv, int argc);

BOOL DaemonCommand(char** argv, int argc);

#endif
