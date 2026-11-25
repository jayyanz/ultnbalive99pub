/*
 * NBALive99.exe - launcher for the NBA Live 99 compatibility build.
 *
 * Starts the unmodified game exe (NBAWIN.EXE v1.1) from its own folder with the
 * "3dfx" argument, and loads nba99core.dll into it before any game code runs:
 * the process is created suspended and LoadLibraryA is queued as an APC on its
 * main thread, which Windows runs right after the exe's own imports are loaded
 * and before the exe's entry point. Works the same under Wine/Proton.
 *
 * "3dfx" (check_commandline in the exe, any case) makes the game use the THRASH
 * driver 3rash\voodooa.dll without reading the registry. The game also takes
 * NOSOUND and WINDOW, and reads more arguments from the environment variable
 * NBALIVE. It clears THRASH_DRIVERNAME from its environment at start (main), so
 * that variable cannot pick the driver.
 *
 * Exe check (size and SHA-256): the v1.1 NBAWIN.EXE from EA's patch is the only
 * exe the fixes are made for. The disc's v1.0 exe and the known no-CD exe are
 * refused with a message saying what to do; any other exe gets a warning.
 *
 * Any extra command line arguments are passed on to the game.
 * Releases ship nba99fix.ini.default; the launcher copies it to nba99fix.ini only
 * when there is no nba99fix.ini yet, so updates keep the user's settings.
 * Settings (optional) in nba99fix.ini, section [launcher]:
 *   Exe=NBAWIN.EXE        game exe to start
 *   Args=3dfx             arguments given to the game before any of ours
 *   Log=0/1               logs\launcher.log (default on in the Debug build)
 *   WatchGame=1           Debug build: report a crash (exception exit code)
 *                         with a message when the game closes
 *
 * Both builds wait for the game to close (no window, no CPU use) and log its
 * exit code; the Debug build reports a crash with a message.
 *
 * Both builds check that nba99core.dll, glide2x.dll and eacsnd.dll carry the
 * same version and build (Main or Debug) as the launcher, and warn if not (EA's
 * own eacsnd.dll, left in place of ours, has no version).
 */
#define WIN32_LEAN_AND_MEAN
#include "nba99_cxx.h"   /* C++ build settings, first (0.7.15) */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "nba99_build.h"

