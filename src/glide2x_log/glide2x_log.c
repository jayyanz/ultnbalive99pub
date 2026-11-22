/*
 * glide2x_log v3.4: logging stand-in for glide2x.dll (NBA Live 99 project, Phase 0).
 *
 * Sits between NBAWIN.EXE and the real Glide DLL. Every call is forwarded
 * unchanged to glide2x_real.dll in the same folder (dgVoodoo's Glide2x.dll
 * renamed). Writes glide2x_log.txt next to the DLL.
 *
 * What it records:
 *  - when the DLL is loaded, relative to process start
 *  - every call to the setup functions (init, query, select, window open/close)
 *  - the first 20 calls of every other function, with args, caller and thread
 *  - a timeline: one line per second of activity with call counts for the
 *    functions that matter (swaps, triangles, LFB read/write locks, lines,
 *    texture and palette uploads), so menus and matches can be told apart
 *  - frame interval stats every 300 swaps, with a histogram
 *  - call counts per function at exit
 *
 * v2 changes: log output is buffered in memory and written at most once per
 * second (v1 cost about 4 ms per line, which slowed the game). Adds the
 * timeline, the LFB read/write split and the frame interval histogram.
 * v3 adds the stall watchdog: when the game stops making Glide calls for more
 * than 300 ms mid-game, it samples where every thread in the game is (v3.1;
 * v3 sampled only one thread, which turned out to be the window thread).
 * v3.2: watchdog runs at TIME_CRITICAL and reports when it was itself starved;
 * logs process CPU affinity, every thread the game creates (entry point) and
 * every SetThreadPriority call; STALL lines show each thread's priority and
 * start address.
 * v3.3: per-thread CPU accounting; after a watchdog starvation or a stall, logs
 * which threads used the CPU, with current/base priority and start address.
 * The DLL pins itself so the game's detect-unload-reload cannot unmap it.
 * v3.4: opt-in affinity=game mode (glide2x_log.ini): all CPUs for the process,
 * only the game's own threads and the WINMM timer thread pinned to one CPU.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "glide_thunks.inc"

void *g_real[GLIDE_FN_COUNT];            /* read by the thunks */

static HMODULE g_self;
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static volatile LONG g_ready;
static LONG g_count[GLIDE_FN_COUNT];
static LARGE_INTEGER g_qpf, g_qpc_load;

#define FIRST_N 20
#define SWAP_WINDOW 300

/* indices of functions we treat specially (filled in at load) */
static int ix_swap = -1, ix_lfblock = -1, ix_tri = -1, ix_aatri = -1, ix_line = -1,
           ix_aaline = -1, ix_texdl = -1, ix_paldl = -1, ix_lfbwrite = -1,
           ix_lfbread = -1, ix_clear = -1, ix_point = -1;

/* timeline */
static double g_next_tick_ms = 1000.0;
static LONG g_tick_count[GLIDE_FN_COUNT];
static LONG g_lfb_read, g_lfb_write, g_tick_lfb_read, g_tick_lfb_write;

/* swap timing */
static double g_last_swap_ms, g_win_min, g_win_max, g_win_sum;
static long g_win_n;
static const double k_hist_edges[] = { 20, 30, 37, 45, 70, 1e30 };
#define HIST_N 6
static long g_hist[HIST_N];

/* buffered output */
static char g_buf[256 * 1024];
static size_t g_buf_len;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc_load.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void flush_log(void)
{
    DWORD w;
    if (g_log != INVALID_HANDLE_VALUE && g_buf_len) WriteFile(g_log, g_buf, (DWORD)g_buf_len, &w, NULL);
    g_buf_len = 0;
}

/* caller must hold g_cs (or be in DllMain) */
static void log_line(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (g_log == INVALID_HANDLE_VALUE) return;
    if (g_buf_len > sizeof g_buf - 2048) flush_log();
    va_start(ap, fmt);
    n = _vsnprintf(g_buf + g_buf_len, 1020, fmt, ap);
    va_end(ap);
    if (n < 0 || n > 1020) n = 1020;
    g_buf_len += (size_t)n;
    g_buf[g_buf_len++] = '\r';
    g_buf[g_buf_len++] = '\n';
}

static void dir_of_self(char *out, size_t cap)
{
    char *s;
    GetModuleFileNameA(g_self, out, (DWORD)cap);
    s = strrchr(out, '\\');
    if (s) s[1] = 0; else out[0] = 0;
}

