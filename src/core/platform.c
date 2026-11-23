#define _FILE_OFFSET_BITS 64
#define _CRT_RAND_S
#include "platform.h"
#include "data/defaults.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <easytime.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <pwd.h>
#endif

static BOOL IsSeparator(char c) {
    return c == '/' || c == '\\';
}

static BOOL EndsWith(const char* str, const char* suffix) {
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    return slen >= xlen && strcmp(str + slen - xlen, suffix) == 0;
}

BOOL JoinPath(char* out, size_t size, const char* directory, const char* name) {
    size_t dlen = strlen(directory);
    int written;
    if (dlen == 0) written = snprintf(out, size, "%s", name);
    else if (IsSeparator(directory[dlen - 1])) written = snprintf(out, size, "%s%s", directory, name);
    else written = snprintf(out, size, "%s%c%s", directory, PATH_SEPARATOR, name);
    return written > 0 && (size_t)written < size;
}

BOOL IsAbsolutePath(const char* path) {
    if (path[0] == '\0') return FALSE;
    #ifdef _WIN32
    if (IsSeparator(path[0])) return TRUE;
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') return TRUE;
    return FALSE;
    #else
    return path[0] == '/';
    #endif
}

BOOL AbsolutePath(char* out, size_t size, const char* path) {
    if (IsAbsolutePath(path)) {
        int written = snprintf(out, size, "%s", path);
        return written > 0 && (size_t)written < size;
    }
    #ifdef _WIN32
    return _fullpath(out, path, size) != NULL;
    #else
    char cwd[MAX_PATH_SIZE] = { 0 };
    if (getcwd(cwd, sizeof(cwd)) == NULL) return FALSE;
    return JoinPath(out, size, cwd, path);
    #endif
}

const char* PathBasename(const char* path) {
    const char* base = path;
    for (const char* c = path; *c != '\0'; c++) {
        if (IsSeparator(*c) && *(c + 1) != '\0') base = c + 1;
    }
    return base;
}

BOOL ParentDirectory(char* out, size_t size, const char* path) {
    size_t len = strlen(path);
    if (len + 1 > size) return FALSE;
    strcpy(out, path);
    while (len > 1 && IsSeparator(out[len - 1])) out[--len] = '\0';
    for (int i = (int)len - 1; i >= 0; i--) {
        if (IsSeparator(out[i])) {
            if (i == 0) out[1] = '\0';
            #ifdef _WIN32
            else if (i == 2 && out[1] == ':') out[3] = '\0';
            #endif
            else out[i] = '\0';
            return TRUE;
        }
    }
    strcpy(out, ".");
    return TRUE;
}

static BOOL MakeDirectory(const char* path) {
    #ifdef _WIN32
    if (_mkdir(path) == 0) return TRUE;
    #else
    if (mkdir(path, 0755) == 0) return TRUE;
    #endif
    if (errno != EEXIST) return FALSE;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

BOOL MakeDirectories(const char* path) {
    char buffer[MAX_PATH_SIZE] = { 0 };
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buffer)) return FALSE;
    strcpy(buffer, path);
    size_t start = 1;
    #ifdef _WIN32
    if (len >= 2 && buffer[1] == ':') start = 3;
    #endif
    for (size_t i = start; i < len; i++) {
        if (IsSeparator(buffer[i])) {
            char c = buffer[i];
            buffer[i] = '\0';
            if (!MakeDirectory(buffer)) return FALSE;
            buffer[i] = c;
        }
    }
    return MakeDirectory(buffer);
}

BOOL QueryFileSize(const char* path, uint64_t* size) {
    #ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return FALSE;
    if ((st.st_mode & _S_IFREG) == 0) return FALSE;
    #else
    struct stat st;
    if (stat(path, &st) != 0) return FALSE;
    if (!S_ISREG(st.st_mode)) return FALSE;
    #endif
    *size = (uint64_t)st.st_size;
    return TRUE;
}

