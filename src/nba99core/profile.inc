/*
 * Stall profiler (Debug build only, included by diag.inc). [nba99] ProfileStalls=1
 * (default in Debug) writes logs\profile.log.
 *
 * Menus still hitch on some screens (team select logos, starting line-up
 * cards, the in-game menu). To see where the time goes, a thread watches the
 * frame counter (glide2x's presents). Whenever the current frame has taken
 * longer than 40 ms, it samples the game's main thread every 2 ms: suspends
 * it, copies its registers and stack, resumes it, then works out
 *   - which module the thread was in (game exe, our DLLs, the graphics
 *     driver, kernel/ntdll for file access or waits, ...)
 *   - which game code it was in or called from (the first two return
 *     addresses inside NBAWIN.EXE, "site from caller")
 * Nothing that takes a lock is called while the thread is suspended.
 * The game's file calls are timed too (open, read, write, seek, flush, copy,
 * delete, find, volume and drive queries), split into main thread and other
 * threads, with the files the main thread opened. Once a second with slow
 * frames, one block is written to the log.
 *
 * In a match (simulation steps counted in the last half second) the limit is
 * 22 ms instead (0.3.0): with the frame pacing on, every match frame should
 * take 15 to 18 ms, and the pace log showed a few frames of 26 to 74 ms where
 * the game itself was busy.
 *
 * File totals (0.3.0, for the network-drive work): once a minute, whether or
 * not frames were slow, a "minute" block with every file call counted and
 * timed (main thread and other threads) and the 12 files opened most often
 * that minute with their open count and time, so the per-file costs NBA Live
 * 98 had (volume and free-space queries on every open, small temp file
 * writes, speech bank reads) show up in 99 if they are there.
 */
#define PROF_SLOW_MS 40.0
#define PROF_SLOW_MATCH_MS 22.0   /* in a match (steps running): every frame should be 16.7 ms (0.3.0) */
#define PROF_SITES 64

typedef struct { uint32_t a, b; long n; } prof_site;
typedef struct { HMODULE m; long n; } prof_mod;
enum { IO_OPEN, IO_READ, IO_WRITE, IO_SEEK, IO_FLUSH, IO_COPY, IO_DELETE, IO_FIND, IO_VOLUME, IO_DISKFREE,
       IO_DRIVETYPE, IO_MOVE, IO_N };
static const char *k_io_name[IO_N] = { "open", "read", "write", "seek", "flush", "copy", "delete", "find", "volume",
                                       "diskfree", "drivetype", "move" };
static struct { volatile long n[2]; volatile double ms[2]; volatile double bytes[2]; } g_io[IO_N];   /* [0] main, [1] others */
static struct { long n[2]; double ms[2]; double bytes[2]; } g_io_min[IO_N];   /* this minute */
#define PROF_NAMES 96
static struct { char name[56]; long n; double ms; } g_names[PROF_NAMES];       /* files opened this minute */
static int g_nnames;
static long g_names_other;
static HANDLE g_plog = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_prof_cs;
static char g_prof_files[1024];
static int g_prof_on;

static void plog(const char *fmt, ...)
{
    char buf[1600];
    int n;
    va_list ap;
    DWORD w;
    if (g_plog == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_plog, buf, (DWORD)n, &w, NULL);
}

static double qpc_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_qpf.QuadPart;
}

static void io_add(int k, double ms, double bytes)
{
    int o = GetCurrentThreadId() != g_known_tid[0];
    EnterCriticalSection(&g_prof_cs);
    g_io[k].n[o]++;
    g_io[k].ms[o] += ms;
    g_io[k].bytes[o] += bytes;
    g_io_min[k].n[o]++;
    g_io_min[k].ms[o] += ms;
    g_io_min[k].bytes[o] += bytes;
    LeaveCriticalSection(&g_prof_cs);
}

/* called by h_CreateFileA (through g_prof_open) */
static void prof_open(const char *name, double ms)
{
    io_add(IO_OPEN, ms, 0);
    {   /* per-minute count by file (any thread); path below the game folder when it is in it */
        const char *rel = name;
        size_t dl = strlen(g_exe_dir);
        int i;
        if (!_strnicmp(name, g_exe_dir, dl)) rel = name + dl;
        EnterCriticalSection(&g_prof_cs);
        for (i = 0; i < g_nnames && _stricmp(g_names[i].name, rel); i++) {}
        if (i == g_nnames && g_nnames < PROF_NAMES) {
            lstrcpynA(g_names[i].name, rel, sizeof g_names[i].name);
            g_names[i].n = 0; g_names[i].ms = 0;
            g_nnames++;
        }
        if (i < PROF_NAMES) { g_names[i].n++; g_names[i].ms += ms; } else g_names_other++;
        LeaveCriticalSection(&g_prof_cs);
    }
    if (GetCurrentThreadId() == g_known_tid[0]) {
        const char *b = strrchr(name, '\\');
        size_t l;
        b = b ? b + 1 : name;
        EnterCriticalSection(&g_prof_cs);
        l = strlen(g_prof_files);
        if (l < sizeof g_prof_files - 80)
            _snprintf(g_prof_files + l, sizeof g_prof_files - l, "%s%s (%.0f ms)", l ? ", " : "", b, ms);
        LeaveCriticalSection(&g_prof_cs);
    }
}