static int is_setup_fn(const char *n)
{
    static const char *const k[] = {
        "_grGlideInit@0", "_grGlideShutdown@0", "_grGlideGetVersion@4",
        "_grSstQueryBoards@4", "_grSstQueryHardware@4", "_grSstSelect@4",
        "_grSstWinOpen@28", "_grSstWinClose@0", "_grSstStatus@0",
        "_grGammaCorrectionValue@4", 0 };
    int i;
    for (i = 0; k[i]; i++) if (!strcmp(k[i], n)) return 1;
    return 0;
}

static int find_fn(const char *n)
{
    int i;
    for (i = 0; i < GLIDE_FN_COUNT; i++) if (!strcmp(g_fn_name[i], n)) return i;
    return -1;
}

static void load_real(void)
{
    char path[MAX_PATH];
    HMODULE real;
    int i, missing = 0;

    dir_of_self(path, sizeof path);
    strncat(path, "glide2x_real.dll", sizeof path - strlen(path) - 1);
    real = LoadLibraryA(path);
    if (!real) {
        log_line("FATAL: could not load %s (error %lu)", path, GetLastError());
        flush_log();
        MessageBoxA(NULL, "glide2x_log: glide2x_real.dll not found next to glide2x.dll.\n"
                          "Rename the real Glide2x.dll (e.g. dgVoodoo's) to glide2x_real.dll.",
                    "NBA Live 99 Glide logger", MB_ICONERROR);
        ExitProcess(1);
    }
    for (i = 0; i < GLIDE_FN_COUNT; i++) {
        g_real[i] = (void *)GetProcAddress(real, g_fn_name[i]);
        if (!g_real[i]) { log_line("WARNING: real DLL has no %s", g_fn_name[i]); missing++; }
    }
    ix_swap = find_fn("_grBufferSwap@4");       ix_lfblock = find_fn("_grLfbLock@24");
    ix_tri = find_fn("_grDrawTriangle@12");     ix_aatri = find_fn("_grAADrawTriangle@24");
    ix_line = find_fn("_grDrawLine@8");         ix_aaline = find_fn("_grAADrawLine@8");
    ix_texdl = find_fn("_grTexDownloadMipMap@16"); ix_paldl = find_fn("_grTexDownloadTable@12");
    ix_lfbwrite = find_fn("_grLfbWriteRegion@32"); ix_lfbread = find_fn("_grLfbReadRegion@28");
    ix_clear = find_fn("_grBufferClear@12");    ix_point = find_fn("_grDrawPoint@4");
    log_line("[%10.3f ms] real Glide loaded from %s (%d of %d functions missing)",
             now_ms(), path, missing, GLIDE_FN_COUNT);
}

static long tick_delta(int ix)
{
    long d;
    if (ix < 0) return 0;
    d = g_count[ix] - g_tick_count[ix];
    g_tick_count[ix] = g_count[ix];
    return d;
}

/* one timeline line covering activity since the last one; holds g_cs */
static void emit_tick(double t)
{
    long sw = tick_delta(ix_swap), tri = tick_delta(ix_tri), aat = tick_delta(ix_aatri),
         ln = tick_delta(ix_line) + tick_delta(ix_aaline), pt = tick_delta(ix_point),
         tdl = tick_delta(ix_texdl), pdl = tick_delta(ix_paldl), cl = tick_delta(ix_clear),
         lw = tick_delta(ix_lfbwrite), lr = tick_delta(ix_lfbread);
    long lkr = g_lfb_read - g_tick_lfb_read, lkw = g_lfb_write - g_tick_lfb_write;
    long total = 0;
    int i;
    g_tick_lfb_read = g_lfb_read; g_tick_lfb_write = g_lfb_write;
    for (i = 0; i < GLIDE_FN_COUNT; i++)
        if (i != ix_swap && i != ix_tri && i != ix_aatri && i != ix_line && i != ix_aaline &&
            i != ix_point && i != ix_texdl && i != ix_paldl && i != ix_clear &&
            i != ix_lfbwrite && i != ix_lfbread)
            total += tick_delta(i);
    if (!(sw | tri | aat | ln | pt | tdl | pdl | cl | lw | lr | lkr | lkw | total)) return;
    log_line("[%10.3f ms] TICK swaps %ld clears %ld | tris %ld aatris %ld lines %ld points %ld | "
             "lfbLock read %ld write %ld | lfbRegion read %ld write %ld | texDL %ld palDL %ld | other %ld",
             t, sw, cl, tri, aat, ln, pt, lkr, lkw, lr, lw, tdl, pdl, total);
}

