/*
 * Debug build only (included by nba99core.cpp when NBA99_DEBUG=1):
 *  - system report at start-up (versions of our files and SDL3, Windows or
 *    Wine version, CPUs, memory, display mode, the settings in nba99fix.ini);
 *  - crash report: exception, registers and call stack in nba99core.log,
 *    the same in crash\nba99_<date>_<time>.txt, and a minidump next to it
 *    (CrashLog=1, CrashDumpFull=0/1, CrashTest=N to try it);
 *  - ThreadDump=N and WatchAddr=0x... for hangs and state changes.
 */

/* ------------------------------------------------------------------------- */
/* diagnostics: ThreadDump=N logs where every thread is, every N seconds      */
/* (EIP plus likely return addresses; game exe addresses are plain VAs).     */
/* ------------------------------------------------------------------------- */
#include <tlhelp32.h>
#define TD_STACK 1024
#define TD_MAX 48
typedef struct { DWORD tid; int ok, prio; uint32_t start; CONTEXT ctx; SIZE_T n; uint32_t stk[TD_STACK]; } td_sample;
static td_sample g_td[TD_MAX];
static DWORD g_td_tid;
typedef LONG (WINAPI *NtQIT_fn)(HANDLE, int, PVOID, ULONG, PULONG);
static NtQIT_fn p_NtQIT;

static int td_desc(uint32_t a, char *out, int cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *b;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)a, &m) || !m)
        return _snprintf(out, cap, "0x%08lx", (unsigned long)a);
    GetModuleFileNameA(m, path, sizeof path);
    b = strrchr(path, '\\'); b = b ? b + 1 : path;
    if ((uintptr_t)m == 0x400000) return _snprintf(out, cap, "EXE!0x%08lx", (unsigned long)a);
    return _snprintf(out, cap, "%s+0x%lx", b, (unsigned long)(a - (uint32_t)(uintptr_t)m));
}

static int td_exec(uint32_t a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000 || !VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

static int td_after_call(uint32_t a)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)a;
    if (!td_exec(a - 7)) return 0;
    if (p[-5] == 0xE8) return 1;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return 1;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return 1;
    if (p[-7] == 0x2E && p[-6] == 0xFF) return 1;
    return 0;
}

static void td_sample_one(td_sample *s)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s->tid);
    s->ok = 0; s->n = 0; s->start = 0; s->prio = 99;
    if (!h) return;
    s->prio = GetThreadPriority(h);
    if (p_NtQIT) { ULONG r; uint32_t sa = 0; if (p_NtQIT(h, 9, &sa, sizeof sa, &r) == 0) s->start = sa; }
    memset(&s->ctx, 0, sizeof s->ctx);
    s->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(h) != (DWORD)-1) {
        if (GetThreadContext(h, &s->ctx)) {
            MEMORY_BASIC_INFORMATION mi;
            SIZE_T want = sizeof s->stk, got = 0;
            s->ok = 1;
            if (VirtualQuery((LPCVOID)(uintptr_t)s->ctx.Esp, &mi, sizeof mi)) {
                SIZE_T avail = (SIZE_T)((char *)mi.BaseAddress + mi.RegionSize - (char *)(uintptr_t)s->ctx.Esp);
                if (avail < want) want = avail;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)s->ctx.Esp, s->stk, want, &got))
                    s->n = got / 4;
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

/* CPU time per thread since the previous dump (to see who is busy) */
static struct { DWORD tid; ULONGLONG cpu; } g_td_cpu[128];
static int g_ntd_cpu;
static double td_cpu_ms(DWORD tid)
{
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    FILETIME c, e, k, u;
    ULONGLONG now, prev = 0;
    int i;
    if (!h) return -1;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) { CloseHandle(h); return -1; }
    CloseHandle(h);
    now = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
    for (i = 0; i < g_ntd_cpu && g_td_cpu[i].tid != tid; i++) {}
    if (i < g_ntd_cpu) prev = g_td_cpu[i].cpu;
    else if (g_ntd_cpu < 128) { g_td_cpu[g_ntd_cpu].tid = tid; i = g_ntd_cpu++; }
    if (i < 128) g_td_cpu[i].cpu = now;
    return (double)(now - prev) / 10000.0;
}

static int td_is_wine(void)
{
    return GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
}

