/*
 * Temp file write buffer (0.3.0, from NBA Live 98 0.7.x; included by
 * nba99core.cpp, both builds). [nba99] BufferTempFiles=1 (default).
 *
 * While loading the teams for a match, NBA Live 99 builds nbatemp\$nbalst*.tmp
 * in many small writes: the 0.3.0 measurements counted 75 opens of
 * $nbalst2.tmp and about 950 writes (5.7 MB) in that minute, 3.5 s of write
 * time from a network drive, on the loader threads and the main thread.
 *
 * For files in nbatemp\ opened for writing, the file's contents are kept in
 * memory while the game has it open: writes, reads, seeks, size and
 * truncation work on the memory copy, and the whole file is written to disk
 * in one go when the game closes it (or flushes it, or hands the handle to
 * another call that needs the real file). The file itself, its folder and
 * every name-based call (find, delete, rename) stay real, so nothing else
 * changes. FlushFileBuffers on these files only writes the memory copy out:
 * they are deleted when the game exits, so forcing them to the disk is wasted.
 * Saves and everything outside nbatemp\ are untouched.
 */
#define TMB_MAX 32
#define TMB_LIMIT (16u << 20)   /* files bigger than this are not buffered */

typedef BOOL (WINAPI *tmb_ReadFile_fn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *tmb_WriteFile_fn)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD (WINAPI *tmb_SetFilePointer_fn)(HANDLE, LONG, PLONG, DWORD);
typedef DWORD (WINAPI *tmb_GetFileSize_fn)(HANDLE, LPDWORD);
typedef BOOL (WINAPI *tmb_handle_fn)(HANDLE);
typedef BOOL (WINAPI *tmb_GetFileInformationByHandle_fn)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
typedef BOOL (WINAPI *tmb_GetFileTime_fn)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
typedef BOOL (WINAPI *tmb_DuplicateHandle_fn)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
typedef HANDLE (WINAPI *tmb_CreateFileMappingA_fn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

static tmb_ReadFile_fn tmb_ReadFile;
static tmb_WriteFile_fn tmb_WriteFile;
static tmb_SetFilePointer_fn tmb_SetFilePointer;
static tmb_GetFileSize_fn tmb_GetFileSize;
static tmb_handle_fn tmb_SetEndOfFile, tmb_FlushFileBuffers, tmb_CloseHandle;
static tmb_GetFileInformationByHandle_fn tmb_GetFileInformationByHandle;
static tmb_GetFileTime_fn tmb_GetFileTime;
static tmb_DuplicateHandle_fn tmb_DuplicateHandle;
static tmb_CreateFileMappingA_fn tmb_CreateFileMappingA;

static struct {
    HANDLE h;
    unsigned char *buf;
    DWORD size, cap, pos;
    int dirty;
    int readable;
} g_tmb[TMB_MAX];
static CRITICAL_SECTION g_tmb_cs;
static int g_tmb_on;
static long g_tmb_files, g_tmb_writes, g_tmb_commits, g_tmb_flushes;

/* is this a path inside the game's nbatemp folder? */
static int tmb_is_temp(const char *name)
{
    const char *p;
    if (!name) return 0;
    for (p = name; *p; p++) {
        if ((p == name || p[-1] == '\\' || p[-1] == '/' || p[-1] == ':') &&
            _strnicmp(p, "nbatemp", 7) == 0 && (p[7] == '\\' || p[7] == '/'))
            return 1;
    }
    return 0;
}

static int tmb_find(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return -1;
    for (i = 0; i < TMB_MAX; i++) if (g_tmb[i].h == h) return i;
    return -1;
}

static int tmb_grow(int i, DWORD need)
{
    DWORD cap;
    unsigned char *nb;
    if (need <= g_tmb[i].cap) return 1;
    if (need > TMB_LIMIT) return 0;
    cap = g_tmb[i].cap ? g_tmb[i].cap : 16384;
    while (cap < need) cap *= 2;
    nb = g_tmb[i].buf ? (unsigned char *)HeapReAlloc(GetProcessHeap(), 0, g_tmb[i].buf, cap)
                      : (unsigned char *)HeapAlloc(GetProcessHeap(), 0, cap);
    if (!nb) return 0;
    g_tmb[i].buf = nb;
    g_tmb[i].cap = cap;
    return 1;
}

/* write the memory copy to the real file (call with the lock held) */
static BOOL tmb_commit(int i)
{
    DWORD w = 0;
    BOOL ok = TRUE;
    if (!g_tmb[i].dirty) return TRUE;
    tmb_SetFilePointer(g_tmb[i].h, 0, NULL, FILE_BEGIN);
    if (g_tmb[i].size) ok = tmb_WriteFile(g_tmb[i].h, g_tmb[i].buf, g_tmb[i].size, &w, NULL) && w == g_tmb[i].size;
    if (ok) ok = tmb_SetEndOfFile(g_tmb[i].h);
    tmb_SetFilePointer(g_tmb[i].h, (LONG)g_tmb[i].pos, NULL, FILE_BEGIN);
    g_tmb[i].dirty = 0;
    g_tmb_commits++;
    if (!ok) LOG("temp buffer: writing a temp file failed (error %lu)", GetLastError());
    return ok;
}

/* stop buffering a handle: commit, put the real file pointer where the game expects it */
static void tmb_drop(int i)
{
    tmb_commit(i);
    tmb_SetFilePointer(g_tmb[i].h, (LONG)g_tmb[i].pos, NULL, FILE_BEGIN);
    if (g_tmb[i].buf) HeapFree(GetProcessHeap(), 0, g_tmb[i].buf);
    memset(&g_tmb[i], 0, sizeof g_tmb[0]);
}

/* called by h_CreateFileA after a successful open */
static void tmb_opened(HANDLE h, const char *name, DWORD access, DWORD disp, DWORD flags)
{
    int i;
    DWORD sz, hi = 0, got = 0;
    if (!g_tmb_on || h == INVALID_HANDLE_VALUE || !(access & (GENERIC_WRITE | GENERIC_ALL))) return;
    if (!tmb_is_temp(name)) return;
    if (flags & (FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING)) {   /* not seen: say so once */
        static LONG logged;
        if (!InterlockedExchange(&logged, 1)) LOG("temp buffer: %s opened with flags 0x%lx (overlapped or unbuffered): not buffered", name, flags);
        return;
    }
    EnterCriticalSection(&g_tmb_cs);
    for (i = 0; i < TMB_MAX && g_tmb[i].h; i++) {}
    if (i == TMB_MAX) { LeaveCriticalSection(&g_tmb_cs); return; }
    sz = (disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING) ? 0 : tmb_GetFileSize(h, &hi);
    if (sz == INVALID_FILE_SIZE || hi || sz > TMB_LIMIT) { LeaveCriticalSection(&g_tmb_cs); return; }
    memset(&g_tmb[i], 0, sizeof g_tmb[0]);
    g_tmb[i].h = h;
    g_tmb[i].readable = (access & (GENERIC_READ | GENERIC_ALL)) != 0;
    if (sz) {
        /* existing contents: read them in (needs read access; otherwise do not buffer) */
        if (!g_tmb[i].readable || !tmb_grow(i, sz) ||
            !tmb_ReadFile(h, g_tmb[i].buf, sz, &got, NULL) || got != sz) {
            if (g_tmb[i].buf) HeapFree(GetProcessHeap(), 0, g_tmb[i].buf);
            memset(&g_tmb[i], 0, sizeof g_tmb[0]);
            tmb_SetFilePointer(h, 0, NULL, FILE_BEGIN);
            LeaveCriticalSection(&g_tmb_cs);
            return;
        }
        g_tmb[i].size = sz;
        tmb_SetFilePointer(h, 0, NULL, FILE_BEGIN);
    }
    g_tmb_files++;
    LeaveCriticalSection(&g_tmb_cs);
}

static BOOL WINAPI h_tmb_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD put, LPOVERLAPPED o)
{
    int i;
    if (!o) {
        EnterCriticalSection(&g_tmb_cs);
        i = tmb_find(h);
        if (i >= 0) {
            DWORD end = g_tmb[i].pos + n;
            if (end >= g_tmb[i].pos && tmb_grow(i, end)) {
                if (g_tmb[i].pos > g_tmb[i].size)   /* written past the end: the gap reads as zeros */
                    memset(g_tmb[i].buf + g_tmb[i].size, 0, g_tmb[i].pos - g_tmb[i].size);
                if (n) memcpy(g_tmb[i].buf + g_tmb[i].pos, b, n);
                g_tmb[i].pos = end;
                if (end > g_tmb[i].size) g_tmb[i].size = end;
                g_tmb[i].dirty = 1;
                g_tmb_writes++;
                LeaveCriticalSection(&g_tmb_cs);
                if (put) *put = n;
                return TRUE;
            }
            tmb_drop(i);   /* too big for memory: carry on with the real file */
        }
        LeaveCriticalSection(&g_tmb_cs);
    }
    return tmb_WriteFile(h, b, n, put, o);
}