/* ---------------------------------------------------------------------------
 * Stall watchdog (v3.1).
 * Once the game is swapping buffers, a background thread watches for gaps of
 * more than STALL_MS with no Glide calls at all. During such a gap it samples
 * EVERY thread in the process (except itself) at a few points in the stall:
 * briefly suspends each one, copies its registers and the top of its stack,
 * resumes it, and only then (all threads running again) logs EIP plus the
 * likely return addresses as module+offset. The thread that made the most
 * recent grBufferSwap is marked [RENDER]. This shows where each thread is:
 * e.g. inside NBAWIN.EXE, eacsnd.dll, dsound.dll, file reads or a wait.
 * Nothing that can take a lock (CRT, heap, our log) is done while a thread
 * is suspended.
 * v3 (first cut) watched only the first thread that swapped, which turned out
 * to be the window thread running the 3dfx splash screen.
 * ------------------------------------------------------------------------- */
#include <tlhelp32.h>

#define STALL_MS 300
#define STACK_DWORDS 512
#define MAX_THREADS 32
static const double k_sample_at_ms[] = { 300, 900, 1600, 2300, 3500 };
#define N_SAMPLE_POINTS 5

static volatile LONG g_last_call_i, g_last_swap_i, g_last_swap_tid;
static volatile LONG g_watchdog_started, g_stop_watchdog;
static HANDLE g_watchdog;
static DWORD g_watchdog_tid;

typedef struct {
    DWORD tid;
    int ok;
    int prio;
    uint32_t start;
    CONTEXT ctx;
    SIZE_T nstk;
    uint32_t stk[STACK_DWORDS];
} thread_sample;
static thread_sample g_ts[MAX_THREADS];

static int describe_addr(uint32_t a, char *out, int cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *base;
    if (a < 0x10000) return _snprintf(out, cap, "0x%08lx", (unsigned long)a);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &m) || !m)
        return _snprintf(out, cap, "0x%08lx(?)", (unsigned long)a);
    GetModuleFileNameA(m, path, sizeof path);
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if ((uintptr_t)m == 0x400000)  /* the game exe: plain VA, matches Ghidra */
        return _snprintf(out, cap, "%s!0x%08lx", base, (unsigned long)a);
    return _snprintf(out, cap, "%s+0x%lx", base, (unsigned long)(a - (uint32_t)(uintptr_t)m));
}

static int is_exec(uint32_t a)
{
    MEMORY_BASIC_INFORMATION mi;
    if (a < 0x10000) return 0;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE) return 0;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                          PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* return address heuristic: preceded by a CALL instruction */
static int after_call(uint32_t a)
{
    const unsigned char *p = (const unsigned char *)(uintptr_t)a;
    if (!is_exec(a - 7)) return 0;
    if (p[-5] == 0xE8) return 1;                                   /* call rel32 */
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return 1;         /* call [reg] / reg */
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return 1;         /* call [reg+d8] */
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return 1;         /* call [abs] / [reg+d32] */
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return 1;         /* call [sib+d32] */
    if (p[-7] == 0x2E && p[-6] == 0xFF) return 1;                  /* cs: call [abs] (Watcom) */
    return 0;
}

typedef LONG (WINAPI *NtQIT_fn)(HANDLE, int, PVOID, ULONG, PULONG);
static NtQIT_fn p_NtQueryInformationThread;

static void sample_thread(thread_sample *ts)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, ts->tid);
    ts->ok = 0;
    ts->nstk = 0;
    ts->prio = 99;
    ts->start = 0;
    if (!h) return;
    ts->prio = GetThreadPriority(h);
    if (p_NtQueryInformationThread) {
        ULONG ret = 0;
        uint32_t sa = 0;
        if (p_NtQueryInformationThread(h, 9 /* ThreadQuerySetWin32StartAddress */, &sa, sizeof sa, &ret) == 0)
            ts->start = sa;
    }
    memset(&ts->ctx, 0, sizeof ts->ctx);
    ts->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(h) != (DWORD)-1) {
        if (GetThreadContext(h, &ts->ctx)) {
            MEMORY_BASIC_INFORMATION mi;
            SIZE_T want = sizeof ts->stk, got = 0;
            ts->ok = 1;
            if (VirtualQuery((LPCVOID)(uintptr_t)ts->ctx.Esp, &mi, sizeof mi)) {
                SIZE_T avail = (SIZE_T)((char *)mi.BaseAddress + mi.RegionSize -
                                        (char *)(uintptr_t)ts->ctx.Esp);
                if (avail < want) want = avail;
                if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)ts->ctx.Esp,
                                      ts->stk, want, &got))
                    ts->nstk = got / 4;
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

