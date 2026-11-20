#ifndef DNS_H
#define DNS_H

#include "data/defaults.h"
#include <easyobjects.h>
#include <stdint.h>

typedef struct {
    char id[MAX_ID_SIZE];
    char address[16];
    uint16_t port;
} Contact;

DECLARE_ARRLIST(Contact);

BOOL LoadDNS(const char* path, ARRLIST_Contact* contacts);

BOOL SaveDNS(const char* path, ARRLIST_Contact* contacts);

Contact* FindContact(ARRLIST_Contact* contacts, const char* id);

BOOL RecordContact(const char* path, const char* id, const char* address, uint16_t port);

void CleanDNS(ARRLIST_Contact* contacts);

#endif
