#ifndef TERMINAL_H
#define TERMINAL_H

#include <easybool.h>
#include <stddef.h>

BOOL IsInteractiveTerminal();

int TerminalWidth();

BOOL BeginRawInput();

void EndRawInput();

int ReadKeys(char* out, size_t size, int timeout_ms);

BOOL InterruptRequested();

#endif