static void log_thread_sample(double t, double stalled, const thread_sample *ts)
{
    char line[1600];
    int len = 0, found = 0;
    SIZE_T i;
    len += _snprintf(line, sizeof line, "[%10.3f ms] STALL %5.0f ms tid %5lu%s prio %d start ", t, stalled,
                     ts->tid, (LONG)ts->tid == g_last_swap_tid ? " [RENDER]" : "", ts->prio);
    len += describe_addr(ts->start, line + len, (int)sizeof line - len);
    len += _snprintf(line + len, sizeof line - len, ": ");
    if (!ts->ok) {
        len += _snprintf(line + len, sizeof line - len, "could not sample");
    } else {
        len += _snprintf(line + len, sizeof line - len, "eip ");
        len += describe_addr((uint32_t)ts->ctx.Eip, line + len, (int)sizeof line - len);
        len += _snprintf(line + len, sizeof line - len, " | stack:");
        for (i = 0; i < ts->nstk && found < 12 && len < (int)sizeof line - 80; i++) {
            uint32_t v = ts->stk[i];
            if (is_exec(v) && after_call(v)) {
                line[len++] = ' ';
                len += describe_addr(v, line + len, (int)sizeof line - len);
                found++;
            }
        }
    }
    EnterCriticalSection(&g_cs);
    log_line("%s", line);
    LeaveCriticalSection(&g_cs);
}

static void sample_all_threads(double t, double stalled)
{
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    HANDLE snap;
    int n = 0, i;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != g_watchdog_tid && n < MAX_THREADS)
                g_ts[n++].tid = te.th32ThreadID;
            te.dwSize = sizeof te;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    /* suspend/copy/resume one thread at a time; no formatting while suspended */
    for (i = 0; i < n; i++) sample_thread(&g_ts[i]);
    for (i = 0; i < n; i++) log_thread_sample(t, stalled, &g_ts[i]);
}


/* ---------------------------------------------------------------------------
 * CPU accounting (v3.3). Keeps a handle to every thread in the process and
 * reads its CPU time each watchdog tick. When the watchdog was starved, or a
 * stall ends, it logs which threads used the CPU in that window, with their
 * current and base priority (from NtQueryInformationThread; a current
 * priority of 16 or more means real-time range, e.g. an MMCSS audio thread)
 * and start address. If the process as a whole used little CPU while the
 * watchdog was starved, the CPU went to something outside the game.
 * ------------------------------------------------------------------------- */
typedef struct {
    LONG ExitStatus;
    PVOID TebBaseAddress;
    PVOID UniqueProcess;
    PVOID UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
} THREAD_BASIC_INFO32;

typedef struct {
    DWORD tid;
    HANDLE h;
    uint32_t start;
    ULONGLONG cpu_tick;    /* CPU time at last tick (100 ns) */
    ULONGLONG cpu_mark;    /* CPU time at stall start */
    int used;
} tinfo;
#define MAX_TRACK 96
static tinfo g_tr[MAX_TRACK];
static int g_ntr;
static double g_next_refresh;

static ULONGLONG thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    ULARGE_INTEGER a, b;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime;
    b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return a.QuadPart + b.QuadPart;
}


/* ---------------------------------------------------------------------------
 * Affinity mode (v3.4, opt-in via glide2x_log.ini: [glide2x_log] affinity=game).
 * The SingleProcAffinity shim pins EVERY thread to CPU 0, including the GPU
 * driver's own worker threads, which then starve the game for ~2.5 s at a
 * time. In "game" mode we give the process all CPUs back and pin only the
 * game's own threads (start address in the exe) and the WINMM timer thread
 * (it runs the game's timer callback) to the shim's CPU. Driver, dgVoodoo and
 * other threads float on all CPUs.
 * ------------------------------------------------------------------------- */
static int g_aff_game;
static DWORD_PTR g_game_mask;

static int is_game_thread_start(uint32_t start)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *base;
    if (start < 0x10000) return 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)start, &m) || !m)
        return 0;
    if (m == GetModuleHandleA(NULL)) return 1;
    GetModuleFileNameA(m, path, sizeof path);
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    return _stricmp(base, "winmm.dll") == 0;
}

