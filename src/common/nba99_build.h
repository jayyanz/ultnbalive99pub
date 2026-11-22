/*
 * Shared build settings for every NBA Live 99 binary (launcher and DLLs).
 *
 * Two builds come out of the same source (see ../build.sh):
 *   Main   (NBA99_DEBUG=0) for players: logs off unless switched on in
 *          nba99fix.ini, developer diagnostics compiled out, stripped.
 *   Debug  (NBA99_DEBUG=1) for testing: logs on, crash reports (log entry
 *          plus a minidump in crash\), diagnostics available, symbols kept.
 *
 * NBA99_VERSION_* and NBA99_BUILD_STAMP are passed in by build.sh.
 */
#ifndef NBA99_BUILD_H
#define NBA99_BUILD_H

#ifndef NBA99_DEBUG
#define NBA99_DEBUG 0
#endif
#ifndef NBA99_VERSION_MAJOR
#define NBA99_VERSION_MAJOR 0
#define NBA99_VERSION_MINOR 0
#define NBA99_VERSION_PATCH 0
#endif
#ifndef NBA99_BUILD_STAMP
#define NBA99_BUILD_STAMP "local"
#endif

#define NBA99_STR2(x) #x
#define NBA99_STR(x) NBA99_STR2(x)
#define NBA99_VERSION NBA99_STR(NBA99_VERSION_MAJOR) "." NBA99_STR(NBA99_VERSION_MINOR) "." NBA99_STR(NBA99_VERSION_PATCH)
#if NBA99_DEBUG
#define NBA99_BUILD_NAME "Debug"
#else
#define NBA99_BUILD_NAME "Main"
#endif
/* e.g. "0.1.0 Debug (20261028.1215)" */
#define NBA99_VERSION_LINE NBA99_VERSION " " NBA99_BUILD_NAME " (" NBA99_BUILD_STAMP ")"

/* logs default to on in Debug, off in Main ([section] Log=0/1 overrides) */
#define NBA99_LOG_DEFAULT NBA99_DEBUG

#ifndef RC_INVOKED
#include <windows.h>
#include <stdio.h>
#include <string.h>

/*
 * Log sessions (0.7.10): every game start writes its logs to its own folder,
 *   <dir>logs\<YYYY-MM-DD_HHMMSS>_<version>_<Main|Debug>_<computer name>\<name>
 * so logs from earlier starts and builds are never overwritten. The folder name travels in
 * the environment variable NBA99_LOG_SESSION: the launcher sets a new one before each game
 * start (the game's DLLs inherit it); a game started without the launcher makes its own in
 * the first module that logs. logs\latest.txt names the newest session folder.
 */
static __inline void nba99_log_session_new(char *out, size_t size)
{
    SYSTEMTIME t;
    char pc[64] = "PC", *c;
    DWORD n = sizeof pc;
    GetLocalTime(&t);
    if (!GetComputerNameA(pc, &n)) strcpy(pc, "PC");
    for (c = pc; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) *c = '-';
    _snprintf(out, size, "%04u-%02u-%02u_%02u%02u%02u_%s_%s_%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
              NBA99_VERSION, NBA99_BUILD_NAME, pc);
    out[size - 1] = 0;
    SetEnvironmentVariableA("NBA99_LOG_SESSION", out);
}

static __inline void nba99_log_session(char *out, size_t size)
{
    DWORD n = GetEnvironmentVariableA("NBA99_LOG_SESSION", out, (DWORD)size);
    if (!n || n >= size) nba99_log_session_new(out, size);
}

/* "<dir>logs\<session>\<name>", creating the folders; dir ends with a backslash */
static __inline void nba99_log_path(const char *dir, const char *name, char *out, size_t size)
{
    char ses[128];
    nba99_log_session(ses, sizeof ses);
    _snprintf(out, size, "%slogs", dir);
    out[size - 1] = 0;
    CreateDirectoryA(out, NULL);
    _snprintf(out, size, "%slogs\\%s", dir, ses);
    out[size - 1] = 0;
    if (CreateDirectoryA(out, NULL)) {   /* a new session: note it in logs\latest.txt */
        char lt[MAX_PATH];
        HANDLE h;
        DWORD w;
        _snprintf(lt, sizeof lt, "%slogs\\latest.txt", dir);
        lt[sizeof lt - 1] = 0;
        h = CreateFileA(lt, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { WriteFile(h, ses, (DWORD)strlen(ses), &w, NULL); WriteFile(h, "\r\n", 2, &w, NULL); CloseHandle(h); }
    }
    _snprintf(out, size, "%slogs\\%s\\%s", dir, ses, name);
    out[size - 1] = 0;
}
#endif

#endif
