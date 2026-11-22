/*
 * thrash_log (3rash\voodoologa.dll): Debug tool. Sits between NBAWIN.EXE and
 * EA's THRASH 3D driver (3rash\VOODOOA.DLL) and forwards every call unchanged.
 *
 * NBA Live 99 draws through EA's THRASH layer: the exe loads one driver DLL from
 * 3rash\ and calls its 34 stdcall exports (THRASH_init, setvideomode, drawtri,
 * pageflip, lockwindow, writerect, ...). This proxy exports the same 34 names,
 * loads the real 3rash\VOODOOA.DLL and logs:
 *   - every call to the setup functions (about, init, is, restore,
 *     selectdisplay, setvideomode, window, clip), with arguments, result and
 *     the exe address that made the call;
 *   - the first 40 calls of every other function (lockwindow, unlockwindow,
 *     readrect and writerect: the first 200), the same way;
 *   - one TICK line for every second with calls in it: calls per function and
 *     the pageflip interval (min avg max ms);
 *   - call counts per function at exit.
 * Game exe addresses are plain virtual addresses (they match the function name
 * list from tools/watcom_names.py); other modules are shown as name+offset.
 *
 * Selected by the core: [nba99] ThrashLog=1 in nba99fix.ini (Debug build) makes
 * the game load 3rash\voodoologa.dll instead of 3rash\voodooa.dll. The log is
 * thrash_log.txt in the session's log folder (logs\<session>\).
 * Optional, [thrashlog] in nba99fix.ini:
 *   Real=voodooa.dll   driver in 3rash\ to forward to (default voodooa.dll)
 *   First=40           detailed calls per function (0 = setup calls only)
 *
 * C++ in the restricted subset like the rest of the Debug build (README "C++ rules");
 * its own code sits in NBA99_C_BEGIN / NBA99_C_END so the exports and the names the
 * asm thunks use (g_real and the log entry points) keep their C names.
 */
#include "nba99_cxx.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "nba99_build.h"

NBA99_C_BEGIN
#include "thrash_thunks.inc"
void *g_real[THRASH_FN_COUNT];       /* read by the thunks */

static HMODULE g_real_mod;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static char g_buf[1 << 16];
static int g_buf_n;
static LARGE_INTEGER g_qpf, g_qpc0;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static int g_first = 40;
static volatile LONG g_count[THRASH_FN_COUNT], g_sec_count[THRASH_FN_COUNT];
static int g_always[THRASH_FN_COUNT], g_limit[THRASH_FN_COUNT];
static int g_pageflip = -1;
static double g_pf_last, g_pf_min = 1e9, g_pf_max, g_pf_sum;
static long g_pf_n;
static volatile LONG g_stop;
static HANDLE g_ticker;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void flush_locked(void)
{
    DWORD w;
    if (g_buf_n && g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, g_buf, (DWORD)g_buf_n, &w, NULL);
    g_buf_n = 0;
}

static void log_line(const char *fmt, ...)
{
    char line[1024];
    int n;
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = _snprintf(line, 32, "[%10.1f] ", now_ms());
    va_start(ap, fmt);
    n += _vsnprintf(line + n, sizeof line - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    EnterCriticalSection(&g_cs);
    if (g_buf_n + n > (int)sizeof g_buf) flush_locked();
    memcpy(g_buf + g_buf_n, line, n);
    g_buf_n += n;
    LeaveCriticalSection(&g_cs);
}

/* "EXE!0x004a1234" for the game exe, "name.dll+0x1234" for other modules */
static void where(uint32_t a, char *out, int cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *b;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)a, &m) || !m) {
        _snprintf(out, cap, "0x%08lx", (unsigned long)a);
        return;
    }
    if (m == GetModuleHandleA(NULL)) { _snprintf(out, cap, "EXE!0x%08lx", (unsigned long)a); return; }
    GetModuleFileNameA(m, path, sizeof path);
    b = strrchr(path, '\\');
    _snprintf(out, cap, "%s+0x%lx", b ? b + 1 : path, (unsigned long)(a - (uint32_t)(uintptr_t)m));
}