#define IO_WRAP(ret, name, k, params, args, bytes_expr)                                \
    typedef ret (WINAPI *name##_fn) params;                                             \
    static name##_fn o_##name;                                                          \
    static ret WINAPI p_##name params                                                   \
    {                                                                                   \
        double t0 = qpc_ms();                                                           \
        ret r = o_##name args;                                                          \
        io_add(k, qpc_ms() - t0, (double)(bytes_expr));                                 \
        return r;                                                                       \
    }
IO_WRAP(BOOL, ReadFile, IO_READ, (HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED o), (h, b, n, got, o), got ? *got : 0)
IO_WRAP(BOOL, WriteFile, IO_WRITE, (HANDLE h, LPCVOID b, DWORD n, LPDWORD put, LPOVERLAPPED o), (h, b, n, put, o), put ? *put : 0)
IO_WRAP(DWORD, SetFilePointer, IO_SEEK, (HANDLE h, LONG d, PLONG hi, DWORD m), (h, d, hi, m), 0)
IO_WRAP(BOOL, FlushFileBuffers, IO_FLUSH, (HANDLE h), (h), 0)
IO_WRAP(BOOL, CopyFileA, IO_COPY, (LPCSTR a, LPCSTR b, BOOL f), (a, b, f), 0)
IO_WRAP(BOOL, DeleteFileA, IO_DELETE, (LPCSTR a), (a), 0)
IO_WRAP(HANDLE, FindFirstFileA, IO_FIND, (LPCSTR a, LPWIN32_FIND_DATAA d), (a, d), 0)
IO_WRAP(BOOL, GetVolumeInformationA, IO_VOLUME, (LPCSTR a, LPSTR b, DWORD c, LPDWORD d, LPDWORD e, LPDWORD f, LPSTR g, DWORD h),
        (a, b, c, d, e, f, g, h), 0)
IO_WRAP(BOOL, GetDiskFreeSpaceA, IO_DISKFREE, (LPCSTR a, LPDWORD b, LPDWORD c, LPDWORD d, LPDWORD e), (a, b, c, d, e), 0)
IO_WRAP(UINT, GetDriveTypeA, IO_DRIVETYPE, (LPCSTR a), (a), 0)
IO_WRAP(BOOL, MoveFileA, IO_MOVE, (LPCSTR a, LPCSTR b), (a, b), 0)

static int in_exe_code(uint32_t a) { return a >= 0x401000 && a < 0x5D1000; }   /* the exe's AUTO (code) section */

/* CPU time each game thread used in the last second (to see which thread
   keeps the game's CPU busy when the main thread is slow) */
static struct { DWORD tid; ULONGLONG cpu; } g_pcpu[64];
static void prof_threads(char *line, int cap, int print)
{
    int i, len = 0;
    if (print) len = _snprintf(line, cap, "  threads (CPU ms this second):");
    for (i = 0; i < g_nknown && i < 64; i++) {
        DWORD tid = g_known_tid[i];
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, tid);
        FILETIME c, e, k, u;
        ULONGLONG now;
        if (!h) continue;
        if (GetThreadTimes(h, &c, &e, &k, &u) && (WaitForSingleObject(h, 0) == WAIT_TIMEOUT)) {
            now = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
            if (print && g_pcpu[i].tid == tid && len < cap - 40)
                len += _snprintf(line + len, cap - len, " %lu(p%d) %.0f", tid, GetThreadPriority(h),
                                 (double)(now - g_pcpu[i].cpu) / 10000.0);
            g_pcpu[i].tid = tid;
            g_pcpu[i].cpu = now;
        }
        CloseHandle(h);
    }
}