static void pin_if_game(DWORD tid, uint32_t start)
{
    HANDLE h;
    char a[64];
    DWORD_PTR old;
    if (!g_aff_game || !is_game_thread_start(start)) return;
    h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return;
    old = SetThreadAffinityMask(h, g_game_mask);
    CloseHandle(h);
    describe_addr(start, a, sizeof a);
    EnterCriticalSection(&g_cs);
    log_line("[%10.3f ms] AFFINITY tid %lu (start %s) pinned to mask 0x%lx (was 0x%lx)",
             now_ms(), tid, a, (unsigned long)g_game_mask, (unsigned long)old);
    LeaveCriticalSection(&g_cs);
}

static int g_first_refresh_done;

static void refresh_threads(void)
{
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    HANDLE snap;
    int i;
    /* drop threads that have exited (the game starts short-lived workers all the time) */
    for (i = 0; i < g_ntr; ) {
        DWORD code = 0;
        if (GetExitCodeThread(g_tr[i].h, &code) && code != STILL_ACTIVE) {
            CloseHandle(g_tr[i].h);
            g_tr[i] = g_tr[--g_ntr];
        } else i++;
    }
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != g_watchdog_tid) {
                for (i = 0; i < g_ntr; i++) if (g_tr[i].tid == te.th32ThreadID) break;
                if (i == g_ntr && g_ntr < MAX_TRACK) {
                    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (h) {
                        tinfo *t = &g_tr[g_ntr++];
                        ULONG ret = 0;
                        t->tid = te.th32ThreadID;
                        t->h = h;
                        t->start = 0;
                        if (p_NtQueryInformationThread)
                            p_NtQueryInformationThread(h, 9, &t->start, sizeof t->start, &ret);
                        /* threads found after the first scan are new: count all their CPU time */
                        t->cpu_tick = t->cpu_mark = g_first_refresh_done ? 0 : thread_cpu(h);
                        pin_if_game(t->tid, t->start);
                    }
                }
            }
            te.dwSize = sizeof te;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    g_first_refresh_done = 1;
}

static void tick_threads(void)
{
    int i;
    for (i = 0; i < g_ntr; i++) g_tr[i].cpu_tick = thread_cpu(g_tr[i].h);
}

static void mark_threads(void)
{
    int i;
    for (i = 0; i < g_ntr; i++) g_tr[i].cpu_mark = thread_cpu(g_tr[i].h);
}

/* log the top CPU users since tick (use_mark=0) or since stall start (use_mark=1) */
static void log_cpu_users(double t, double window_ms, int use_mark, const char *what)
{
    ULONGLONG d[MAX_TRACK], total = 0;
    int order[MAX_TRACK], i, j, n = g_ntr;
    char line[1600];
    int len;
    refresh_threads();
    n = g_ntr;
    for (i = 0; i < n; i++) {
        ULONGLONG now = thread_cpu(g_tr[i].h), base = use_mark ? g_tr[i].cpu_mark : g_tr[i].cpu_tick;
        d[i] = now > base ? now - base : 0;
        total += d[i];
        order[i] = i;
    }
    for (i = 0; i < n; i++)                     /* sort by CPU used, descending */
        for (j = i + 1; j < n; j++)
            if (d[order[j]] > d[order[i]]) { int x = order[i]; order[i] = order[j]; order[j] = x; }
    len = _snprintf(line, sizeof line, "[%10.3f ms] CPU during %s (%.0f ms): game process used %.0f ms in total; top:",
                    t, what, window_ms, total / 10000.0);
    for (i = 0; i < n && i < 6 && len < (int)sizeof line - 200; i++) {
        tinfo *ti = &g_tr[order[i]];
        THREAD_BASIC_INFO32 bi;
        ULONG ret = 0;
        LONG cur = -99, base = -99;
        if (d[order[i]] == 0) break;
        memset(&bi, 0, sizeof bi);
        if (p_NtQueryInformationThread && p_NtQueryInformationThread(ti->h, 0, &bi, sizeof bi, &ret) == 0) {
            cur = bi.Priority; base = bi.BasePriority;
        }
        len += _snprintf(line + len, sizeof line - len, " | tid %lu %.0f ms prio %ld (base %ld)%s start ",
                         ti->tid, d[order[i]] / 10000.0, cur, base, (LONG)ti->tid == g_last_swap_tid ? " [RENDER]" : "");
        len += describe_addr(ti->start, line + len, (int)sizeof line - len);
    }
    EnterCriticalSection(&g_cs);
    log_line("%s", line);
    LeaveCriticalSection(&g_cs);
}

