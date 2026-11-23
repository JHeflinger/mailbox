#include "chatroom.h"
#include "core/daemon.h"
#include "core/platform.h"
#include "core/transfer.h"
#include "data/chat.h"
#include "util/output.h"
#include "util/terminal.h"
#include <easylogger.h>
#include <easyfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HISTORY_SHOWN 30
#define POLL_MS 200
#define READ_CHUNK 4096
#define HELD_NOTICE_SECONDS 5
#define SNIPPET_SIZE 40
#define DIM "\033[2m"

typedef struct {
    const Config* config;
    const char* host;
    Runtime runtime;
    char logpath[MAX_PATH_SIZE];
    uint64_t offset;
    char partial[MAX_LINE_SIZE * 2];
    size_t partiallen;
    ARRLIST_ChatMessage messages;
    char input[MAX_CHAT_SIZE];
    size_t inputlen;
    int escape;
    size_t drawnpending;
    BOOL quit;
} ChatRoom;

typedef struct {
    const Config* config;
    size_t count;
} ChatListing;

static size_t CountPending(ChatRoom* room) {
    size_t pending = 0;
    for (size_t i = 0; i < room->messages.size; i++) {
        if (room->messages.data[i].outgoing && room->messages.data[i].status == CHAT_PENDING) pending++;
    }
    return pending;
}

static size_t Codepoints(const char* str, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)str[i] & 0xC0) != 0x80) count++;
    }
    return count;
}

static void FormatClock(char* out, size_t size, int64_t timestamp) {
    time_t t = (time_t)timestamp;
    time_t now = time(NULL);
    struct tm when = { 0 };
    struct tm today = { 0 };
    struct tm* local = localtime(&t);
    if (local == NULL) {
        snprintf(out, size, "--:--");
        return;
    }
    when = *local;
    local = localtime(&now);
    if (local != NULL) today = *local;
    const char* format = (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday) ? "%H:%M" : "%m-%d %H:%M";
    if (strftime(out, size, format, &when) == 0) snprintf(out, size, "--:--");
}

static void Snippet(char* out, size_t size, const char* text) {
    size_t len = strlen(text);
    if (Codepoints(text, len) <= SNIPPET_SIZE) {
        snprintf(out, size, "%s", text);
        return;
    }
    size_t n = 0, points = 0;
    while (text[n] != '\0' && n < size - 4) {
        if (((unsigned char)text[n] & 0xC0) != 0x80) {
            if (points == SNIPPET_SIZE) break;
            points++;
        }
        n++;
    }
    snprintf(out, size, "%.*s...", (int)n, text);
}

static void DrawPrompt(ChatRoom* room) {
    size_t pending = CountPending(room);
    room->drawnpending = pending;
    char prompt[64] = { 0 };
    if (pending > 0) snprintf(prompt, sizeof(prompt), "you (%llu waiting)> ", (unsigned long long)pending);
    else snprintf(prompt, sizeof(prompt), "you> ");
    // only show the tail of the input that fits, so the line never wraps and \r always works
    int width = TerminalWidth() - (int)strlen(prompt) - 1;
    if (width < 4) width = 4;
    const char* start = room->input;
    size_t points = Codepoints(room->input, room->inputlen);
    while (points > (size_t)width && *start) {
        start++;
        while (((unsigned char)*start & 0xC0) == 0x80) start++;
        points--;
    }
    printf("\r\033[K%s%s%s%s", Paint(EZ_GREEN), prompt, Paint(EZ_RESET), start);
    fflush(stdout);
}

static void ClearPrompt() {
    printf("\r\033[K");
}

