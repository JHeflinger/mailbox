#ifndef OUTPUT_H
#define OUTPUT_H

#include <easybool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t total;
    uint64_t done;
    float start;
    float last;
    BOOL finished;
} Progress;

void InitializeOutput();

const char* Paint(const char* color);

void PrintError(const char* fmt, ...);

void PrintWarn(const char* fmt, ...);

void PrintSuccess(const char* fmt, ...);

void PrintNote(const char* fmt, ...);

void LogEvent(const char* fmt, ...);

void FormatSize(char* out, size_t size, uint64_t bytes);

void FormatTimestamp(char* out, size_t size, int64_t timestamp);

void BeginProgress(Progress* progress, uint64_t total);

void UpdateProgress(Progress* progress, uint64_t done);

void EndProgress(Progress* progress);

#endif