/* log every thread's position; all=1 also lists the threads we did not
   create (driver, SDL, audio) through a ToolHelp snapshot (not under Wine,
   where snapshots can block while the game is stuck) */
static void td_dump(int all, const char *tag)
{
    int n = 0, i;
    SIZE_T k;
    for (i = 0; i < g_nknown && i < 64 && n < TD_MAX; i++) g_td[n++].tid = g_known_tid[i];
    if (all && !td_is_wine()) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            if (Thread32First(snap, &te)) do {
                int seen = 0, j;
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                for (j = 0; j < n; j++) if (g_td[j].tid == te.th32ThreadID) seen = 1;
                if (!seen && n < TD_MAX) g_td[n++].tid = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
            CloseHandle(snap);
        }
    }
    for (i = 0; i < n; i++) td_sample_one(&g_td[i]);
    for (i = 0; i < n; i++) {
        char line[2000];
        int len = 0, found = 0;
        td_sample *s = &g_td[i];
        double cpu = td_cpu_ms(s->tid);
        len += _snprintf(line, sizeof line, "%sTHREAD %5lu prio %3d cpu %6.1f ms start ", tag, s->tid, s->prio, cpu);
        len += td_desc(s->start, line + len, sizeof line - len);
        if (!s->ok) { LOG("%s: could not sample", line); continue; }
        len += _snprintf(line + len, sizeof line - len, " eip ");
        len += td_desc((uint32_t)s->ctx.Eip, line + len, sizeof line - len);
        len += _snprintf(line + len, sizeof line - len, " |");
        for (k = 0; k < s->n && found < 16 && len < (int)sizeof line - 80; k++)
            if (td_exec(s->stk[k]) && td_after_call(s->stk[k])) {
                line[len++] = ' ';
                len += td_desc(s->stk[k], line + len, sizeof line - len);
                found++;
            }
        LOG("%s", line);
    }
}

static DWORD WINAPI td_thread(LPVOID arg)
{
    int secs = (int)(intptr_t)arg;
    for (;;) {
        Sleep(secs * 1000);
        td_dump(0, "");
    }
    return 0;
}