static BOOL WINAPI h_tmb_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED o)
{
    int i;
    if (!o) {
        EnterCriticalSection(&g_tmb_cs);
        i = tmb_find(h);
        if (i >= 0) {
            DWORD k = 0;
            if (!g_tmb[i].readable) {
                LeaveCriticalSection(&g_tmb_cs);
                if (got) *got = 0;
                SetLastError(ERROR_ACCESS_DENIED);
                return FALSE;
            }
            if (g_tmb[i].pos < g_tmb[i].size) {
                k = g_tmb[i].size - g_tmb[i].pos;
                if (k > n) k = n;
                memcpy(b, g_tmb[i].buf + g_tmb[i].pos, k);
                g_tmb[i].pos += k;
            }
            LeaveCriticalSection(&g_tmb_cs);
            if (got) *got = k;
            return TRUE;
        }
        LeaveCriticalSection(&g_tmb_cs);
    }
    return tmb_ReadFile(h, b, n, got, o);
}

static DWORD WINAPI h_tmb_SetFilePointer(HANDLE h, LONG d, PLONG hi, DWORD m)
{
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) {
        LONGLONG base, np;
        LONGLONG dist = hi ? (((LONGLONG)*hi) << 32) | (DWORD)d : (LONGLONG)d;
        base = m == FILE_BEGIN ? 0 : m == FILE_CURRENT ? (LONGLONG)g_tmb[i].pos : (LONGLONG)g_tmb[i].size;
        np = base + dist;
        if (np < 0) {
            LeaveCriticalSection(&g_tmb_cs);
            SetLastError(ERROR_NEGATIVE_SEEK);
            return INVALID_SET_FILE_POINTER;
        }
        if (np <= (LONGLONG)TMB_LIMIT) {
            g_tmb[i].pos = (DWORD)np;
            LeaveCriticalSection(&g_tmb_cs);
            if (hi) *hi = 0;
            SetLastError(NO_ERROR);
            return (DWORD)np;
        }
        tmb_drop(i);
    }
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_SetFilePointer(h, d, hi, m);
}

