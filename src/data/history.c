#include "history.h"
#include "core/platform.h"
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_HEADER "# mailbox history v2 - <unix time>\\t<status>\\t<bytes>\\t<recipient>\\t<ip>\\t<port>\\t<file>\\t<saved as>\\t<tracking>\\t<via>\\t<detail>"
#define HISTORY_FIELDS_V1 8
#define HISTORY_FIELDS 11

IMPL_ARRLIST(Sent);

static const char* g_status_names[] = {
    "delivered",
    "bad-password",
    "no-space",
    "rejected",
    "unreachable",
    "failed",
    "delivering"
};

const char* SentStatusName(SentStatus status) {
    if ((int)status < 0 || (size_t)status >= sizeof(g_status_names) / sizeof(g_status_names[0])) return "unknown";
    return g_status_names[status];
}

static BOOL ParseStatus(const char* str, SentStatus* status) {
    for (size_t i = 0; i < sizeof(g_status_names) / sizeof(g_status_names[0]); i++) {
        if (strcmp(str, g_status_names[i]) == 0) {
            *status = (SentStatus)i;
            return TRUE;
        }
    }
    return FALSE;
}

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

static BOOL CopyChecked(char* out, size_t size, const char* str) {
    if (strlen(str) >= size) return FALSE;
    strcpy(out, str);
    return TRUE;
}

static BOOL ParseSent(char* line, Sent* sent) {
    char* fields[HISTORY_FIELDS] = { 0 };
    size_t count = SplitFields(line, fields, HISTORY_FIELDS);
    if (count != HISTORY_FIELDS && count != HISTORY_FIELDS_V1) return FALSE;
    memset(sent, 0, sizeof(Sent));
    sent->timestamp = (int64_t)strtoll(fields[0], NULL, 10);
    if (!ParseStatus(fields[1], &sent->status)) return FALSE;
    sent->size = (uint64_t)strtoull(fields[2], NULL, 10);
    unsigned long port = strtoul(fields[5], NULL, 10);
    if (port > 65535) return FALSE;
    sent->port = (uint16_t)port;
    if (!CopyChecked(sent->recipient, sizeof(sent->recipient), fields[3]) ||
        !CopyChecked(sent->address, sizeof(sent->address), fields[4]) ||
        !CopyChecked(sent->filename, sizeof(sent->filename), fields[6]) ||
        !CopyChecked(sent->saved, sizeof(sent->saved), fields[7])) return FALSE;
    if (count == HISTORY_FIELDS) {
        if (!CopyChecked(sent->tracking, sizeof(sent->tracking), fields[8]) ||
            !CopyChecked(sent->via, sizeof(sent->via), fields[9]) ||
            !CopyChecked(sent->detail, sizeof(sent->detail), fields[10])) return FALSE;
    }
    return TRUE;
}

static void WriteSent(FILE* file, const Sent* sent) {
    fprintf(file, "%lld\t%s\t%llu\t%s\t%s\t%u\t%s\t%s\t%s\t%s\t%s\n",
        (long long)sent->timestamp, SentStatusName(sent->status), (unsigned long long)sent->size,
        sent->recipient, sent->address, (unsigned int)sent->port, sent->filename, sent->saved,
        sent->tracking, sent->via, sent->detail);
}

static void LockPath(const char* path, char* out, size_t size) {
    snprintf(out, size, "%s.lock", path);
}

static void CleanField(char* str) {
    for (char* c = str; *c; c++) {
        if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
    }
}

BOOL LoadHistory(const char* path, ARRLIST_Sent* history) {
    ARRLIST_Sent_clear(history);
    if (!ez_file_exists(path)) return TRUE;
    ez_File* file = ez_load_file(path);
    if (file == NULL) return FALSE;
    ez_FileParser parser = ez_parser(file);
    char line[MAX_LINE_SIZE] = { 0 };
    while (ez_next_line(&parser, line, sizeof(line) - 1)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;
        Sent sent;
        if (ParseSent(line, &sent)) ARRLIST_Sent_add(history, sent);
    }
    ez_free_file(file);
    return TRUE;
}

BOOL RecordSent(const char* path, const Sent* sent) {
    char lock[MAX_PATH_SIZE + 16] = { 0 };
    LockPath(path, lock, sizeof(lock));
    if (!AcquireLock(lock)) return FALSE;
    Sent clean = *sent;
    CleanField(clean.recipient);
    CleanField(clean.detail);
    BOOL fresh = !ez_file_exists(path);
    FILE* file = fopen(path, "a");
    BOOL ok = file != NULL;
    if (ok) {
        if (fresh) fprintf(file, "%s\n", HISTORY_HEADER);
        WriteSent(file, &clean);
        ok = fclose(file) == 0;
    }
    ReleaseLock(lock);
    return ok;
}

BOOL UpdateSent(const char* path, const char* tracking, SentStatus status, const char* detail, const char* saved, BOOL* found) {
    *found = FALSE;
    if (tracking[0] == '\0') return TRUE;
    char lock[MAX_PATH_SIZE + 16] = { 0 };
    LockPath(path, lock, sizeof(lock));
    if (!AcquireLock(lock)) return FALSE;
    ARRLIST_Sent history = { 0 };
    BOOL ok = LoadHistory(path, &history);
    if (ok) {
        for (size_t i = 0; i < history.size; i++) {
            Sent* sent = &history.data[i];
            if (strcmp(sent->tracking, tracking) != 0) continue;
            *found = TRUE;
            sent->status = status;
            snprintf(sent->detail, sizeof(sent->detail), "%s", detail);
            CleanField(sent->detail);
            if (saved[0] != '\0') snprintf(sent->saved, sizeof(sent->saved), "%s", saved);
        }
    }
    if (ok && *found) {
        char temp[MAX_PATH_SIZE + 16] = { 0 };
        snprintf(temp, sizeof(temp), "%s.tmp", path);
        FILE* file = fopen(temp, "w");
        ok = file != NULL;
        if (ok) {
            fprintf(file, "%s\n", HISTORY_HEADER);
            for (size_t i = 0; i < history.size; i++) WriteSent(file, &history.data[i]);
            ok = fclose(file) == 0;
            ok = ok && MoveReplace(temp, path);
            if (!ok) RemoveFile(temp);
        }
    }
    CleanHistory(&history);
    ReleaseLock(lock);
    return ok;
}

void CleanHistory(ARRLIST_Sent* history) {
    ARRLIST_Sent_clear(history);
}