NBA99_C_BEGIN
static char g_dir[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void lg(const char *fmt, ...)
{
    char buf[1024];
    int n;
    va_list ap;
    DWORD w;
    SYSTEMTIME st;
    if (g_log == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&st);
    n = _snprintf(buf, 32, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    n += _vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_log, buf, (DWORD)n, &w, NULL);
}

/* FileVersion string of one of our files ("" if it has none) */
static void file_version(const char *name, char *out, int cap)
{
    char path[MAX_PATH];
    DWORD h, n;
    void *buf, *val;
    UINT len;
    out[0] = 0;
    _snprintf(path, sizeof path, "%s%s", g_dir, name);
    n = GetFileVersionInfoSizeA(path, &h);
    if (!n || !(buf = HeapAlloc(GetProcessHeap(), 0, n))) return;
    if (GetFileVersionInfoA(path, 0, n, buf) && VerQueryValueA(buf, "\\StringFileInfo\\0c0904b0\\FileVersion", &val, &len) && len)
        _snprintf(out, cap, "%s", (char *)val);
    out[cap - 1] = 0;
    HeapFree(GetProcessHeap(), 0, buf);
}

/* warn when our files come from different versions or builds; returns 0 to stop */
static int check_versions(void)
{
    static const char *files[] = { "nba99core.dll", "glide2x.dll", "eacsnd.dll" };   /* EA's eacsnd.dll has no version: warned */
    char msg[1500], v[128];
    int i, bad = 0, len;
    len = _snprintf(msg, sizeof msg, "These files do not match this launcher (%s):\n\n", NBA99_VERSION_LINE);
    for (i = 0; i < 3; i++) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path, "%s%s", g_dir, files[i]);
        file_version(files[i], v, sizeof v);
        lg("%-14s %s", files[i], GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? "missing" : v[0] ? v : "no version");
        if (strcmp(v, NBA99_VERSION_LINE) != 0) {
            bad = 1;
            len += _snprintf(msg + len, sizeof msg - len, "%s: %s\n", files[i],
                             GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES ? "missing" : v[0] ? v : "older file, no version");
        }
    }
    if (!bad) return 1;
    _snprintf(msg + len, sizeof msg - len, "\nCopy all files from one release (Main or Debug) into the game folder.\n"
              "Start the game anyway?");
    msg[sizeof msg - 1] = 0;
    return MessageBoxA(NULL, msg, "NBA Live 99 launcher", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

#if NBA99_DEBUG
/* newest crash\*.txt written since 'since' (file time), or "" */
static void newest_crash_report(const FILETIME *since, char *out, int cap)
{
    WIN32_FIND_DATAA fd;
    FILETIME best = *since;
    char pat[MAX_PATH];
    HANDLE h;
    out[0] = 0;
    _snprintf(pat, sizeof pat, "%scrash\\nba99_*.txt", g_dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) {
            best = fd.ftLastWriteTime;
            _snprintf(out, cap, "crash\\%s", fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#endif

/* ------------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4), our own small version: no new imports               */
/* ------------------------------------------------------------------------- */
typedef struct { unsigned int h[8]; unsigned char buf[64]; unsigned long long len; unsigned int n; } sha256_ctx;
static const unsigned int k_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha256_ctx *c, const unsigned char *p)
{
    unsigned int w[64], a, b, d, e, f, g, h, cc, t1, t2;
    int i;
    for (i = 0; i < 16; i++) w[i] = (unsigned int)p[4 * i] << 24 | (unsigned int)p[4 * i + 1] << 16 | (unsigned int)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_sha_k[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha_init(sha256_ctx *c)
{
    static const unsigned int iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0;
}
static void sha_update(sha256_ctx *c, const unsigned char *p, size_t n)
{
    c->len += n;
    while (n) {
        size_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k); c->n += (unsigned int)k; p += k; n -= k;
        if (c->n == 64) { sha_block(c, c->buf); c->n = 0; }
    }
}
static void sha_final(sha256_ctx *c, char hex[65])
{
    unsigned long long bits = c->len * 8;
    unsigned char pad = 0x80, z = 0, l[8];
    int i;
    sha_update(c, &pad, 1);
    while (c->n != 56) sha_update(c, &z, 1);
    for (i = 0; i < 8; i++) l[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_update(c, l, 8);
    for (i = 0; i < 8; i++) _snprintf(hex + 8 * i, 9, "%08x", c->h[i]);
    hex[64] = 0;
}

/* size and SHA-256 of a file; 0 if it cannot be read */
static int file_sha256(const char *path, DWORD *size, char hex[65])
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    static unsigned char buf[65536];
    sha256_ctx c;
    DWORD rd;
    if (f == INVALID_HANDLE_VALUE) return 0;
    *size = GetFileSize(f, NULL);
    sha_init(&c);
    while (ReadFile(f, buf, sizeof buf, &rd, NULL) && rd) sha_update(&c, buf, rd);
    CloseHandle(f);
    sha_final(&c, hex);
    return 1;
}

#define EXE_V11_SIZE 2488320u
#define EXE_V11_SHA  "3563b929b75379df3d93468d1c957b742f17b2a708098b7cfbeeac3fda51287d"
#define EXE_V10_SIZE 2478592u
#define EXE_V10_SHA  "bd180729487b88ace5582597c728649d2ff4c7e09908bd3eb227bf78365cb1a1"
#define EXE_NOCD_SHA "c8a63d265a77b8ae59855e2590a1ea1cd35d87ccedcecc7b95f041d9dcccfda0"

/* 1 = start the game, 0 = stop */
static int check_exe(const char *exe)
{
    DWORD size = 0;
    char hex[65], msg[1024];
    if (!file_sha256(exe, &size, hex)) {
        lg("exe check: could not read %s (error %lu)", exe, GetLastError());
        return 1;   /* CreateProcess will report it */
    }
    lg("exe check: %s, %lu bytes, SHA-256 %s", exe, size, hex);
    if (size == EXE_V11_SIZE && !strcmp(hex, EXE_V11_SHA)) { lg("exe check: NBAWIN.EXE v1.1 (EA patch), the reference exe"); return 1; }
    if (size == EXE_V10_SIZE && !strcmp(hex, EXE_V10_SHA)) {
        lg("exe check: NBAWIN.EXE v1.0 from the disc: refused");
        MessageBoxA(NULL, "This NBAWIN.EXE is version 1.0, as it comes on the disc.\n\n"
                          "Apply EA's v1.1 patch first: run nbalive99patch_english.exe (or open it with 7-Zip) and "
                          "extract it into the game folder, then start NBALive99.exe again. The v1.1 NBAWIN.EXE is "
                          "2,488,320 bytes. See the README, Install.", "NBA Live 99 launcher", MB_ICONERROR);
        return 0;
    }
    if (!strcmp(hex, EXE_NOCD_SHA)) {
        lg("exe check: the known no-CD copy of the v1.1 exe: refused");
        MessageBoxA(NULL, "This NBAWIN.EXE is a changed (no-CD) copy of the v1.1 exe, as found in some repacks.\n\n"
                          "These fixes need EA's own v1.1 NBAWIN.EXE and remove the CD check themselves. Extract "
                          "NBAWIN.EXE from EA's v1.1 patch (nbalive99patch_english.exe) into the game folder, then "
                          "start NBALive99.exe again.", "NBA Live 99 launcher", MB_ICONERROR);
        return 0;
    }
    lg("exe check: unknown exe");
    _snprintf(msg, sizeof msg, "%s is not the v1.1 NBAWIN.EXE these fixes are made for (%lu bytes; the v1.1 exe is "
              "2,488,320 bytes from EA's v1.1 patch).\n\nOnly the general fixes would apply: no CD check fix. "
              "Start the game anyway?", exe, size);
    msg[sizeof msg - 1] = 0;
    return MessageBoxA(NULL, msg, "NBA Live 99 launcher", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
}

static void fail(const char *fmt, const char *a, DWORD err)
{
    char msg[1024];
    _snprintf(msg, sizeof msg, fmt, a, err);
    msg[sizeof msg - 1] = 0;
    MessageBoxA(NULL, msg, "NBA Live 99 launcher", MB_ICONERROR);
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Settings survive updates: releases ship nba99fix.ini.default, never nba99fix.ini.
   On the first start (no nba99fix.ini yet) the user's nba99fix.ini is made from the
   default; after that it is never replaced, so copying a new release over an
   install keeps the user's settings (including the in-game Enhanced Options).
   Returns 0 kept the existing file, 1 created, 2 neither file exists,
   -1 the copy failed (the game then runs on built-in defaults). */
static int make_ini(const char *ini)
{
    char def[MAX_PATH];
    if (file_exists(ini)) return 0;
    _snprintf(def, sizeof def, "%s.default", ini);
    def[sizeof def - 1] = 0;
    if (!file_exists(def)) return 2;
    return CopyFileA(def, ini, TRUE) ? 1 : -1;   /* TRUE: never overwrite */
}

/* the part of our command line after the program name */
static const char *passthrough_args(void)
{
    const char *c = GetCommandLineA();
    if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
    else while (*c && *c != ' ' && *c != '\t') c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    char ini[MAX_PATH], exe[MAX_PATH], exe_name[MAX_PATH], args[256], cmdline[2048], dll[MAX_PATH], evname[64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    LPVOID remote;
    SIZE_T wrote;
    HANDLE ready;
    FARPROC loadlib;
    char *s;
    DWORD wr;
    int ini_made;
    (void)inst; (void)prev; (void)cmd; (void)show;

    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    _snprintf(ini, sizeof ini, "%snba99fix.ini", g_dir);
    ini_made = make_ini(ini);
    if (GetPrivateProfileIntA("launcher", "Log", NBA99_LOG_DEFAULT, ini)) {
        char lp[MAX_PATH];
        nba99_log_path(g_dir, "launcher.log", lp, sizeof lp);
        g_log = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    lg("NBA Live 99 launcher %s, folder %s", NBA99_VERSION_LINE, g_dir);
    lg("settings: %s", ini_made == 1 ? "nba99fix.ini created from nba99fix.ini.default (first start)" :
                       ini_made < 0 ? "no nba99fix.ini and it could not be created from nba99fix.ini.default (built-in defaults)" :
                       ini_made == 2 ? "no nba99fix.ini or nba99fix.ini.default (built-in defaults)" : "using the existing nba99fix.ini");
    if (!check_versions()) { lg("stopped: file versions do not match"); return 1; }

    GetPrivateProfileStringA("launcher", "Exe", "", exe_name, sizeof exe_name, ini);
    if (exe_name[0]) {
        _snprintf(exe, sizeof exe, "%s%s", g_dir, exe_name);
    } else {
        _snprintf(exe, sizeof exe, "%sNBAWIN.EXE", g_dir);
    }
    if (!file_exists(exe)) {
        fail("Could not find the game exe (NBAWIN.EXE) in\n%s\n\nCopy NBALive99.exe and the other release files into "
             "the game folder, next to NBAWIN.EXE.", g_dir, 0);
        return 1;
    }
    if (!check_exe(exe)) { lg("stopped: exe refused"); return 1; }
    _snprintf(dll, sizeof dll, "%snba99core.dll", g_dir);
    if (!file_exists(dll)) {
        fail("nba99core.dll is missing from\n%s", g_dir, 0);
        return 1;
    }
    GetPrivateProfileStringA("launcher", "Args", "3dfx", args, sizeof args, ini);
    _snprintf(cmdline, sizeof cmdline, "\"%s\" %s %s", exe, args, passthrough_args());
    cmdline[sizeof cmdline - 1] = 0;

    {
        DWORD code = 0;
        {   /* each game start logs to its own folder (logs\<date>_<time>_<version>_<build>_<PC>) */
            char ses[128];
            nba99_log_session(ses, sizeof ses);
            lg("logs for this start: logs\\%s", ses);
        }
        lg("starting: %s", cmdline);
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (!CreateProcessA(exe, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, g_dir, &si, &pi)) {
            fail("Could not start %s (error %lu)", exe, GetLastError());
            return 1;
        }

        /* the core DLL signals this event once it has finished its setup */
        _snprintf(evname, sizeof evname, "NBA99CORE_READY_%lu", pi.dwProcessId);
        ready = CreateEventA(NULL, TRUE, FALSE, evname);

        remote = VirtualAllocEx(pi.hProcess, NULL, strlen(dll) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        loadlib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
        if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll, strlen(dll) + 1, &wrote) ||
            !QueueUserAPC((PAPCFUNC)loadlib, pi.hThread, (ULONG_PTR)remote)) {
            DWORD e = GetLastError();
            TerminateProcess(pi.hProcess, 1);
            fail("Could not load nba99core.dll into the game%s (error %lu)", "", e);
            return 1;
        }
        ResumeThread(pi.hThread);

        wr = ready ? WaitForSingleObject(ready, 15000) : WAIT_FAILED;
        if (wr != WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &code);
            if (code == STILL_ACTIVE)
                MessageBoxA(NULL, "The game started but nba99core.dll did not report in.\n"
                                  "The CD check and install folder fixes may not be active.\n"
                                  "Set Log=1 under [nba99] in nba99fix.ini and see nba99core.log in the logs folder.",
                            "NBA Live 99 launcher", MB_ICONWARNING);
        }
        lg("game started (process %lu), core %s", pi.dwProcessId, wr == WAIT_OBJECT_0 ? "ready" : "did not report in");

        /* both builds wait for the game (no window, no CPU use) and log how it closed */
        {
#if NBA99_DEBUG
            FILETIME start;
            SYSTEMTIME now;
            GetSystemTime(&now);
            SystemTimeToFileTime(&now, &start);
            {   /* reports are stamped to the second: allow for that */
                ULARGE_INTEGER u; u.LowPart = start.dwLowDateTime; u.HighPart = start.dwHighDateTime;
                u.QuadPart -= 10000000ULL * 120; start.dwLowDateTime = u.LowPart; start.dwHighDateTime = u.HighPart;
            }
#endif
            WaitForSingleObject(pi.hProcess, INFINITE);
            code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            lg("game closed, exit code 0x%08lx", code);
#if NBA99_DEBUG
            if (code >= 0xC0000000u && GetPrivateProfileIntA("launcher", "WatchGame", 1, ini)) {
                char rep[MAX_PATH], msg[1200];
                newest_crash_report(&start, rep, sizeof rep);
                lg("crash report: %s", rep[0] ? rep : "none written");
                _snprintf(msg, sizeof msg, "NBA Live 99 crashed (exception 0x%08lX).\n\n%s%s%s"
                          "The logs folder has the details of this session.", code,
                          rep[0] ? "Crash report: " : "No crash report was written.\n", rep[0] ? rep : "",
                          rep[0] ? " (and the .dmp file with the same name)\n" : "");
                msg[sizeof msg - 1] = 0;
                MessageBoxA(NULL, msg, "NBA Live 99 (Debug build)", MB_ICONERROR);
            }
#endif
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (ready) CloseHandle(ready);
        ready = NULL;
    }
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    return 0;
}
NBA99_C_END