static DWORD WINAPI h_tmb_GetFileSize(HANDLE h, LPDWORD hi)
{
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) {
        DWORD s = g_tmb[i].size;
        LeaveCriticalSection(&g_tmb_cs);
        if (hi) *hi = 0;
        SetLastError(NO_ERROR);
        return s;
    }
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_GetFileSize(h, hi);
}

static BOOL WINAPI h_tmb_SetEndOfFile(HANDLE h)
{
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0 && tmb_grow(i, g_tmb[i].pos)) {
        if (g_tmb[i].pos > g_tmb[i].size)
            memset(g_tmb[i].buf + g_tmb[i].size, 0, g_tmb[i].pos - g_tmb[i].size);
        g_tmb[i].size = g_tmb[i].pos;
        g_tmb[i].dirty = 1;
        LeaveCriticalSection(&g_tmb_cs);
        return TRUE;
    }
    if (i >= 0) tmb_drop(i);
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_SetEndOfFile(h);
}

static BOOL WINAPI h_tmb_FlushFileBuffers(HANDLE h)
{
    int i;
    BOOL ok;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) {
        /* temp file: hand the data to Windows, but do not force it to the disk */
        ok = tmb_commit(i);
        g_tmb_flushes++;
        LeaveCriticalSection(&g_tmb_cs);
        return ok;
    }
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_FlushFileBuffers(h);
}