static void PrintMessage(ChatRoom* room, const ChatMessage* m, BOOL replay) {
    char clock[32] = { 0 };
    FormatClock(clock, sizeof(clock), m->time);
    const char* who = m->outgoing ? "you" : room->host;
    const char* color = m->outgoing ? EZ_GREEN : EZ_CYAN;
    printf("%s[%s]%s %s%s%s: %s", Paint(DIM), clock, Paint(EZ_RESET), Paint(color), who, Paint(EZ_RESET), m->text);
    if (replay && m->outgoing && m->status == CHAT_PENDING) printf(" %s(waiting)%s", Paint(DIM), Paint(EZ_RESET));
    if (replay && m->outgoing && m->status == CHAT_FAILED) {
        printf(" %s(not delivered: %s)%s", Paint(EZ_RED), m->detail[0] ? m->detail : "failed", Paint(EZ_RESET));
    }
    printf("\n");
}

static void HandleLine(ChatRoom* room, char* text, BOOL live) {
    ChatLine line;
    if (ParseChatLine(text, &line) == CHATLINE_NONE) return;
    ChatMessage* m = ApplyChatLine(&room->messages, &line);
    if (!live || m == NULL) return;
    char snippet[MAX_CHAT_SIZE] = { 0 };
    if (line.kind == CHATLINE_MESSAGE) {
        ClearPrompt();
        PrintMessage(room, m, FALSE);
    } else if (m->status == CHAT_FAILED) {
        Snippet(snippet, sizeof(snippet), m->text);
        ClearPrompt();
        printf("  %s✗ not delivered: \"%s\"%s%s%s\n", Paint(EZ_RED), snippet, m->detail[0] ? " - " : "", m->detail, Paint(EZ_RESET));
    } else if (m->status == CHAT_DELIVERED && m->settled - m->time >= HELD_NOTICE_SECONDS) {
        // only worth mentioning when it had to wait, e.g. they were offline
        Snippet(snippet, sizeof(snippet), m->text);
        ClearPrompt();
        printf("  %s✓ delivered: \"%s\"%s\n", Paint(DIM), snippet, Paint(EZ_RESET));
    }
}

static BOOL PollLog(ChatRoom* room, BOOL live) {
    uint64_t size = 0;
    if (!QueryFileSize(room->logpath, &size)) return FALSE;
    if (size < room->offset) {
        // the log was cleared or replaced underneath us - start over
        room->offset = 0;
        room->partiallen = 0;
        CleanChat(&room->messages);
    }
    if (size == room->offset) return FALSE;
    FILE* file = fopen(room->logpath, "rb");
    if (file == NULL) return FALSE;
    if (fseek(file, (long)room->offset, SEEK_SET) != 0) {
        fclose(file);
        return FALSE;
    }
    BOOL changed = FALSE;
    char chunk[READ_CHUNK];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        room->offset += got;
        for (size_t i = 0; i < got; i++) {
            if (chunk[i] != '\n') {
                if (room->partiallen < sizeof(room->partial) - 1) room->partial[room->partiallen++] = chunk[i];
                continue;
            }
            room->partial[room->partiallen] = '\0';
            HandleLine(room, room->partial, live);
            room->partiallen = 0;
            changed = TRUE;
        }
    }
    fclose(file);
    return changed;
}

static void PrintHelp() {
    printf("%s  type and press enter to send - messages to someone offline wait and deliver when they're back\n", Paint(DIM));
    printf("  /quit, ctrl+c or ctrl+d to leave, ctrl+u clears the line, ctrl+l clears the screen%s\n", Paint(EZ_RESET));
}

static void Submit(ChatRoom* room) {
    room->input[room->inputlen] = '\0';
    char text[MAX_CHAT_SIZE] = { 0 };
    snprintf(text, sizeof(text), "%s", room->input);
    SanitizeChatText(text);
    room->inputlen = 0;
    room->input[0] = '\0';
    if (text[0] == '\0') return;
    if (strcmp(text, "/quit") == 0 || strcmp(text, "/exit") == 0 || strcmp(text, "/q") == 0) {
        room->quit = TRUE;
        return;
    }
    if (strcmp(text, "/help") == 0) {
        ClearPrompt();
        PrintHelp();
        return;
    }
    char error[MAX_DETAIL_SIZE] = { 0 };
    // re-read the runtime file every time - the daemon may have been restarted (new token/port) mid-chat
    BOOL running = DaemonRunning(room->config, &room->runtime);
    if (!running) snprintf(error, sizeof(error), "mailbox is down - run \"mailbox up\"");
    if (!running || !QueueChatMessage(room->runtime.port, room->runtime.token, room->host, text, error, sizeof(error))) {
        char snippet[MAX_CHAT_SIZE] = { 0 };
        Snippet(snippet, sizeof(snippet), text);
        ClearPrompt();
        printf("  %s✗ couldn't send \"%s\" - %s%s\n", Paint(EZ_RED), snippet, error, Paint(EZ_RESET));
        return;
    }
    // the daemon writes the message into the log before it answers, so it shows up right away
    PollLog(room, TRUE);
}

