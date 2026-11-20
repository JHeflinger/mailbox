#ifndef TRANSFER_H
#define TRANSFER_H

#include "data/config.h"
#include <easynet.h>

BOOL SendMail(const Config* config, const char* target, const char* filepath);

void ReceiveMail(const Config* config, EZ_SOCKET socket, const uint8_t* token, BOOL* shutdown);

BOOL RequestShutdown(const Config* config, const uint8_t* token);

BOOL SanitizeFilename(const char* name, char* out, size_t size);

BOOL UniqueFilename(const char* directory, const char* name, char* out, size_t size);

#endif