static BOOL WINAPI h_tmb_CloseHandle(HANDLE h)
{
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) tmb_drop(i);
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_CloseHandle(h);
}

/* calls that need the real file to be up to date: write the memory copy first */
static void tmb_sync(HANDLE h)
{
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) tmb_commit(i);
    LeaveCriticalSection(&g_tmb_cs);
}

static BOOL WINAPI h_tmb_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION fi)
{
    tmb_sync(h);
    return tmb_GetFileInformationByHandle(h, fi);
}

static BOOL WINAPI h_tmb_GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w)
{
    tmb_sync(h);
    return tmb_GetFileTime(h, c, a, w);
}

static BOOL WINAPI h_tmb_DuplicateHandle(HANDLE sp, HANDLE h, HANDLE tp, LPHANDLE out, DWORD acc, BOOL inh, DWORD opt)
{
    /* a second handle to the same file would not see the memory copy: stop buffering this one */
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) tmb_drop(i);
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_DuplicateHandle(sp, h, tp, out, acc, inh, opt);
}

static HANDLE WINAPI h_tmb_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name)
{
    int i;
    EnterCriticalSection(&g_tmb_cs);
    i = tmb_find(h);
    if (i >= 0) tmb_drop(i);
    LeaveCriticalSection(&g_tmb_cs);
    return tmb_CreateFileMappingA(h, sa, prot, hi, lo, name);
}

/* before the game opens a temp file again by name, make sure the disk copy is current */
static void tmb_before_open(const char *name)
{
    int i;
    (void)name;
    if (!g_tmb_on) return;
    EnterCriticalSection(&g_tmb_cs);
    for (i = 0; i < TMB_MAX; i++) if (g_tmb[i].h && g_tmb[i].dirty) tmb_commit(i);
    LeaveCriticalSection(&g_tmb_cs);
}

static void tempbuf_setup(HMODULE exe)
{
    int n = 0;
    if (!ini_int("BufferTempFiles", 1)) { LOG("temp file buffer: off (BufferTempFiles=0)"); return; }
    InitializeCriticalSection(&g_tmb_cs);
#define TMB_HOOK(fn) \
    tmb_##fn = (tmb_##fn##_fn)iat_hook(exe, "KERNEL32.dll", #fn, (void *)h_tmb_##fn); \
    if (tmb_##fn) n++; else tmb_##fn = (tmb_##fn##_fn)fn;
#define TMB_HOOKH(fn) \
    tmb_##fn = (tmb_handle_fn)iat_hook(exe, "KERNEL32.dll", #fn, (void *)h_tmb_##fn); \
    if (tmb_##fn) n++; else tmb_##fn = (tmb_handle_fn)fn;
    TMB_HOOK(ReadFile) TMB_HOOK(WriteFile) TMB_HOOK(SetFilePointer) TMB_HOOK(GetFileSize)
    TMB_HOOKH(SetEndOfFile) TMB_HOOKH(FlushFileBuffers) TMB_HOOKH(CloseHandle)
    TMB_HOOK(GetFileInformationByHandle) TMB_HOOK(GetFileTime) TMB_HOOK(DuplicateHandle)
    TMB_HOOK(CreateFileMappingA)
#undef TMB_HOOK
#undef TMB_HOOKH
    g_tmb_on = 1;
    LOG("temp file buffer: on (%d of 11 calls hooked; files in nbatemp\\ are kept in memory while open)", n);
}

static void tempbuf_report(void)
{
    if (g_tmb_files)
        LOG("temp file buffer: %ld temp files, %ld writes kept in memory, %ld disk writes, %ld flushes skipped",
            g_tmb_files, g_tmb_writes, g_tmb_commits, g_tmb_flushes);
}
