#include "state.h"
#include "core/platform.h"
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_HEADER "# mailbox state v1 - <id>\\t<read>\\t<unix time>\\t<bytes>\\t<sender>\\t<ip>\\t<file>"
#define STATE_FIELDS 7

IMPL_ARRLIST(Mail);

static size_t SplitFields(char* line, char** fields, size_t max) {
    size_t count = 0;
    fields[count++] = line;
    for (char* c = line; *c != '\0' && count < max; c++) {
        if (*c == '\t') {
            *c = '\0';
            fields[count++] = c + 1;
        }
    }
    return count;
}

static BOOL ParseMail(char* line, Mail* mail) {
    char* fields[STATE_FIELDS] = { 0 };
    if (SplitFields(line, fields, STATE_FIELDS) != STATE_FIELDS) return FALSE;
    memset(mail, 0, sizeof(Mail));
    char* end = NULL;
    unsigned long long id = strtoull(fields[0], &end, 10);
    if (*end != '\0' || id == 0 || id > UINT32_MAX) return FALSE;
    mail->id = (uint32_t)id;
    mail->read = strcmp(fields[1], "1") == 0;
    mail->timestamp = (int64_t)strtoll(fields[2], NULL, 10);
    mail->size = (uint64_t)strtoull(fields[3], NULL, 10);
    if (strlen(fields[4]) >= sizeof(mail->sender) ||
        strlen(fields[5]) >= sizeof(mail->address) ||
        strlen(fields[6]) >= sizeof(mail->filename)) return FALSE;
    strcpy(mail->sender, fields[4]);
    strcpy(mail->address, fields[5]);
    strcpy(mail->filename, fields[6]);
    return TRUE;
}

static BOOL SaveState(const char* path, ARRLIST_Mail* mail) {
    char temp[MAX_PATH_SIZE + 16] = { 0 };
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE* file = fopen(temp, "w");
    if (file == NULL) return FALSE;
    fprintf(file, "%s\n", STATE_HEADER);
    for (size_t i = 0; i < mail->size; i++) {
        Mail* m = &mail->data[i];
        fprintf(file, "%u\t%d\t%lld\t%llu\t%s\t%s\t%s\n",
            (unsigned int)m->id, m->read ? 1 : 0, (long long)m->timestamp,
            (unsigned long long)m->size, m->sender, m->address, m->filename);
    }
    BOOL ok = fflush(file) == 0;
    ok = (fclose(file) == 0) && ok;
    if (!ok) {
        RemoveFile(temp);
        return FALSE;
    }
    return MoveReplace(temp, path);
}

static void LockPath(const char* path, char* out, size_t size) {
    snprintf(out, size, "%s.lock", path);
}

BOOL LoadState(const char* path, ARRLIST_Mail* mail) {
    ARRLIST_Mail_clear(mail);
    if (!ez_file_exists(path)) return TRUE;
    ez_File* file = ez_load_file(path);
    if (file == NULL) return FALSE;
    ez_FileParser parser = ez_parser(file);
    char line[MAX_LINE_SIZE] = { 0 };
    while (ez_next_line(&parser, line, sizeof(line) - 1)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        Mail m;
        if (ParseMail(line, &m)) ARRLIST_Mail_add(mail, m);
    }
    ez_free_file(file);
    return TRUE;
}

BOOL RecordMail(const char* path, Mail* mail) {
    char lock[MAX_PATH_SIZE + 16] = { 0 };
    LockPath(path, lock, sizeof(lock));
    if (!AcquireLock(lock)) return FALSE;
    ARRLIST_Mail list = { 0 };
    BOOL ok = LoadState(path, &list);
    if (ok) {
        uint32_t next = 1;
        for (size_t i = 0; i < list.size; i++) {
            if (list.data[i].id >= next) next = list.data[i].id + 1;
        }
        mail->id = next;
        ARRLIST_Mail_add(&list, *mail);
        ok = SaveState(path, &list);
    }
    CleanState(&list);
    ReleaseLock(lock);
    return ok;
}

ClearResult ClearMail(const char* path, BOOL all, uint32_t id, size_t* cleared) {
    *cleared = 0;
    char lock[MAX_PATH_SIZE + 16] = { 0 };
    LockPath(path, lock, sizeof(lock));
    if (!AcquireLock(lock)) return CLEAR_FAILED;
    ARRLIST_Mail list = { 0 };
    ClearResult result = CLEAR_OK;
    if (!LoadState(path, &list)) {
        result = CLEAR_FAILED;
    } else {
        BOOL found = FALSE;
        for (size_t i = 0; i < list.size; i++) {
            Mail* m = &list.data[i];
            if (!all && m->id != id) continue;
            found = TRUE;
            if (!m->read) {
                m->read = TRUE;
                (*cleared)++;
            }
        }
        if (!all && !found) result = CLEAR_NOT_FOUND;
        else if (!all && *cleared == 0) result = CLEAR_ALREADY_READ;
        else if (*cleared > 0 && !SaveState(path, &list)) result = CLEAR_FAILED;
    }
    CleanState(&list);
    ReleaseLock(lock);
    return result;
}

void CleanState(ARRLIST_Mail* mail) {
    ARRLIST_Mail_clear(mail);
}
