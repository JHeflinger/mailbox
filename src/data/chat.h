#ifndef CHAT_H
#define CHAT_H

#include "data/config.h"
#include <easyobjects.h>

typedef enum {
    CHAT_PENDING = 0,
    CHAT_DELIVERED,
    CHAT_FAILED,
    CHAT_RECEIVED
} ChatStatus;

typedef struct {
    int64_t time;
    int64_t settled;
    BOOL outgoing;
    ChatStatus status;
    char tracking[TRACKING_SIZE + 1];
    char text[MAX_CHAT_SIZE];
    char detail[MAX_DETAIL_SIZE];
} ChatMessage;

DECLARE_ARRLIST(ChatMessage);

typedef enum {
    CHATLINE_NONE = 0,
    CHATLINE_MESSAGE,
    CHATLINE_ACK
} ChatLineKind;

typedef struct {
    ChatLineKind kind;
    ChatMessage message;
} ChatLine;

typedef enum {
    APPEND_OK = 0,
    APPEND_FULL,
    APPEND_FAILED
} AppendResult;

BOOL ChatLogPath(const Config* config, const char* host, char* out, size_t size);

AppendResult AppendChatMessage(const Config* config, const char* host, BOOL outgoing, const char* tracking, int64_t time, const char* text);

BOOL AppendChatAck(const Config* config, const char* host, const char* tracking, BOOL delivered, const char* detail);

ChatLineKind ParseChatLine(char* line, ChatLine* out);

ChatMessage* ApplyChatLine(ARRLIST_ChatMessage* messages, const ChatLine* line);

void SanitizeChatText(char* text);

void CleanChat(ARRLIST_ChatMessage* messages);

#endif