static void Backspace(ChatRoom* room) {
    while (room->inputlen > 0) {
        unsigned char c = (unsigned char)room->input[--room->inputlen];
        if ((c & 0xC0) != 0x80) break;
    }
    room->input[room->inputlen] = '\0';
}

static void DeleteWord(ChatRoom* room) {
    while (room->inputlen > 0 && room->input[room->inputlen - 1] == ' ') room->inputlen--;
    while (room->inputlen > 0 && room->input[room->inputlen - 1] != ' ') room->inputlen--;
    room->input[room->inputlen] = '\0';
}

static void HandleKey(ChatRoom* room, unsigned char c) {
    if (room->escape == 1) {
        room->escape = (c == '[' || c == 'O') ? 2 : 0;
        return;
    }
    if (room->escape == 2) {
        if (c >= 0x40 && c <= 0x7E) room->escape = 0;
        return;
    }
    switch (c) {
        case 0x1B: room->escape = 1; return;
        case '\r': case '\n': Submit(room); return;
        case 0x7F: case 0x08: Backspace(room); return;
        case 0x15: room->inputlen = 0; room->input[0] = '\0'; return;
        case 0x17: DeleteWord(room); return;
        case 0x04: if (room->inputlen == 0) room->quit = TRUE; return;
        case 0x0C: printf("\033[2J\033[H"); return;
        case '\t': c = ' '; break;
        default: break;
    }
    if (c < 0x20) return;
    // leave room so a multi-byte character never gets cut in half at the limit
    if (room->inputlen + 4 >= sizeof(room->input)) return;
    room->input[room->inputlen++] = (char)c;
    room->input[room->inputlen] = '\0';
}

static void ReplayHistory(ChatRoom* room) {
    size_t total = room->messages.size;
    size_t first = total > HISTORY_SHOWN ? total - HISTORY_SHOWN : 0;
    printf("%s── chat with %s%s%s", Paint(DIM), Paint(EZ_CYAN), room->host, Paint(EZ_RESET));
    printf("%s ── /help for keys", Paint(DIM));
    if (first > 0) printf(" ── %llu older messages in %s", (unsigned long long)first, room->logpath);
    printf("%s\n", Paint(EZ_RESET));
    for (size_t i = first; i < total; i++) PrintMessage(room, &room->messages.data[i], TRUE);
}