/* crash\nba99_<date>_<time>.txt and .dmp, for the first crash only */
typedef BOOL (WINAPI *MiniDumpWriteDump_fn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
typedef struct { DWORD ThreadId; EXCEPTION_POINTERS *ExceptionPointers; BOOL ClientPointers; } mdei_t;
static LONG g_crash_saved;
static void crash_save(EXCEPTION_POINTERS *ep, const char *line)
{
    char dir[MAX_PATH], path[MAX_PATH], base[MAX_PATH];
    SYSTEMTIME st;
    HANDLE f;
    DWORD w;
    HMODULE dbg;
    if (InterlockedExchange(&g_crash_saved, 1)) return;
    GetLocalTime(&st);
    _snprintf(dir, sizeof dir, "%scrash", g_exe_dir);
    CreateDirectoryA(dir, NULL);
    _snprintf(base, sizeof base, "%s\\nba99_%04d%02d%02d_%02d%02d%02d", dir, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    _snprintf(path, sizeof path, "%s.txt", base);
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        char hdr[512];
        int n = _snprintf(hdr, sizeof hdr, "NBA Live 99 fix %s crash report\r\n%s\r\n\r\n", NBA99_VERSION_LINE,
                          g_sysline);
        WriteFile(f, hdr, (DWORD)n, &w, NULL);
        WriteFile(f, line, (DWORD)strlen(line), &w, NULL);
        WriteFile(f, "\r\n", 2, &w, NULL);
        CloseHandle(f);
    }
    dbg = LoadLibraryA("dbghelp.dll");
    if (dbg) {
        MiniDumpWriteDump_fn mdw = (MiniDumpWriteDump_fn)GetProcAddress(dbg, "MiniDumpWriteDump");
        _snprintf(path, sizeof path, "%s.dmp", base);
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (mdw && f != INVALID_HANDLE_VALUE) {
            mdei_t ei;
            /* MiniDumpNormal | WithDataSegs | WithHandleData | WithThreadInfo, or WithFullMemory */
            int type = ini_int("CrashDumpFull", 0) ? 0x2 | 0x4 : 0x1 | 0x4 | 0x1000;
            ei.ThreadId = GetCurrentThreadId();
            ei.ExceptionPointers = ep;
            ei.ClientPointers = FALSE;
            if (mdw(GetCurrentProcess(), GetCurrentProcessId(), f, type, &ei, NULL, NULL))
                LOG("crash report saved: %s (and .txt)", path);
            else
                LOG("crash report: minidump failed (error %lu); text report in %s.txt", GetLastError(), base);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
}

/* exceptions raised inside system DLLs are usually probes the caller handles
   (IsBadReadPtr and the like), not crashes */
static int crash_in_system_dll(uint32_t eip)
{
    static const char *sys[] = { "ntdll.dll", "kernel32.dll", "kernelbase.dll" };
    int i;
    for (i = 0; i < 3; i++) {
        HMODULE m = GetModuleHandleA(sys[i]);
        HMODULE at = NULL;
        if (m && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)(uintptr_t)eip, &at) && at == m)
            return 1;
    }
    return 0;
}

/* crash report: the first few access violations are logged with a stack scan */
static LONG g_crash_n;
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char line[2000];
    int len = 0, found = 0;
    uint32_t *sp;
    MEMORY_BASIC_INFORMATION mi;
    SIZE_T k, n = 0;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && er->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        er->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO && er->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (crash_in_system_dll((uint32_t)c->Eip)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crash_n) > 5) return EXCEPTION_CONTINUE_SEARCH;
    len += _snprintf(line, sizeof line, "EXCEPTION 0x%08lx thread %lu at ", (unsigned long)er->ExceptionCode,
                     GetCurrentThreadId());
    len += td_desc((uint32_t)c->Eip, line + len, sizeof line - len);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        len += _snprintf(line + len, sizeof line - len, " (%s 0x%08lx)", er->ExceptionInformation[0] ? "write" : "read",
                         (unsigned long)er->ExceptionInformation[1]);
    len += _snprintf(line + len, sizeof line - len, " eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx |",
                     c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    sp = (uint32_t *)(uintptr_t)c->Esp;
    if (VirtualQuery(sp, &mi, sizeof mi)) n = ((char *)mi.BaseAddress + mi.RegionSize - (char *)sp) / 4;
    if (n > 2048) n = 2048;
    for (k = 0; k < n && found < 20 && len < (int)sizeof line - 80; k++)
        if (td_exec(sp[k]) && td_after_call(sp[k])) {
            line[len++] = ' ';
            len += td_desc(sp[k], line + len, sizeof line - len);
            found++;
        }
    LOG("%s", line);
    crash_save(ep, line);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* diagnostics: WatchAddr=0x... logs every change of a dword in the game (polled every ms) */
static DWORD WINAPI watch_thread(LPVOID arg)
{
    volatile uint32_t *p = (volatile uint32_t *)arg;
    uint32_t last = *p;
    char d[96];
    td_desc(last, d, sizeof d);
    LOG("watch 0x%08lx = 0x%08lx (%s)", (unsigned long)(uintptr_t)p, (unsigned long)last, d);
    timeBeginPeriod(1);
    {
        DWORD t0 = GetTickCount();
        for (;;) {
            uint32_t v = *p;
            int spin = GetTickCount() - t0 < 8000;   /* busy-poll the first seconds: catch short-lived values */
            if (v != last) {
                td_desc(v, d, sizeof d);
                LOG("watch 0x%08lx changed to 0x%08lx (%s)", (unsigned long)(uintptr_t)p, (unsigned long)v, d);
                last = v;
            }
            if (spin) YieldProcessor(); else break;
        }
    }
    for (;;) {
        uint32_t v = *p;
        if (v != last) {
            td_desc(v, d, sizeof d);
            LOG("watch 0x%08lx changed to 0x%08lx (%s)", (unsigned long)(uintptr_t)p, (unsigned long)v, d);
            last = v;
        }
        Sleep(1);
    }
    return 0;
}

/* Unhandled crash: the game's runtime filter ends in UnhandledExceptionFilter
   (the exe's import), which would show the Windows or Wine crash dialog and
   keep the process alive. Save the report and end the process with the
   exception code, so the launcher can tell the player where the report is. */
static LONG WINAPI h_UnhandledExceptionFilter(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0xC0000001u;
    if (ep && !g_crash_saved) {
        char line[200];
        _snprintf(line, sizeof line, "UNHANDLED EXCEPTION 0x%08lx thread %lu at 0x%08lx", (unsigned long)code,
                  GetCurrentThreadId(), (unsigned long)(ep->ContextRecord ? ep->ContextRecord->Eip : 0));
        LOG("%s", line);
        crash_save(ep, line);
    }
    LOG("unhandled exception 0x%08lx: game closed by the crash report", (unsigned long)code);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* The game installs its own top-level filter (Watcom runtime). Keep ours on
   top and call the game's from it: if the game's filter handles the
   exception, fine; otherwise report and close. */
static LPTOP_LEVEL_EXCEPTION_FILTER g_game_filter;
static LONG WINAPI top_filter(EXCEPTION_POINTERS *ep)
{
    if (g_game_filter) {
        LONG r = g_game_filter(ep);
        if (r == EXCEPTION_CONTINUE_EXECUTION) return r;
    }
    return h_UnhandledExceptionFilter(ep);
}
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI h_SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER f)
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_game_filter;
    g_game_filter = f;
    LOG("crash report: game set its exception filter 0x%08lx (kept under ours)", (unsigned long)(uintptr_t)f);
    return prev;
}