static DWORD WINAPI watchdog_main(LPVOID unused)
{
    int in_stall = 0, next_point = 0;
    double stall_start = 0, prev;
    (void)unused;
    refresh_threads();
    g_next_refresh = now_ms() + 1000.0;
    prev = now_ms();
    while (!g_stop_watchdog) {
        double t, since_call, since_swap_at_stall;
        Sleep(50);
        t = now_ms();
        if (t - prev > 250.0) {
            EnterCriticalSection(&g_cs);
            log_line("[%10.3f ms] WATCHDOG STARVED: asked to sleep 50 ms, woke after %.0f ms", t, t - prev);
            LeaveCriticalSection(&g_cs);
            log_cpu_users(t, t - prev, 0, "watchdog starvation");
        }
        prev = t;
        tick_threads();
        if (t >= g_next_refresh) { refresh_threads(); g_next_refresh = t + 1000.0; }
        since_call = t - (double)g_last_call_i;
        since_swap_at_stall = (double)g_last_call_i - (double)g_last_swap_i;
        if (!in_stall) {
            /* only count gaps that start while the game is swapping (in-game / 3D scenes) */
            if (since_call > STALL_MS && since_swap_at_stall < 1000.0) {
                in_stall = 1;
                next_point = 0;
                stall_start = (double)g_last_call_i;
                mark_threads();
            }
            continue;
        }
        if (since_call < STALL_MS) {
            double dur = (double)g_last_call_i - stall_start;
            log_cpu_users(t, t - stall_start, 1, "stall");
            EnterCriticalSection(&g_cs);
            log_line("[%10.3f ms] STALL END: %.0f ms with no Glide calls", t, dur);
            flush_log();
            LeaveCriticalSection(&g_cs);
            in_stall = 0;
            continue;
        }
        if (next_point < N_SAMPLE_POINTS && t - stall_start >= k_sample_at_ms[next_point]) {
            sample_all_threads(t, t - stall_start);
            next_point++;
        }
    }
    return 0;
}

static void start_watchdog(void)
{
    if (InterlockedExchange(&g_watchdog_started, 1)) return;
    g_watchdog = CreateThread(NULL, 0, watchdog_main, NULL, 0, &g_watchdog_tid);
    if (g_watchdog) SetThreadPriority(g_watchdog, THREAD_PRIORITY_TIME_CRITICAL);
    EnterCriticalSection(&g_cs);
    log_line("[%10.3f ms] watchdog started at TIME_CRITICAL priority (samples all threads during stalls)", now_ms());
    LeaveCriticalSection(&g_cs);
}

/* Called from every thunk. stack[0] = caller return address, stack[1..] = args. */
void __cdecl glog_call(int idx, const uint32_t *stack)
{
    LONG c;
    double t;

    if (!g_ready) {
        EnterCriticalSection(&g_cs);
        if (!g_ready) { load_real(); InterlockedExchange(&g_ready, 1); }
        LeaveCriticalSection(&g_cs);
    }

    c = InterlockedIncrement(&g_count[idx]);
    if (idx == ix_lfblock) {
        if (stack[1] == 0) InterlockedIncrement(&g_lfb_read);   /* GR_LFB_READ_ONLY */
        else InterlockedIncrement(&g_lfb_write);
    }

    t = now_ms();
    InterlockedExchange(&g_last_call_i, (LONG)t);

    if (idx == ix_swap) {
        InterlockedExchange(&g_last_swap_i, (LONG)t);
        InterlockedExchange(&g_last_swap_tid, (LONG)GetCurrentThreadId());
        if (!g_watchdog_started) start_watchdog();
        EnterCriticalSection(&g_cs);
        if (c > 1) {
            double dt = t - g_last_swap_ms;
            int h = 0;
            while (h < HIST_N - 1 && dt >= k_hist_edges[h]) h++;
            g_hist[h]++;
            if (g_win_n == 0 || dt < g_win_min) g_win_min = dt;
            if (g_win_n == 0 || dt > g_win_max) g_win_max = dt;
            g_win_sum += dt;
            if (++g_win_n == SWAP_WINDOW) {
                log_line("[%10.3f ms] SWAPS #%ld: last %d intervals min %.2f avg %.2f max %.2f ms (%.1f fps) | "
                         "<20ms %ld, 20-30 %ld, 30-37 %ld, 37-45 %ld, 45-70 %ld, >70 %ld",
                         t, c, SWAP_WINDOW, g_win_min, g_win_sum / g_win_n, g_win_max,
                         1000.0 * g_win_n / g_win_sum,
                         g_hist[0], g_hist[1], g_hist[2], g_hist[3], g_hist[4], g_hist[5]);
                g_win_n = 0; g_win_sum = 0;
                memset(g_hist, 0, sizeof g_hist);
            }
        }
        g_last_swap_ms = t;
        LeaveCriticalSection(&g_cs);
    }

    if (c <= FIRST_N || is_setup_fn(g_fn_name[idx])) {
        char args[256] = "";
        int nargs = g_fn_argbytes[idx] / 4, i, len = 0;
        for (i = 0; i < nargs && i < 8; i++)
            len += _snprintf(args + len, sizeof args - len, "%s0x%08lx", i ? ", " : "",
                             (unsigned long)stack[1 + i]);
        EnterCriticalSection(&g_cs);
        log_line("[%10.3f ms] tid %5lu  #%-4ld %-28s(%s)  caller 0x%08lx",
                 t, GetCurrentThreadId(), c, g_fn_name[idx], args, (unsigned long)stack[0]);
        LeaveCriticalSection(&g_cs);
    }

    if (t >= g_next_tick_ms) {
        EnterCriticalSection(&g_cs);
        if (t >= g_next_tick_ms) {
            emit_tick(t);
            flush_log();
            g_next_tick_ms = t + 1000.0;
        }
        LeaveCriticalSection(&g_cs);
    }
}