uint64_t DirectoryUsage(const char* path) {
    uint64_t total = 0;
    char child[MAX_PATH_SIZE] = { 0 };
    #ifdef _WIN32
    char pattern[MAX_PATH_SIZE] = { 0 };
    if (!JoinPath(pattern, sizeof(pattern), path, "*")) return 0;
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        if (!JoinPath(child, sizeof(child), path, data.cFileName)) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += DirectoryUsage(child);
        else total += (((uint64_t)data.nFileSizeHigh) << 32) | (uint64_t)data.nFileSizeLow;
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    #else
    DIR* dir = opendir(path);
    if (dir == NULL) return 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!JoinPath(child, sizeof(child), path, entry->d_name)) continue;
        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) total += DirectoryUsage(child);
        else if (S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
    }
    closedir(dir);
    #endif
    return total;
}

void PurgeExtension(const char* directory, const char* extension) {
    char child[MAX_PATH_SIZE] = { 0 };
    #ifdef _WIN32
    char pattern[MAX_PATH_SIZE] = { 0 };
    if (!JoinPath(pattern, sizeof(pattern), directory, "*")) return;
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!EndsWith(data.cFileName, extension)) continue;
        if (JoinPath(child, sizeof(child), directory, data.cFileName)) RemoveFile(child);
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
    #else
    DIR* dir = opendir(directory);
    if (dir == NULL) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!EndsWith(entry->d_name, extension)) continue;
        if (JoinPath(child, sizeof(child), directory, entry->d_name)) RemoveFile(child);
    }
    closedir(dir);
    #endif
}

void ListDirectory(const char* directory, FileVisitor visitor, void* data) {
    #ifdef _WIN32
    char pattern[MAX_PATH_SIZE] = { 0 };
    if (!JoinPath(pattern, sizeof(pattern), directory, "*")) return;
    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        visitor(found.cFileName, data);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
    #else
    DIR* dir = opendir(directory);
    if (dir == NULL) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        visitor(entry->d_name, data);
    }
    closedir(dir);
    #endif
}

BOOL MoveReplace(const char* source, const char* destination) {
    #ifdef _WIN32
    return MoveFileExA(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    #else
    return rename(source, destination) == 0;
    #endif
}

BOOL RestrictFile(const char* path) {
    #ifdef _WIN32
    (void)path;
    return TRUE;
    #else
    return chmod(path, 0600) == 0;
    #endif
}

BOOL RemoveFile(const char* path) {
    return remove(path) == 0;
}

BOOL HomeDirectory(char* out, size_t size) {
    const char* home = NULL;
    #ifdef _WIN32
    home = getenv("USERPROFILE");
    if (home == NULL || home[0] == '\0') {
        const char* drive = getenv("HOMEDRIVE");
        const char* path = getenv("HOMEPATH");
        if (drive == NULL || path == NULL) return FALSE;
        int written = snprintf(out, size, "%s%s", drive, path);
        return written > 0 && (size_t)written < size;
    }
    #else
    home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        struct passwd* pw = getpwuid(getuid());
        if (pw == NULL) return FALSE;
        home = pw->pw_dir;
    }
    #endif
    int written = snprintf(out, size, "%s", home);
    return written > 0 && (size_t)written < size;
}

BOOL MachineName(char* out, size_t size) {
    memset(out, 0, size);
    #ifdef _WIN32
    DWORD len = (DWORD)size;
    if (!GetComputerNameA(out, &len)) return FALSE;
    #else
    if (gethostname(out, size - 1) != 0) return FALSE;
    #endif
    return out[0] != '\0';
}

BOOL RandomBytes(uint8_t* out, size_t size) {
    #ifdef _WIN32
    for (size_t i = 0; i < size; i++) {
        unsigned int value;
        if (rand_s(&value) != 0) return FALSE;
        out[i] = (uint8_t)(value & 0xFF);
    }
    return TRUE;
    #else
    FILE* file = fopen("/dev/urandom", "rb");
    if (file == NULL) return FALSE;
    size_t read = fread(out, 1, size, file);
    fclose(file);
    return read == size;
    #endif
}