/* CrashTest=N: crash on purpose after N seconds, to try the crash report */
static DWORD WINAPI crash_test_thread(LPVOID arg)
{
    Sleep((DWORD)(uintptr_t)arg * 1000);
    LOG("CrashTest: crashing on purpose now");
    *(volatile int *)(uintptr_t)0x10 = 1;
    return 0;
}

/* Debug: the game's own calls that move, show, activate or focus its top-level window (Alt-Tab hang) */
typedef BOOL (WINAPI *ShowWindow_fn)(HWND, int);
typedef BOOL (WINAPI *SetWindowPos_fn)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *HwndBool_fn)(HWND);
typedef HWND (WINAPI *HwndHwnd_fn)(HWND);
static ShowWindow_fn o_ShowWindow;
static SetWindowPos_fn o_SetWindowPos;
static HwndBool_fn o_SetForegroundWindow;
static HwndHwnd_fn o_SetActiveWindow, o_SetFocus;
static LONG g_fw_logs;
static int fw_top(HWND h) { return h && !GetParent(h) && g_fw_logs < 400; }
static const char *caller_str(void *at)   /* one caller per thread at a time: a per-thread buffer */
{
    static __thread char buf[64];
    addr_name((uint32_t)(uintptr_t)at, buf, sizeof buf);
    return buf;
}
static BOOL WINAPI h_ShowWindow(HWND h, int cmd)
{
    void *at = __builtin_return_address(0);
    BOOL r = o_ShowWindow(h, cmd);
    if (fw_top(h)) { InterlockedIncrement(&g_fw_logs);
        LOG("focus: ShowWindow(0x%08lx, %d) from %s: %d (iconic %d, thread %lu)", (unsigned long)(uintptr_t)h, cmd,
            caller_str(at), r, IsIconic(h) ? 1 : 0, GetCurrentThreadId()); }
    return r;
}
static BOOL WINAPI h_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT f)
{
    void *at = __builtin_return_address(0);
    BOOL r = o_SetWindowPos(h, after, x, y, cx, cy, f);
    if (fw_top(h)) { InterlockedIncrement(&g_fw_logs);
        LOG("focus: SetWindowPos(0x%08lx, after 0x%08lx, %d,%d %dx%d, flags 0x%x) from %s: %d (thread %lu)",
            (unsigned long)(uintptr_t)h, (unsigned long)(uintptr_t)after, x, y, cx, cy, f, caller_str(at), r,
            GetCurrentThreadId()); }
    return r;
}
static BOOL WINAPI h_SetForegroundWindow(HWND h)
{
    void *at = __builtin_return_address(0);
    BOOL r = o_SetForegroundWindow(h);
    if (fw_top(h)) { InterlockedIncrement(&g_fw_logs);
        LOG("focus: SetForegroundWindow(0x%08lx) from %s: %d (thread %lu)", (unsigned long)(uintptr_t)h,
            caller_str(at), r, GetCurrentThreadId()); }
    return r;
}
static HWND WINAPI h_SetActiveWindow(HWND h)
{
    void *at = __builtin_return_address(0);
    HWND r = o_SetActiveWindow(h);
    if (fw_top(h)) { InterlockedIncrement(&g_fw_logs);
        LOG("focus: SetActiveWindow(0x%08lx) from %s: previous 0x%08lx (thread %lu)", (unsigned long)(uintptr_t)h,
            caller_str(at), (unsigned long)(uintptr_t)r, GetCurrentThreadId()); }
    return r;
}
static HWND WINAPI h_SetFocus(HWND h)
{
    void *at = __builtin_return_address(0);
    HWND r = o_SetFocus(h);
    if (fw_top(h)) { InterlockedIncrement(&g_fw_logs);
        LOG("focus: SetFocus(0x%08lx) from %s: previous 0x%08lx (thread %lu)", (unsigned long)(uintptr_t)h,
            caller_str(at), (unsigned long)(uintptr_t)r, GetCurrentThreadId()); }
    return r;
}
static void focus_hooks_setup(HMODULE exe)
{
    o_ShowWindow = (ShowWindow_fn)iat_hook(exe, "USER32.dll", "ShowWindow", (void *)h_ShowWindow);
    o_SetWindowPos = (SetWindowPos_fn)iat_hook(exe, "USER32.dll", "SetWindowPos", (void *)h_SetWindowPos);
    o_SetForegroundWindow = (HwndBool_fn)iat_hook(exe, "USER32.dll", "SetForegroundWindow", (void *)h_SetForegroundWindow);
    o_SetActiveWindow = (HwndHwnd_fn)iat_hook(exe, "USER32.dll", "SetActiveWindow", (void *)h_SetActiveWindow);
    o_SetFocus = (HwndHwnd_fn)iat_hook(exe, "USER32.dll", "SetFocus", (void *)h_SetFocus);
    if (!o_ShowWindow) o_ShowWindow = ShowWindow;
    if (!o_SetWindowPos) o_SetWindowPos = SetWindowPos;
    if (!o_SetForegroundWindow) o_SetForegroundWindow = SetForegroundWindow;
    if (!o_SetActiveWindow) o_SetActiveWindow = SetActiveWindow;
    if (!o_SetFocus) o_SetFocus = SetFocus;
    LOG("focus: the game's ShowWindow, SetWindowPos, SetForegroundWindow, SetActiveWindow and SetFocus on its main window are logged");
}

