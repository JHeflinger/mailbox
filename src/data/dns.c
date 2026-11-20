#include "dns.h"
#include "data/config.h"
#include "core/platform.h"
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

IMPL_ARRLIST(Contact);

BOOL LoadDNS(const char* path, ARRLIST_Contact* contacts) {
    ARRLIST_Contact_clear(contacts);
    if (!ez_file_exists(path)) return TRUE;
    ez_File* file = ez_load_file(path);
    if (file == NULL) return FALSE;
    ez_FileParser parser = ez_parser(file);
    char line[MAX_LINE_SIZE] = { 0 };
    while (ez_next_line(&parser, line, sizeof(line) - 1)) {
        TrimString(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        Contact contact = { 0 };
        char id[MAX_LINE_SIZE] = { 0 };
        char address[MAX_LINE_SIZE] = { 0 };
        unsigned int port = 0;
        int fields = sscanf(line, "%2047s %2047s %u", id, address, &port);
        if (fields < 2 || !ValidID(id) || strlen(address) >= sizeof(contact.address) || port > 65535) continue;
        strcpy(contact.id, id);
        strcpy(contact.address, address);
        contact.port = fields == 3 ? (uint16_t)port : 0;
        Contact* existing = FindContact(contacts, contact.id);
        if (existing != NULL) *existing = contact;
        else ARRLIST_Contact_add(contacts, contact);
    }
    ez_free_file(file);
    return TRUE;
}

BOOL SaveDNS(const char* path, ARRLIST_Contact* contacts) {
    char temp[MAX_PATH_SIZE + 16] = { 0 };
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE* file = fopen(temp, "w");
    if (file == NULL) return FALSE;
    fprintf(file, "# mailbox dns - <id> <ip> <port>\n");
    for (size_t i = 0; i < contacts->size; i++) {
        Contact* contact = &contacts->data[i];
        fprintf(file, "%s %s %u\n", contact->id, contact->address, (unsigned int)contact->port);
    }
    BOOL ok = fflush(file) == 0;
    ok = (fclose(file) == 0) && ok;
    if (!ok) {
        RemoveFile(temp);
        return FALSE;
    }
    return MoveReplace(temp, path);
}

Contact* FindContact(ARRLIST_Contact* contacts, const char* id) {
    for (size_t i = 0; i < contacts->size; i++) {
        if (strcmp(contacts->data[i].id, id) == 0) return &contacts->data[i];
    }
    return NULL;
}

BOOL RecordContact(const char* path, const char* id, const char* address, uint16_t port) {
    if (!ValidID(id)) return FALSE;
    char lock[MAX_PATH_SIZE + 16] = { 0 };
    snprintf(lock, sizeof(lock), "%s.lock", path);
    if (!AcquireLock(lock)) return FALSE;
    ARRLIST_Contact contacts = { 0 };
    BOOL ok = LoadDNS(path, &contacts);
    if (ok) {
        Contact* existing = FindContact(&contacts, id);
        Contact contact = { 0 };
        snprintf(contact.id, sizeof(contact.id), "%s", id);
        snprintf(contact.address, sizeof(contact.address), "%s", address);
        contact.port = port;
        if (existing != NULL) *existing = contact;
        else ARRLIST_Contact_add(&contacts, contact);
        ok = SaveDNS(path, &contacts);
    }
    CleanDNS(&contacts);
    ReleaseLock(lock);
    return ok;
}

void CleanDNS(ARRLIST_Contact* contacts) {
    ARRLIST_Contact_clear(contacts);
}
