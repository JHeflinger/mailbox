#ifndef PLATFORM_H
#define PLATFORM_H

#include <easybool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    DETACH_FAILED = 0,
    DETACH_PARENT,
    DETACH_CHILD
} DetachResult;

BOOL JoinPath(char* out, size_t size, const char* directory, const char* name);

BOOL IsAbsolutePath(const char* path);

BOOL AbsolutePath(char* out, size_t size, const char* path);

const char* PathBasename(const char* path);

BOOL ParentDirectory(char* out, size_t size, const char* path);

BOOL MakeDirectories(const char* path);

BOOL QueryFileSize(const char* path, uint64_t* size);

uint64_t DirectoryUsage(const char* path);

void PurgeExtension(const char* directory, const char* extension);

BOOL MoveReplace(const char* source, const char* destination);

BOOL RestrictFile(const char* path);

BOOL RemoveFile(const char* path);

BOOL HomeDirectory(char* out, size_t size);

BOOL MachineName(char* out, size_t size);

BOOL RandomBytes(uint8_t* out, size_t size);

int64_t CurrentProcess();

BOOL ProcessAlive(int64_t pid);

BOOL KillProcess(int64_t pid);

DetachResult DetachDaemon();

BOOL RedirectOutput(const char* logfile);

BOOL AcquireLock(const char* path);

void ReleaseLock(const char* path);

BOOL ReadPassword(const char* prompt, char* out, size_t size);

#endif
