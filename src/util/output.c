#include "output.h"
#include <easylogger.h>
#include <easytime.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#define ISATTY(fd) _isatty(fd)
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#define ISATTY(fd) isatty(fd)
#endif

#define PROGRESS_WIDTH 40
#define PROGRESS_INTERVAL 0.1f

static BOOL g_colors = FALSE;
static BOOL g_interactive = FALSE;

void InitializeOutput() {
    g_interactive = ISATTY(1) ? TRUE : FALSE;
    g_colors = g_interactive;
    #ifdef _WIN32
    if (g_colors) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (!GetConsoleMode(out, &mode) || !SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            g_colors = FALSE;
        }
    }
    #endif
}

const char* Paint(const char* color) {
    return g_colors ? color : "";
}

static void PrintTagged(FILE* stream, const char* color, const char* tag, const char* fmt, va_list args) {
    fprintf(stream, "%s%s%s ", Paint(color), tag, Paint(EZ_RESET));
    vfprintf(stream, fmt, args);
    fprintf(stream, "\n");
    fflush(stream);
}

void PrintError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PrintTagged(stdout, EZ_RED, "error:", fmt, args);
    va_end(args);
}

void PrintWarn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PrintTagged(stdout, EZ_YELLOW, "warning:", fmt, args);
    va_end(args);
}

void PrintSuccess(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PrintTagged(stdout, EZ_GREEN, "success:", fmt, args);
    va_end(args);
}

void PrintNote(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    PrintTagged(stdout, EZ_CYAN, "mailbox:", fmt, args);
    va_end(args);
}

void LogEvent(const char* fmt, ...) {
    char stamp[32] = { 0 };
    time_t now = time(NULL);
    struct tm* local = localtime(&now);
    if (local == NULL || strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local) == 0) strcpy(stamp, "?");
    printf("[%s] ", stamp);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

void FormatSize(char* out, size_t size, uint64_t bytes) {
    const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(out, size, "%llu B", (unsigned long long)bytes);
    else snprintf(out, size, "%.1f %s", value, units[unit]);
}

void FormatTimestamp(char* out, size_t size, int64_t timestamp) {
    time_t t = (time_t)timestamp;
    struct tm* local = localtime(&t);
    if (local == NULL || strftime(out, size, "%Y-%m-%d %H:%M", local) == 0) {
        snprintf(out, size, "unknown");
    }
}

static void DrawProgress(Progress* progress) {
    float pct = progress->total == 0 ? 100.0f : 100.0f * (float)((double)progress->done / (double)progress->total);
    int filled = (int)(pct * PROGRESS_WIDTH / 100.0f);
    if (filled > PROGRESS_WIDTH) filled = PROGRESS_WIDTH;
    char bar[PROGRESS_WIDTH + 1] = { 0 };
    for (int i = 0; i < PROGRESS_WIDTH; i++) {
        if (i < filled) bar[i] = '=';
        else if (i == filled) bar[i] = '>';
        else bar[i] = ' ';
    }
    char done[32] = { 0 };
    char total[32] = { 0 };
    char rate[32] = { 0 };
    float elapsed = ez_get_time() - progress->start;
    FormatSize(done, sizeof(done), progress->done);
    FormatSize(total, sizeof(total), progress->total);
    FormatSize(rate, sizeof(rate), elapsed > 0.0f ? (uint64_t)((double)progress->done / elapsed) : 0);
    printf("\rProgress: [%s%s%s] %6.2f%%  %s / %s  %s/s    ",
        Paint(EZ_GREEN), bar, Paint(EZ_RESET), pct, done, total, rate);
    fflush(stdout);
}

void BeginProgress(Progress* progress, uint64_t total) {
    memset(progress, 0, sizeof(Progress));
    progress->total = total;
    progress->start = ez_get_time();
    progress->last = -1.0f;
    if (g_interactive) DrawProgress(progress);
}

void UpdateProgress(Progress* progress, uint64_t done) {
    progress->done = done;
    if (!g_interactive) return;
    float now = ez_get_time();
    if (now - progress->last < PROGRESS_INTERVAL && done != progress->total) return;
    progress->last = now;
    DrawProgress(progress);
}

void EndProgress(Progress* progress) {
    if (progress->finished) return;
    progress->finished = TRUE;
    if (g_interactive || progress->done == progress->total) DrawProgress(progress);
    printf("\n");
    fflush(stdout);
}