/* the minute's file totals and most-opened files; resets them */
static void prof_minute(double t)
{
    char line[1500];
    int o, k, len, any = 0;
    EnterCriticalSection(&g_prof_cs);
    for (k = 0; k < IO_N; k++) if (g_io_min[k].n[0] || g_io_min[k].n[1]) any = 1;
    if (any) {
        plog("==== minute to %.0f s: file calls", t);
        for (o = 0; o < 2; o++) {
            len = _snprintf(line, sizeof line, "  %s:", o ? "other threads" : "main thread");
            for (k = 0; k < IO_N; k++)
                if (g_io_min[k].n[o])
                    len += _snprintf(line + len, sizeof line - len, " %s %ld x %.1f ms", k_io_name[k], g_io_min[k].n[o],
                                     g_io_min[k].ms[o]);
            if (g_io_min[IO_READ].bytes[o] > 0)
                len += _snprintf(line + len, sizeof line - len, " | read %.0f KB", g_io_min[IO_READ].bytes[o] / 1024);
            if (g_io_min[IO_WRITE].bytes[o] > 0)
                len += _snprintf(line + len, sizeof line - len, " | written %.0f KB", g_io_min[IO_WRITE].bytes[o] / 1024);
            plog("%s", line);
        }
        len = _snprintf(line, sizeof line, "  most opened:");
        for (k = 0; k < 12; k++) {
            int i, best = -1;
            for (i = 0; i < g_nnames; i++) if (g_names[i].n && (best < 0 || g_names[i].n > g_names[best].n)) best = i;
            if (best < 0) break;
            len += _snprintf(line + len, sizeof line - len, " %s %ld x %.1f ms,", g_names[best].name, g_names[best].n,
                             g_names[best].ms);
            g_names[best].n = 0;
        }
        if (g_names_other) len += _snprintf(line + len, sizeof line - len, " (%ld opens of other files)", g_names_other);
        plog("%s", line);
    }
    memset(g_io_min, 0, sizeof g_io_min);
    g_nnames = 0;
    g_names_other = 0;
    LeaveCriticalSection(&g_prof_cs);
}

static DWORD WINAPI prof_thread(LPVOID arg)
{
    static uint32_t stk[4096];
    static prof_site sites[PROF_SITES];
    static prof_mod mods[24];
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_known_tid[0]);
    long last_pres = 0, last_steps = 0, samples = 0, slow_frames = 0, nsites = 0, nmods = 0, other_sites = 0;
    double last_change = qpc_ms(), last_step_ms = 0, sec_start = qpc_ms(), min_start = qpc_ms(), worst = 0;
    int in_slow = 0, i, j;
    (void)arg;
    if (!th) { plog("could not open the main thread; profiler off"); return 0; }
    for (;;) {
        double now, since;
        Sleep(2);
        now = qpc_ms();
        if (g_stats.presents != last_pres) {
            if (in_slow && now - last_change > worst) worst = now - last_change;
            last_pres = g_stats.presents;
            last_change = now;
            in_slow = 0;
        }
        since = now - last_change;
        if (g_stats.sim_steps != last_steps) { last_steps = g_stats.sim_steps; last_step_ms = now; }
        if (since > (now - last_step_ms < 500.0 ? PROF_SLOW_MATCH_MS : PROF_SLOW_MS) && last_pres) {
            CONTEXT c;
            MEMORY_BASIC_INFORMATION mi;
            SIZE_T n = 0;
            uint32_t a = 0, b = 0, eip;
            HMODULE m = NULL;
            if (!in_slow) { in_slow = 1; slow_frames++; }
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(th) == (DWORD)-1) continue;
            if (GetThreadContext(th, &c) && VirtualQuery((LPCVOID)(uintptr_t)c.Esp, &mi, sizeof mi)) {
                n = (SIZE_T)((char *)mi.BaseAddress + mi.RegionSize - (char *)(uintptr_t)c.Esp) / 4;
                if (n > 4096) n = 4096;
                memcpy(stk, (const void *)(uintptr_t)c.Esp, n * 4);
            }
            ResumeThread(th);
            eip = (uint32_t)c.Eip;
            if (!eip) continue;
            samples++;
            /* module the thread is in */
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)eip, &m);
            for (i = 0; i < nmods && mods[i].m != m; i++) {}
            if (i == nmods && nmods < 24) { mods[nmods].m = m; mods[nmods].n = 0; nmods++; }
            if (i < 24) mods[i].n++;
            /* game code: first two exe addresses (EIP or return addresses) */
            if (in_exe_code(eip)) a = eip;
            for (j = 0; j < (int)n && !b; j++) {
                uint32_t r = stk[j];
                if (!in_exe_code(r) || !td_after_call(r)) continue;
                if (!a) a = r; else b = r;
            }
            if (a) {
                for (i = 0; i < nsites && !(sites[i].a == a && sites[i].b == b); i++) {}
                if (i == nsites && nsites < PROF_SITES) { sites[i].a = a; sites[i].b = b; sites[i].n = 0; nsites++; }
                if (i < PROF_SITES) sites[i].n++; else other_sites++;
            }
        }
        if (now - sec_start >= 1000.0) {
            if (samples) {
                char line[1500];
                int len, k, o;
                if (in_slow && since > worst) worst = since;
                plog("---- %.1f s: %ld slow frame(s), longest %.0f ms, %ld samples (about %ld ms of slow time)",
                     now_ms() / 1000.0, slow_frames, worst, samples, samples * 2);
                /* modules, biggest first */
                len = _snprintf(line, sizeof line, "  in:");
                for (k = 0; k < 5; k++) {
                    int best = -1;
                    char nm[MAX_PATH], *bn;
                    for (i = 0; i < nmods; i++) if (mods[i].n && (best < 0 || mods[i].n > mods[best].n)) best = i;
                    if (best < 0) break;
                    if (!mods[best].m || !GetModuleFileNameA(mods[best].m, nm, sizeof nm)) strcpy(nm, "(no module)");
                    bn = strrchr(nm, '\\'); bn = bn ? bn + 1 : nm;
                    len += _snprintf(line + len, sizeof line - len, " %s %ld%%", bn, mods[best].n * 100 / samples);
                    mods[best].n = 0;
                }
                plog("%s", line);
                /* game code sites, biggest first */
                len = _snprintf(line, sizeof line, "  game code:");
                for (k = 0; k < 6; k++) {
                    int best = -1;
                    for (i = 0; i < nsites; i++) if (sites[i].n && (best < 0 || sites[i].n > sites[best].n)) best = i;
                    if (best < 0) break;
                    len += _snprintf(line + len, sizeof line - len, " 0x%06lx from 0x%06lx %ld%%,", (unsigned long)sites[best].a,
                                     (unsigned long)sites[best].b, sites[best].n * 100 / samples);
                    sites[best].n = 0;
                }
                plog("%s", line);
                prof_threads(line, sizeof line, 1);
                plog("%s", line);
                /* file calls */
                for (o = 0; o < 2; o++) {
                    len = _snprintf(line, sizeof line, "  files (%s):", o ? "other threads" : "main thread");
                    EnterCriticalSection(&g_prof_cs);
                    for (k = 0; k < IO_N; k++)
                        if (g_io[k].n[o])
                            len += _snprintf(line + len, sizeof line - len, " %s %ld x %.1f ms%s", k_io_name[k], g_io[k].n[o],
                                             g_io[k].ms[o], "");
                    if (g_io[IO_READ].bytes[o] > 0)
                        len += _snprintf(line + len, sizeof line - len, " | read %.0f KB", g_io[IO_READ].bytes[o] / 1024);
                    LeaveCriticalSection(&g_prof_cs);
                    plog("%s", line);
                }
                EnterCriticalSection(&g_prof_cs);
                if (g_prof_files[0]) plog("  opened: %s", g_prof_files);
                LeaveCriticalSection(&g_prof_cs);
            }
            else prof_threads(NULL, 0, 0);
            /* reset the second */
            EnterCriticalSection(&g_prof_cs);
            memset((void *)g_io, 0, sizeof g_io);
            g_prof_files[0] = 0;
            LeaveCriticalSection(&g_prof_cs);
            samples = slow_frames = nsites = nmods = other_sites = 0;
            worst = 0;
            sec_start = now;
        }
        if (now - min_start >= 60000.0) {
            prof_minute(now_ms() / 1000.0);
            min_start = now;
        }
    }
    return 0;
}