/* ---------------------------------------------------------------------------
 * Game import hooks (v3.2): log thread creation and priority changes made by
 * the game exe, so threads in STALL samples can be identified.
 * ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *CreateThread_fn)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE,
                                         LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *SetThreadPriority_fn)(HANDLE, int);
static CreateThread_fn o_CreateThread;
static SetThreadPriority_fn o_SetThreadPriority;

static const char *prio_name(int p)
{
    switch (p) {
    case THREAD_PRIORITY_IDLE: return "IDLE";
    case THREAD_PRIORITY_LOWEST: return "LOWEST";
    case THREAD_PRIORITY_BELOW_NORMAL: return "BELOW_NORMAL";
    case THREAD_PRIORITY_NORMAL: return "NORMAL";
    case THREAD_PRIORITY_ABOVE_NORMAL: return "ABOVE_NORMAL";
    case THREAD_PRIORITY_HIGHEST: return "HIGHEST";
    case THREAD_PRIORITY_TIME_CRITICAL: return "TIME_CRITICAL";
    default: return "?";
    }
}

static HANDLE WINAPI h_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T ss, LPTHREAD_START_ROUTINE fn,
                                    LPVOID arg, DWORD flags, LPDWORD tid_out)
{
    DWORD tid = 0;
    char a[64], b[64], c[64];
    HANDLE h = o_CreateThread(sa, ss, fn, arg, flags, &tid);
    if (tid_out) *tid_out = tid;
    if (h && g_aff_game) SetThreadAffinityMask(h, g_game_mask);
    describe_addr((uint32_t)(uintptr_t)fn, a, sizeof a);
    describe_addr((uint32_t)(uintptr_t)arg, b, sizeof b);   /* game trampoline 0x53827C passes the real entry as arg */
    EnterCriticalSection(&g_cs);
    log_line("[%10.3f ms] THREAD CREATE tid %lu entry %s arg %s (0x%08lx) flags 0x%lx%s  caller %s",
             now_ms(), tid, a, b, (unsigned long)(uintptr_t)arg, flags,
             g_aff_game ? " (pinned to game CPU)" : "",
             (describe_addr((uint32_t)(uintptr_t)__builtin_return_address(0), c, sizeof c), c));
    LeaveCriticalSection(&g_cs);
    return h;
}

static BOOL WINAPI h_SetThreadPriority(HANDLE h, int p)
{
    char a[64];
    BOOL r = o_SetThreadPriority(h, p);
    describe_addr((uint32_t)(uintptr_t)__builtin_return_address(0), a, sizeof a);
    EnterCriticalSection(&g_cs);
    log_line("[%10.3f ms] THREAD PRIORITY tid %lu -> %d %s  (from tid %lu) caller %s",
             now_ms(), GetThreadId(h), p, prio_name(p), GetCurrentThreadId(), a);
    LeaveCriticalSection(&g_cs);
    return r;
}

