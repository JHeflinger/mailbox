#include "chat.h"
#include "core/platform.h"
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHAT_FIELDS 5

IMPL_ARRLIST(ChatMessage);

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

static BOOL AppendLine(const char* path, const char* line) {
    char lock[MAX_PATH_SIZE + 16] = { 0 };
    snprintf(lock, sizeof(lock), "%s.lock", path);
    if (!AcquireLock(lock)) return FALSE;
    FILE* file = fopen(path, "a");
    BOOL ok = file != NULL;
    if (ok) {
        // one write per line so a reader tailing the file never sees half of one for long
        ok = fputs(line, file) >= 0;
        ok = (fclose(file) == 0) && ok;
    }
    ReleaseLock(lock);
    return ok;
}

void SanitizeChatText(char* text) {
    for (char* c = text; *c; c++) {
        unsigned char u = (unsigned char)*c;
        if (u < 0x20 || u == 0x7F) *c = ' ';
    }
    size_t len = strlen(text);
    while (len > 0 && text[len - 1] == ' ') text[--len] = '\0';
    size_t start = 0;
    while (text[start] == ' ') start++;
    if (start > 0) memmove(text, text + start, len - start + 1);
}

BOOL ChatLogPath(const Config* config, const char* host, char* out, size_t size) {
    if (!ValidID(host)) return FALSE;
    char name[MAX_ID_SIZE + 16] = { 0 };
    snprintf(name, sizeof(name), "%s%s", host, CHAT_EXTENSION);
    return JoinPath(out, size, config->chatpath, name);
}

AppendResult AppendChatMessage(const Config* config, const char* host, BOOL outgoing, const char* tracking, int64_t time, const char* text) {
    char path[MAX_PATH_SIZE] = { 0 };
    if (!ChatLogPath(config, host, path, sizeof(path))) return APPEND_FAILED;
    char clean[MAX_CHAT_SIZE] = { 0 };
    snprintf(clean, sizeof(clean), "%s", text);
    SanitizeChatText(clean);
    char line[MAX_CHAT_SIZE + TRACKING_SIZE + 64] = { 0 };
    int written = snprintf(line, sizeof(line), "msg\t%lld\t%s\t%s\t%s\n", (long long)time, outgoing ? "out" : "in", tracking, clean);
    if (written <= 0 || (size_t)written >= sizeof(line)) return APPEND_FAILED;
    if (DirectoryUsage(config->chatpath) + (uint64_t)written > config->chatsize) return APPEND_FULL;
    return AppendLine(path, line) ? APPEND_OK : APPEND_FAILED;
}

BOOL AppendChatAck(const Config* config, const char* host, const char* tracking, BOOL delivered, const char* detail) {
    char path[MAX_PATH_SIZE] = { 0 };
    if (!ChatLogPath(config, host, path, sizeof(path))) return FALSE;
    char clean[MAX_DETAIL_SIZE] = { 0 };
    snprintf(clean, sizeof(clean), "%s", detail);
    SanitizeChatText(clean);
    char line[MAX_DETAIL_SIZE + TRACKING_SIZE + 64] = { 0 };
    snprintf(line, sizeof(line), "ack\t%lld\t%s\t%s\t%s\n", (long long)time(NULL), tracking,
        delivered ? "delivered" : "failed", clean);
    return AppendLine(path, line);
}

ChatLineKind ParseChatLine(char* line, ChatLine* out) {
    memset(out, 0, sizeof(ChatLine));
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) line[--len] = '\0';
    char* fields[CHAT_FIELDS] = { 0 };
    if (SplitFields(line, fields, CHAT_FIELDS) != CHAT_FIELDS) return CHATLINE_NONE;
    ChatMessage* m = &out->message;
    m->time = (int64_t)strtoll(fields[1], NULL, 10);
    if (strcmp(fields[0], "msg") == 0) {
        if (strlen(fields[3]) >= sizeof(m->tracking)) return CHATLINE_NONE;
        m->outgoing = strcmp(fields[2], "out") == 0;
        m->status = m->outgoing ? CHAT_PENDING : CHAT_RECEIVED;
        strcpy(m->tracking, fields[3]);
        snprintf(m->text, sizeof(m->text), "%s", fields[4]);
        out->kind = CHATLINE_MESSAGE;
        return out->kind;
    }
    if (strcmp(fields[0], "ack") == 0) {
        if (strlen(fields[2]) >= sizeof(m->tracking)) return CHATLINE_NONE;
        strcpy(m->tracking, fields[2]);
        m->status = strcmp(fields[3], "delivered") == 0 ? CHAT_DELIVERED : CHAT_FAILED;
        snprintf(m->detail, sizeof(m->detail), "%s", fields[4]);
        out->kind = CHATLINE_ACK;
        return out->kind;
    }
    return CHATLINE_NONE;
}

ChatMessage* ApplyChatLine(ARRLIST_ChatMessage* messages, const ChatLine* line) {
    if (line->kind == CHATLINE_MESSAGE) {
        ARRLIST_ChatMessage_add(messages, line->message);
        return &messages->data[messages->size - 1];
    }
    if (line->kind != CHATLINE_ACK) return NULL;
    for (size_t i = messages->size; i > 0; i--) {
        ChatMessage* m = &messages->data[i - 1];
        if (!m->outgoing || strcmp(m->tracking, line->message.tracking) != 0) continue;
        m->status = line->message.status;
        m->settled = line->message.time;
        strcpy(m->detail, line->message.detail);
        return m;
    }
    return NULL;
}

void CleanChat(ARRLIST_ChatMessage* messages) {
    ARRLIST_ChatMessage_clear(messages);
}