BOOL RunChatRoom(const Config* config, const char* host) {
    if (!ValidID(host)) {
        PrintError("\"%s\" isn't a mailbox ID - chat uses IDs (the ID: in their config), not IP addresses", host);
        return FALSE;
    }
    if (strcmp(host, config->id) == 0) {
        PrintError("You can't chat with yourself");
        return FALSE;
    }
    if (!IsInteractiveTerminal()) {
        PrintError("chat needs an interactive terminal");
        return FALSE;
    }
    ChatRoom room;
    memset(&room, 0, sizeof(room));
    room.config = config;
    room.host = host;
    if (!DaemonRunning(config, &room.runtime)) {
        PrintError("mailbox is down - run \"mailbox up\" first, it's what sends and receives chats");
        return FALSE;
    }
    if (!ChatLogPath(config, host, room.logpath, sizeof(room.logpath))) {
        PrintError("Chat path for \"%s\" is too long", host);
        return FALSE;
    }
    Route route;
    if (!LookupRoute(config, host, &route)) {
        PrintWarn("mailbox doesn't know how to reach %s yet (not in %s or %s) - you'll see their messages, but yours will fail",
            host, config->dns, config->proxypath);
    }

    PollLog(&room, FALSE);
    ReplayHistory(&room);
    if (!BeginRawInput()) {
        PrintError("Unable to set up the terminal for chat");
        CleanChat(&room.messages);
        return FALSE;
    }
    DrawPrompt(&room);
    while (!room.quit) {
        char keys[256];
        int got = ReadKeys(keys, sizeof(keys), POLL_MS);
        if (got < 0 || InterruptRequested()) break;
        for (int i = 0; i < got && !room.quit; i++) HandleKey(&room, (unsigned char)keys[i]);
        if (room.quit) break;
        BOOL changed = PollLog(&room, TRUE);
        if (got > 0 || changed || CountPending(&room) != room.drawnpending) DrawPrompt(&room);
    }
    ClearPrompt();
    fflush(stdout);
    EndRawInput();
    printf("%sleft chat with %s%s\n", Paint(DIM), host, Paint(EZ_RESET));
    CleanChat(&room.messages);
    return TRUE;
}

static void VisitChat(const char* name, void* data) {
    ChatListing* listing = (ChatListing*)data;
    size_t len = strlen(name);
    size_t extlen = strlen(CHAT_EXTENSION);
    if (len <= extlen || strcmp(name + len - extlen, CHAT_EXTENSION) != 0) return;
    char host[MAX_ID_SIZE] = { 0 };
    if (len - extlen >= sizeof(host)) return;
    memcpy(host, name, len - extlen);
    if (!ValidID(host)) return;
    char path[MAX_PATH_SIZE] = { 0 };
    if (!ChatLogPath(listing->config, host, path, sizeof(path))) return;
    ez_File* file = ez_load_file(path);
    if (file == NULL) return;
    ARRLIST_ChatMessage messages = { 0 };
    ez_FileParser parser = ez_parser(file);
    char line[MAX_LINE_SIZE] = { 0 };
    while (ez_next_line(&parser, line, sizeof(line) - 1)) {
        ChatLine parsed;
        if (ParseChatLine(line, &parsed) != CHATLINE_NONE) ApplyChatLine(&messages, &parsed);
    }
    ez_free_file(file);
    if (messages.size > 0) {
        ChatMessage* last = &messages.data[messages.size - 1];
        size_t waiting = 0;
        for (size_t i = 0; i < messages.size; i++) {
            if (messages.data[i].outgoing && messages.data[i].status == CHAT_PENDING) waiting++;
        }
        char when[32] = { 0 };
        char snippet[MAX_CHAT_SIZE] = { 0 };
        FormatTimestamp(when, sizeof(when), last->time);
        Snippet(snippet, sizeof(snippet), last->text);
        printf("  %s%-20s%s %-16s  %s%s: %s", Paint(EZ_CYAN), host, Paint(EZ_RESET), when,
            last->outgoing ? "you" : host, "", snippet);
        if (waiting > 0) printf("  %s(%llu waiting)%s", Paint(EZ_YELLOW), (unsigned long long)waiting, Paint(EZ_RESET));
        printf("\n");
        listing->count++;
    }
    CleanChat(&messages);
}

BOOL ListChatRooms(const Config* config) {
    ChatListing listing = { config, 0 };
    char usage[32] = { 0 };
    char limit[32] = { 0 };
    FormatSize(usage, sizeof(usage), DirectoryUsage(config->chatpath));
    FormatSize(limit, sizeof(limit), config->chatsize);
    printf("Chats for %s%s%s (%s - %s of %s used)\n\n", Paint(EZ_CYAN), config->id, Paint(EZ_RESET), config->chatpath, usage, limit);
    ListDirectory(config->chatpath, VisitChat, &listing);
    if (listing.count == 0) printf("  No chats yet - start one with \"mailbox chat <id>\".\n");
    return TRUE;
}