static void *iat_hook(HMODULE mod, const char *dll, const char *fn, void *repl)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!dd->VirtualAddress) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((char *)(base + imp->Name), dll)) continue;
        names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            void *orig;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, fn)) continue;
            orig = (void *)(uintptr_t)addrs->u1.Function;
            if (VirtualProtect(&addrs->u1.Function, 4, PAGE_EXECUTE_READWRITE, &old)) {
                addrs->u1.Function = (DWORD)(uintptr_t)repl;
                VirtualProtect(&addrs->u1.Function, 4, old, &old);
                return orig;
            }
            return NULL;
        }
    }
    return NULL;
}

static void install_game_hooks(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    DWORD_PTR pm = 0, sm = 0;
    SYSTEM_INFO si;
    p_NtQueryInformationThread = (NtQIT_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                          "NtQueryInformationThread");
    o_CreateThread = (CreateThread_fn)iat_hook(exe, "KERNEL32.dll", "CreateThread", (void *)h_CreateThread);
    o_SetThreadPriority = (SetThreadPriority_fn)iat_hook(exe, "KERNEL32.dll", "SetThreadPriority",
                                                         (void *)h_SetThreadPriority);
    GetSystemInfo(&si);
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    log_line("process affinity mask 0x%lx of system 0x%lx (%lu logical CPUs); priority class 0x%lx; "
             "hooks: CreateThread %s, SetThreadPriority %s",
             (unsigned long)pm, (unsigned long)sm, si.dwNumberOfProcessors,
             GetPriorityClass(GetCurrentProcess()),
             o_CreateThread ? "ok" : "not found", o_SetThreadPriority ? "ok" : "not found");
    {
        char ini[MAX_PATH], val[32] = "";
        dir_of_self(ini, sizeof ini);
        strncat(ini, "glide2x_log.ini", sizeof ini - strlen(ini) - 1);
        GetPrivateProfileStringA("glide2x_log", "affinity", "", val, sizeof val, ini);
        if (!_stricmp(val, "game")) {
            DWORD_PTR m = pm ? pm : 1;
            g_game_mask = m & (~m + 1);               /* lowest CPU the shim allowed */
            g_aff_game = 1;
            if (sm && pm != sm) SetProcessAffinityMask(GetCurrentProcess(), sm);
            GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
            log_line("affinity=game: process mask now 0x%lx; game and WINMM threads pinned to 0x%lx",
                     (unsigned long)pm, (unsigned long)g_game_mask);
            refresh_threads();                         /* pins existing game threads */
        } else {
            log_line("affinity: unchanged (no glide2x_log.ini with affinity=game)");
        }
    }
}

static double ms_since_process_start(void)
{
    FILETIME cr, ex, k, u, now;
    ULARGE_INTEGER a, b;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return -1;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = cr.dwLowDateTime; a.HighPart = cr.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return (double)(b.QuadPart - a.QuadPart) / 10000.0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH];
        SYSTEMTIME st;
        double since;
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        /* Pin: the game loads glide2x.dll once to detect the card, then FreeLibrary()s
         * it and loads it again. Our import hooks in the exe point into this DLL, so it
         * must never be unmapped (v3.2 first cut crashed here). Pinning also keeps one
         * log for the whole session. */
        {
            HMODULE pinned;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               (LPCSTR)(void *)DllMain, &pinned);
        }
        InitializeCriticalSection(&g_cs);
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_qpc_load);
        since = ms_since_process_start();
        dir_of_self(path, sizeof path);
        strncat(path, "glide2x_log.txt", sizeof path - strlen(path) - 1);
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        GetLocalTime(&st);
        log_line("glide2x_log v3.4  %04d-%02d-%02d %02d:%02d:%02d  command line: %s",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCommandLineA());
        log_line("glide2x.dll loaded %.0f ms after process start (times below are ms since load)", since);
        install_game_hooks();
        flush_log();
    } else if (reason == DLL_PROCESS_DETACH) {
        int i;
        InterlockedExchange(&g_stop_watchdog, 1);
        double t = now_ms();
        emit_tick(t);
        log_line("[%10.3f ms] unloading. Call counts:", t);
        for (i = 0; i < GLIDE_FN_COUNT; i++)
            if (g_count[i]) log_line("  %-28s %ld", g_fn_name[i], g_count[i]);
        log_line("  lfbLock read-only %ld, write-only %ld", g_lfb_read, g_lfb_write);
        flush_log();
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