/* per-thread stack of the game's return addresses while the real driver runs */
typedef struct { int depth; uint32_t ret[32]; char logged[32]; } tstack;

static tstack *ts_get(void)
{
    tstack *t = (tstack *)TlsGetValue(g_tls);
    if (!t) {
        t = (tstack *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
        TlsSetValue(g_tls, t);
    }
    return t;
}

static void name_of(int i, char *out, int cap)
{
    const char *n = g_fn_name[i] + 1;     /* skip the leading underscore */
    int k = 0;
    while (n[k] && n[k] != '@' && k < cap - 1) { out[k] = n[k]; k++; }
    out[k] = 0;
}

/* called by every thunk before the real call; stack[0] = return address, stack[1..] = args */
void tl_enter(int i, uint32_t *stack)
{
    tstack *t = ts_get();
    LONG c = InterlockedIncrement(&g_count[i]);
    int logit;
    InterlockedIncrement(&g_sec_count[i]);
    if (i == g_pageflip) {
        double now = now_ms();
        if (g_pf_last > 0) {
            double d = now - g_pf_last;
            if (d < g_pf_min) g_pf_min = d;
            if (d > g_pf_max) g_pf_max = d;
            g_pf_sum += d;
            g_pf_n++;
        }
        g_pf_last = now;
    }
    logit = g_always[i] || c <= g_limit[i];
    if (logit) {
        char nm[40], at[96], args[200];
        int k, n = 0, na = g_fn_argbytes[i] / 4;
        name_of(i, nm, sizeof nm);
        where(stack[0], at, sizeof at);
        args[0] = 0;
        for (k = 0; k < na && n < (int)sizeof args - 24; k++) {
            uint32_t v = stack[1 + k];
            n += _snprintf(args + n, sizeof args - n, v < 0x10000 ? "%s%lu" : "%s0x%08lx", k ? ", " : "", (unsigned long)v);
        }
        log_line("%-20s #%-5ld (%s)  from %s  tid %lu", nm, (long)c, args, at, GetCurrentThreadId());
    }
    if (t && t->depth < 32) {
        t->ret[t->depth] = stack[0];
        t->logged[t->depth] = (char)logit;
    }
    if (t) t->depth++;
}

/* called by every thunk after the real call; returns the game's return address */
uint32_t tl_leave(int i, uint32_t result)
{
    tstack *t = ts_get();
    uint32_t ret = 0;
    int logged = 0;
    if (t && t->depth > 0) {
        t->depth--;
        if (t->depth < 32) { ret = t->ret[t->depth]; logged = t->logged[t->depth]; }
    }
    if (logged && g_fn_argbytes[i] >= 0) {
        char nm[40];
        name_of(i, nm, sizeof nm);
        log_line("%-20s   -> 0x%08lx (%ld)", nm, (unsigned long)result, (long)result);
    }
    if (!ret) {   /* deeper than 32 nested calls: cannot happen with this driver */
        log_line("FATAL: return address lost in %s", g_fn_name[i]);
        EnterCriticalSection(&g_cs); flush_locked(); LeaveCriticalSection(&g_cs);
        ExitProcess(3);
    }
    return ret;
}

static DWORD WINAPI ticker(LPVOID arg)
{
    (void)arg;
    while (!g_stop) {
        int i, n = 0, any = 0;
        char line[1400];
        Sleep(1000);
        n = _snprintf(line, sizeof line, "TICK");
        for (i = 0; i < THRASH_FN_COUNT; i++) {
            LONG c = InterlockedExchange(&g_sec_count[i], 0);
            if (c && n < (int)sizeof line - 48) {
                char nm[40];
                name_of(i, nm, sizeof nm);
                n += _snprintf(line + n, sizeof line - n, " %s=%ld", nm + 7, (long)c);   /* without "THRASH_" */
                any = 1;
            }
        }
        if (g_pf_n && n < (int)sizeof line - 80) {
            n += _snprintf(line + n, sizeof line - n, "  | pageflip ms %.1f %.1f %.1f", g_pf_min, g_pf_sum / g_pf_n, g_pf_max);
            g_pf_min = 1e9; g_pf_max = 0; g_pf_sum = 0; g_pf_n = 0;
        }
        if (any) log_line("%s", line);
        EnterCriticalSection(&g_cs);
        flush_locked();
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

static void start(HINSTANCE self)
{
    char dir[MAX_PATH], ini[MAX_PATH], real[MAX_PATH], name[64], path[MAX_PATH], *s;
    int i, missing = 0;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    InitializeCriticalSection(&g_cs);
    g_tls = TlsAlloc();
    GetModuleFileNameA(NULL, dir, sizeof dir);           /* game folder */
    s = strrchr(dir, '\\');
    if (s) s[1] = 0;
    _snprintf(ini, sizeof ini, "%snba99fix.ini", dir);
    nba99_log_path(dir, "thrash_log.txt", path, sizeof path);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_first = (int)GetPrivateProfileIntA("thrashlog", "First", 40, ini);
    GetPrivateProfileStringA("thrashlog", "Real", "voodooa.dll", name, sizeof name, ini);
    _snprintf(real, sizeof real, "%s3rash\\%s", dir, name);
    log_line("thrash_log %s (THRASH call log), game folder %s", NBA99_VERSION_LINE, dir);
    {
        char me[MAX_PATH];
        GetModuleFileNameA(self, me, sizeof me);
        log_line("this proxy: %s; command line: %s", me, GetCommandLineA());
    }
    for (i = 0; i < THRASH_FN_COUNT; i++) {
        char nm[40];
        name_of(i, nm, sizeof nm);
        g_always[i] = !strcmp(nm, "THRASH_about") || !strcmp(nm, "THRASH_init") || !strcmp(nm, "THRASH_is") ||
                      !strcmp(nm, "THRASH_restore") || !strcmp(nm, "THRASH_selectdisplay") ||
                      !strcmp(nm, "THRASH_setvideomode") || !strcmp(nm, "THRASH_window") || !strcmp(nm, "THRASH_clip");
        g_limit[i] = (!strcmp(nm, "THRASH_lockwindow") || !strcmp(nm, "THRASH_unlockwindow") ||
                      !strcmp(nm, "THRASH_readrect") || !strcmp(nm, "THRASH_writerect")) ? g_first * 5 : g_first;
        if (!strcmp(nm, "THRASH_pageflip")) g_pageflip = i;
    }
    g_real_mod = LoadLibraryA(real);
    if (!g_real_mod) {
        log_line("FATAL: could not load the real driver %s (error %lu)", real, GetLastError());
        EnterCriticalSection(&g_cs); flush_locked(); LeaveCriticalSection(&g_cs);
        MessageBoxA(NULL, "thrash_log could not load the real THRASH driver (3rash\\voodooa.dll).\n"
                          "See thrash_log.txt in the logs folder.", "NBA Live 99 thrash_log", MB_ICONERROR);
        ExitProcess(2);
    }
    log_line("real driver: %s loaded at 0x%08lx", real, (unsigned long)(uintptr_t)g_real_mod);
    for (i = 0; i < THRASH_FN_COUNT; i++) {
        g_real[i] = (void *)GetProcAddress(g_real_mod, g_fn_name[i]);
        if (!g_real[i]) { log_line("WARNING: real driver has no %s", g_fn_name[i]); missing++; }
    }
    log_line("%d of %d functions found in the real driver", THRASH_FN_COUNT - missing, THRASH_FN_COUNT);
    EnterCriticalSection(&g_cs); flush_locked(); LeaveCriticalSection(&g_cs);
    g_ticker = CreateThread(NULL, 0, ticker, NULL, 0, NULL);
}

static void stop(void)
{
    int i;
    char line[1600];
    int n = _snprintf(line, sizeof line, "call counts at exit:");
    g_stop = 1;
    for (i = 0; i < THRASH_FN_COUNT; i++)
        if (g_count[i] && n < (int)sizeof line - 48) {
            char nm[40];
            name_of(i, nm, sizeof nm);
            n += _snprintf(line + n, sizeof line - n, " %s=%ld", nm + 7, (long)g_count[i]);
        }
    log_line("%s", line);
    EnterCriticalSection(&g_cs);
    flush_locked();
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_cs);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        start(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        stop();
    }
    return TRUE;
}

NBA99_C_END
