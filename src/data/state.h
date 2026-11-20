#ifndef STATE_H
#define STATE_H

#include "data/defaults.h"
#include <easyobjects.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    BOOL read;
    int64_t timestamp;
    uint64_t size;
    char sender[MAX_ID_SIZE];
    char address[16];
    char filename[MAX_FILENAME_SIZE];
} Mail;

DECLARE_ARRLIST(Mail);

typedef enum {
    CLEAR_OK = 0,
    CLEAR_NOT_FOUND,
    CLEAR_ALREADY_READ,
    CLEAR_FAILED
} ClearResult;

BOOL LoadState(const char* path, ARRLIST_Mail* mail);

BOOL RecordMail(const char* path, Mail* mail);

ClearResult ClearMail(const char* path, BOOL all, uint32_t id, size_t* cleared);

void CleanState(ARRLIST_Mail* mail);

#endif