static void prof_start(void);
static void start_thread_dump(void)
{
    prof_start();
    focus_hooks_setup(GetModuleHandleA(NULL));
    if (ini_int("CrashTest", 0) > 0) {
        HANDLE t = o_CreateThread(NULL, 0, crash_test_thread, (LPVOID)(uintptr_t)ini_int("CrashTest", 0), 0, NULL);
        if (t) CloseHandle(t);
        LOG("CrashTest: will crash on purpose after %d s", ini_int("CrashTest", 0));
    }
    {
        char buf[32];
        GetPrivateProfileStringA("nba99", "WatchAddr", "", buf, sizeof buf, g_ini);
        if (buf[0]) {
            uint32_t a = (uint32_t)strtoul(buf, NULL, 0);
            if (a >= 0x400000 && a < 0x700000) {
                HANDLE h = o_CreateThread(NULL, 0, watch_thread, (LPVOID)(uintptr_t)a, 0, NULL);
                if (h) { SetThreadPriority(h, THREAD_PRIORITY_HIGHEST); CloseHandle(h); }
            }
        }
    }
    if (ini_int("CrashLog", 1)) {
        AddVectoredExceptionHandler(0, crash_handler);
        if (iat_hook(GetModuleHandleA(NULL), "KERNEL32.dll", "UnhandledExceptionFilter", (void *)h_UnhandledExceptionFilter))
            LOG("crash report: on (unhandled crashes end the game with a report instead of the system dialog)");
        iat_hook(GetModuleHandleA(NULL), "KERNEL32.dll", "SetUnhandledExceptionFilter", (void *)h_SetUnhandledExceptionFilter);
        SetUnhandledExceptionFilter(top_filter);
    }
    int secs = ini_int("ThreadDump", 0);
    HANDLE h;
    p_NtQIT = (NtQIT_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (secs <= 0) return;
    h = o_CreateThread(NULL, 0, td_thread, (LPVOID)(intptr_t)secs, 0, &g_td_tid);
    if (h) CloseHandle(h);
    LOG("diagnostics: logging every thread's position every %d s", secs);
}

#include "profile.inc"