int64_t CurrentProcess() {
    #ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
    #else
    return (int64_t)getpid();
    #endif
}

BOOL ProcessAlive(int64_t pid) {
    if (pid <= 0) return FALSE;
    #ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (process == NULL) return FALSE;
    DWORD code = 0;
    BOOL alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
    #else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
    #endif
}

BOOL KillProcess(int64_t pid) {
    if (pid <= 0) return FALSE;
    #ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (process == NULL) return FALSE;
    BOOL killed = TerminateProcess(process, 0) != 0;
    CloseHandle(process);
    return killed;
    #else
    return kill((pid_t)pid, SIGTERM) == 0;
    #endif
}

DetachResult DetachDaemon() {
    #ifdef _WIN32
    char exe[MAX_PATH_SIZE] = { 0 };
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len == 0 || len >= sizeof(exe)) return DETACH_FAILED;
    char command[MAX_PATH_SIZE + 64] = { 0 };
    snprintf(command, sizeof(command), "\"%s\" %s", exe, DAEMON_COMMAND);
    STARTUPINFOA startup = { 0 };
    PROCESS_INFORMATION info = { 0 };
    startup.cb = sizeof(startup);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE,
            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &startup, &info)) {
        return DETACH_FAILED;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return DETACH_PARENT;
    #else
    fflush(stdout);
    fflush(stderr);
    pid_t first = fork();
    if (first < 0) return DETACH_FAILED;
    if (first > 0) {
        int status = 0;
        waitpid(first, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? DETACH_PARENT : DETACH_FAILED;
    }
    if (setsid() < 0) _exit(1);
    signal(SIGHUP, SIG_IGN);
    pid_t second = fork();
    if (second < 0) _exit(1);
    if (second > 0) _exit(0);
    if (chdir("/") != 0) _exit(1);
    return DETACH_CHILD;
    #endif
}

BOOL RedirectOutput(const char* logfile) {
    #ifdef _WIN32
    const char* null_device = "NUL";
    #else
    const char* null_device = "/dev/null";
    #endif
    if (freopen(null_device, "r", stdin) == NULL) return FALSE;
    if (freopen(logfile, "a", stdout) == NULL) return FALSE;
    if (freopen(logfile, "a", stderr) == NULL) return FALSE;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return TRUE;
}

#define LOCK_STALE_SECONDS 10
#define LOCK_WAIT_MS 5000
#define LOCK_POLL_MS 20

BOOL AcquireLock(const char* path) {
    for (int waited = 0; waited <= LOCK_WAIT_MS; waited += LOCK_POLL_MS) {
        #ifdef _WIN32
        int fd = _open(path, _O_CREAT | _O_EXCL | _O_WRONLY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            _close(fd);
            return TRUE;
        }
        #else
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) {
            close(fd);
            return TRUE;
        }
        #endif
        if (errno != EEXIST) return FALSE;
        struct stat st;
        if (stat(path, &st) == 0 && difftime(time(NULL), st.st_mtime) > LOCK_STALE_SECONDS) {
            RemoveFile(path);
            continue;
        }
        ez_sleep_ms(LOCK_POLL_MS);
    }
    return FALSE;
}

void ReleaseLock(const char* path) {
    RemoveFile(path);
}

BOOL ReadPassword(const char* prompt, char* out, size_t size) {
    printf("%s", prompt);
    fflush(stdout);
    memset(out, 0, size);
    #ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    BOOL console = GetConsoleMode(input, &mode);
    if (console) SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
    char* res = fgets(out, (int)size, stdin);
    if (console) {
        SetConsoleMode(input, mode);
        printf("\n");
    }
    #else
    struct termios previous;
    BOOL terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &previous) == 0;
    if (terminal) {
        struct termios hidden = previous;
        hidden.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }
    char* res = fgets(out, (int)size, stdin);
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &previous);
        printf("\n");
    }
    #endif
    if (res == NULL) return FALSE;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return TRUE;
}