static void prof_start(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    char path[MAX_PATH];
    HANDLE h;
    if (!ini_int("ProfileStalls", 1)) return;
    InitializeCriticalSection(&g_prof_cs);
    nba99_log_path(g_exe_dir, "profile.log", path, sizeof path);
    g_plog = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    plog("NBA Live 99 stall profile, %s. A block for every second with frames over %.0f ms (%.0f ms in a match).",
         NBA99_VERSION_LINE, PROF_SLOW_MS, PROF_SLOW_MATCH_MS);
    plog("in: where the main thread was (module); game code: NBAWIN.EXE address it was in or called from, and its caller;");
    plog("files: the game's file calls in that second (count x total time).");
    plog("Once a minute: all file calls that minute and the files opened most often (\"minute\" blocks).");
#define HOOK(fn) o_##fn = (fn##_fn)iat_hook(exe, "KERNEL32.dll", #fn, (void *)p_##fn); if (!o_##fn) o_##fn = fn;
    HOOK(ReadFile) HOOK(WriteFile) HOOK(SetFilePointer) HOOK(FlushFileBuffers) HOOK(CopyFileA) HOOK(DeleteFileA)
    HOOK(FindFirstFileA) HOOK(GetVolumeInformationA) HOOK(GetDiskFreeSpaceA) HOOK(GetDriveTypeA) HOOK(MoveFileA)
#undef HOOK
    g_prof_open = prof_open;
    g_prof_on = 1;
    h = o_CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
    if (h) { SetThreadPriority(h, THREAD_PRIORITY_HIGHEST); CloseHandle(h); }
    LOG("stall profiler on: logs\\profile.log");
}
