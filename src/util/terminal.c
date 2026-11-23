#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <io.h>
#else
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#endif

#define FALLBACK_COLUMNS 80
#define KEY_POLL_MS 20

static volatile int g_interrupted = 0;
static BOOL g_raw = FALSE;

#ifdef _WIN32
static UINT g_previous_codepage = 0;

static BOOL WINAPI HandleConsoleControl(DWORD event) {
    (void)event;
    g_interrupted = 1;
    return TRUE;
}
#else
static struct termios g_previous;

static void HandleInterrupt(int signal) {
    (void)signal;
    g_interrupted = 1;
}
#endif

BOOL IsInteractiveTerminal() {
    #ifdef _WIN32
    return _isatty(0) && _isatty(1);
    #else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    #endif
}

int TerminalWidth() {
    #ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return info.srWindow.Right - info.srWindow.Left + 1;
    }
    #else
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return size.ws_col;
    #endif
    return FALLBACK_COLUMNS;
}

BOOL BeginRawInput() {
    if (g_raw) return TRUE;
    g_interrupted = 0;
    #ifdef _WIN32
    g_previous_codepage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(HandleConsoleControl, TRUE);
    #else
    if (tcgetattr(STDIN_FILENO, &g_previous) != 0) return FALSE;
    struct termios raw = g_previous;
    // no line buffering or echo - we draw the input line ourselves. ISIG stays on so ctrl+c still
    // arrives as a signal, which only sets a flag so we can put the terminal back properly
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return FALSE;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = HandleInterrupt;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    #endif
    g_raw = TRUE;
    atexit(EndRawInput);
    return TRUE;
}

void EndRawInput() {
    if (!g_raw) return;
    g_raw = FALSE;
    #ifdef _WIN32
    SetConsoleCtrlHandler(HandleConsoleControl, FALSE);
    if (g_previous_codepage != 0) SetConsoleOutputCP(g_previous_codepage);
    #else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_previous);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    #endif
}

BOOL InterruptRequested() {
    return g_interrupted != 0;
}

#ifdef _WIN32
static int AppendWide(char* out, size_t size, size_t used, const wchar_t* wide, int count) {
    char utf8[8] = { 0 };
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, count, utf8, sizeof(utf8), NULL, NULL);
    if (len <= 0 || used + (size_t)len > size) return 0;
    memcpy(out + used, utf8, (size_t)len);
    return len;
}
#endif

int ReadKeys(char* out, size_t size, int timeout_ms) {
    #ifdef _WIN32
    int waited = 0;
    while (!_kbhit()) {
        if (g_interrupted) return -1;
        if (waited >= timeout_ms) return 0;
        Sleep(KEY_POLL_MS);
        waited += KEY_POLL_MS;
    }
    size_t used = 0;
    while (_kbhit() && used + 8 < size) {
        wchar_t wide[2] = { (wchar_t)_getwch(), 0 };
        if (wide[0] == 0 || wide[0] == 0xE0) {
            _getwch(); // arrow keys and friends arrive as a two-part code - drop them
            continue;
        }
        int count = 1;
        if (wide[0] >= 0xD800 && wide[0] <= 0xDBFF && _kbhit()) {
            wide[1] = (wchar_t)_getwch();
            count = 2;
        }
        if (wide[0] == 26) {
            out[used++] = 0x04; // ctrl+z behaves like ctrl+d
            continue;
        }
        used += (size_t)AppendWide(out, size, used, wide, count);
    }
    return (int)used;
    #else
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
    if (ready < 0) return (errno == EINTR && !g_interrupted) ? 0 : -1;
    if (ready == 0) return g_interrupted ? -1 : 0;
    ssize_t got = read(STDIN_FILENO, out, size);
    if (got < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (got == 0) return -1; // stdin closed
    return (int)got;
    #endif
}
