#ifndef CONFIG_H
#define CONFIG_H

#include "data/defaults.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char path[MAX_PATH_SIZE];
    char home[MAX_PATH_SIZE];
    uint16_t port;
    char id[MAX_ID_SIZE];
    uint64_t inboxsize;
    char inbox[MAX_PATH_SIZE];
    char dns[MAX_PATH_SIZE];
    char state[MAX_PATH_SIZE];
    char password[MAX_PASSWORD_SIZE];
    BOOL protected;
} Config;

BOOL LocateConfig(char* out, size_t size);

BOOL LoadConfig(Config* config);

BOOL ValidID(const char* id);

BOOL RuntimePath(const Config* config, const char* name, char* out, size_t size);

void TrimString(char* str);

#endif
