/*
 * Speech in memory (0.3.0, from NBA Live 98 0.7.11; included by nba99core.cpp, both builds).
 * [nba99] SpeechInMemory=1 (default).
 *
 * During a match NBA Live 99's streaming thread reopens the commentary banks for every line:
 * the 0.3.0 measurements counted speech\playspch\gmctdata.viv 10 to 19 times a minute and
 * plyrdata.viv 5 to 7, plus three look-ups per player line (speech\..\user\NAME.dat,
 * speech\..\update\NAME.dat, NAME.dat in the game folder). From a network drive each open
 * took 20 to 300 ms, under EA's per-drive file lock, which the main thread then waits on: one
 * frame of 100 to 1000 ms every few seconds. From a local drive there were none.
 *
 * A background thread (lowest priority) reads every file in speech\<bank folder>\ and its
 * update\ and user\ folders (about 142 MB for the English install: GMCTDATA, PLYRDATA and
 * their HDRS banks, EVENT.DAT, the v1.1 patch's per-player DAT/HDR files) into memory once, in
 * 1 MB pieces, a few seconds after start-up. Once a file is fully in memory, the game's
 * read-only opens of it no longer touch the disk: the game gets a real handle of an unnamed
 * event (so the handle value is unique and CloseHandle works) and every file call on that
 * handle (read, seek, size, type, information, time, duplicate, overlapped result, file mapping)
 * is answered from the memory copy. Files still loading and opens for writing go to the real file as
 * before.
 *
 * Missing files: a read-only open of a file under speech\ or a .dat/.hdr name in the game
 * folder itself that Windows answered "not found" is answered "not found" again from then on
 * without asking the disk (the look-ups above mostly fail: the user\ folders are empty). Any
 * open for writing or creating, any delete and any move clears that list.
 */
#define SPM_FILES 512
#define SPM_HANDLES 64
#define SPM_LIMIT (320u << 20)   /* at most this much in memory in all */

typedef BOOL (WINAPI *spm_ReadFile_fn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *spm_WriteFile_fn)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef DWORD (WINAPI *spm_SetFilePointer_fn)(HANDLE, LONG, PLONG, DWORD);
typedef DWORD (WINAPI *spm_GetFileSize_fn)(HANDLE, LPDWORD);
typedef DWORD (WINAPI *spm_GetFileType_fn)(HANDLE);
typedef BOOL (WINAPI *spm_handle_fn)(HANDLE);
typedef BOOL (WINAPI *spm_GetFileInformationByHandle_fn)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
typedef BOOL (WINAPI *spm_GetFileTime_fn)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
typedef BOOL (WINAPI *spm_DuplicateHandle_fn)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
typedef HANDLE (WINAPI *spm_CreateFileMappingA_fn)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);

static spm_ReadFile_fn spm_ReadFile;
static spm_WriteFile_fn spm_WriteFile;
static spm_SetFilePointer_fn spm_SetFilePointer;
static spm_GetFileSize_fn spm_GetFileSize;
static spm_GetFileType_fn spm_GetFileType;
static spm_handle_fn spm_SetEndOfFile, spm_FlushFileBuffers, spm_CloseHandle;
static spm_GetFileInformationByHandle_fn spm_GetFileInformationByHandle;
static spm_GetFileTime_fn spm_GetFileTime;
static spm_DuplicateHandle_fn spm_DuplicateHandle;
static spm_CreateFileMappingA_fn spm_CreateFileMappingA;

typedef struct {
    char path[MAX_PATH];             /* full path, lower case */
    unsigned char *buf;
    DWORD size;
    BY_HANDLE_FILE_INFORMATION info;
    volatile LONG ready;             /* 1 = fully in memory */
    HANDLE map;                      /* page-file section holding buf, once the game maps the bank */
} spm_file;

typedef struct {   /* one open of a bank (shared by duplicated handles, like a real file pointer) */
    int file;
    DWORD pos;
    LONG refs;
} spm_open;

static spm_file g_spm[SPM_FILES];
static int g_nspm;
static struct { HANDLE h; spm_open *o; } g_spmh[SPM_HANDLES];
static CRITICAL_SECTION g_spm_cs;
static int g_spm_on;
static DWORD g_spm_total;
static double g_spm_load_ms;
static long g_spm_opens, g_spm_reads, g_spm_real_opens, g_spm_missing_hits;

typedef BOOL (WINAPI *spm_GetOverlappedResult_fn)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static spm_GetOverlappedResult_fn spm_GetOverlappedResult;

/* files known to be missing (full path, lower case) */
#define SPM_MISSING 512
static char (*g_spm_missing)[MAX_PATH];
static int g_nspm_missing;

static void spm_key(const char *name, char *key, size_t cap)
{
    if (!GetFullPathNameA(name, (DWORD)cap, key, NULL)) _snprintf(key, cap, "%s", name);
    key[cap - 1] = 0;
    CharLowerA(key);
}

static int spm_find_file(const char *key)
{
    int i;
    for (i = 0; i < g_nspm; i++) if (!strcmp(g_spm[i].path, key)) return i;
    return -1;
}

/* the open state for a handle of ours, or NULL (call with the lock held) */
static spm_open *spm_lookup(HANDLE h, int *slot)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return NULL;
    for (i = 0; i < SPM_HANDLES; i++)
        if (g_spmh[i].h == h) { if (slot) *slot = i; return g_spmh[i].o; }
    return NULL;
}

static int spm_add_handle(HANDLE h, spm_open *o)
{
    int i;
    for (i = 0; i < SPM_HANDLES; i++)
        if (!g_spmh[i].h) { g_spmh[i].h = h; g_spmh[i].o = o; o->refs++; return 1; }
    return 0;
}

/* called by h_CreateFileA with the path the game will really open: a handle of ours when
   the bank is in memory, else NULL (the caller opens the real file) */
static HANDLE spm_try_open(const char *name, DWORD access, DWORD disp, DWORD flags)
{
    char key[MAX_PATH];
    int f;
    HANDLE h;
    spm_open *o;
    if (!g_spm_on || !name) return NULL;
    if ((access & ~(GENERIC_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE)) || !(access & (GENERIC_READ | FILE_READ_DATA)))
        return NULL;
    if (disp != OPEN_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE)) return NULL;
    spm_key(name, key, sizeof key);
    EnterCriticalSection(&g_spm_cs);
    f = spm_find_file(key);
    if (f < 0 || !g_spm[f].ready) {
        if (strstr(key, "\\speech\\") && strstr(key, ".viv")) g_spm_real_opens++;   /* a bank not in memory (yet) */
        LeaveCriticalSection(&g_spm_cs);
        return NULL;
    }
    h = CreateEventA(NULL, TRUE, FALSE, NULL);
    o = (spm_open *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    if (!h || !o || (o->file = f, !spm_add_handle(h, o))) {
        if (h) CloseHandle(h);
        if (o) HeapFree(GetProcessHeap(), 0, o);
        LeaveCriticalSection(&g_spm_cs);
        return NULL;
    }
    g_spm_opens++;
    LeaveCriticalSection(&g_spm_cs);
    SetLastError(NO_ERROR);
    return h;
}

static BOOL WINAPI h_spm_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (o) {
        spm_file *f = &g_spm[o->file];
        DWORD pos = ov ? ov->Offset : o->pos, k = 0;
        if (ov && ov->OffsetHigh) pos = f->size;
        if (pos < f->size) {
            k = f->size - pos;
            if (k > n) k = n;
            memcpy(b, f->buf + pos, k);
        }
        if (ov) {
            ov->Internal = 0;
            ov->InternalHigh = k;
            if (ov->hEvent) SetEvent((HANDLE)((uintptr_t)ov->hEvent & ~(uintptr_t)1));
        }
        o->pos = pos + k;
        g_spm_reads++;
        LeaveCriticalSection(&g_spm_cs);
        if (got) *got = k;
        SetLastError(NO_ERROR);
        return TRUE;
    }
    LeaveCriticalSection(&g_spm_cs);
    return spm_ReadFile(h, b, n, got, ov);
}

static BOOL WINAPI h_spm_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    LeaveCriticalSection(&g_spm_cs);
    if (o) { if (put) *put = 0; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }   /* opened read only */
    return spm_WriteFile(h, b, n, put, ov);
}

static DWORD WINAPI h_spm_SetFilePointer(HANDLE h, LONG d, PLONG hi, DWORD m)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (o) {
        LONGLONG dist = hi ? (((LONGLONG)*hi) << 32) | (DWORD)d : (LONGLONG)d;
        LONGLONG base = m == FILE_BEGIN ? 0 : m == FILE_CURRENT ? (LONGLONG)o->pos : (LONGLONG)g_spm[o->file].size;
        LONGLONG np = base + dist;
        if (np < 0 || np > 0xFFFFFFFELL) {
            LeaveCriticalSection(&g_spm_cs);
            SetLastError(ERROR_NEGATIVE_SEEK);
            return INVALID_SET_FILE_POINTER;
        }
        o->pos = (DWORD)np;
        LeaveCriticalSection(&g_spm_cs);
        if (hi) *hi = 0;
        SetLastError(NO_ERROR);
        return (DWORD)np;
    }
    LeaveCriticalSection(&g_spm_cs);
    return spm_SetFilePointer(h, d, hi, m);
}

static DWORD WINAPI h_spm_GetFileSize(HANDLE h, LPDWORD hi)
{
    spm_open *o;
    DWORD s = 0;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (o) s = g_spm[o->file].size;
    LeaveCriticalSection(&g_spm_cs);
    if (o) { if (hi) *hi = 0; SetLastError(NO_ERROR); return s; }
    return spm_GetFileSize(h, hi);
}

static DWORD WINAPI h_spm_GetFileType(HANDLE h)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    LeaveCriticalSection(&g_spm_cs);
    if (o) { SetLastError(NO_ERROR); return FILE_TYPE_DISK; }
    return spm_GetFileType(h);
}

static BOOL WINAPI h_spm_SetEndOfFile(HANDLE h)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    LeaveCriticalSection(&g_spm_cs);
    if (o) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return spm_SetEndOfFile(h);
}

static BOOL WINAPI h_spm_FlushFileBuffers(HANDLE h)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    LeaveCriticalSection(&g_spm_cs);
    if (o) return TRUE;
    return spm_FlushFileBuffers(h);
}

static BOOL WINAPI h_spm_CloseHandle(HANDLE h)
{
    int slot = -1;
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, &slot);
    if (o) {
        g_spmh[slot].h = NULL;
        g_spmh[slot].o = NULL;
        if (--o->refs <= 0) HeapFree(GetProcessHeap(), 0, o);
    }
    LeaveCriticalSection(&g_spm_cs);
    return spm_CloseHandle(h);   /* ours: closes the event */
}

static BOOL WINAPI h_spm_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION fi)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (o && fi) *fi = g_spm[o->file].info;
    LeaveCriticalSection(&g_spm_cs);
    if (o) { SetLastError(NO_ERROR); return TRUE; }
    return spm_GetFileInformationByHandle(h, fi);
}

static BOOL WINAPI h_spm_GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (o) {
        if (c) *c = g_spm[o->file].info.ftCreationTime;
        if (a) *a = g_spm[o->file].info.ftLastAccessTime;
        if (w) *w = g_spm[o->file].info.ftLastWriteTime;
    }
    LeaveCriticalSection(&g_spm_cs);
    if (o) { SetLastError(NO_ERROR); return TRUE; }
    return spm_GetFileTime(h, c, a, w);
}

static BOOL WINAPI h_spm_DuplicateHandle(HANDLE sp, HANDLE h, HANDLE tp, LPHANDLE out, DWORD acc, BOOL inh, DWORD opt)
{
    spm_open *o;
    BOOL ok;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (!o) { LeaveCriticalSection(&g_spm_cs); return spm_DuplicateHandle(sp, h, tp, out, acc, inh, opt); }
    /* the event duplicates like any handle; the copy shares the file position, as a real one would */
    ok = spm_DuplicateHandle(sp, h, tp, out, 0, inh, (opt & ~DUPLICATE_CLOSE_SOURCE) | DUPLICATE_SAME_ACCESS);
    if (ok && out && sp == tp && sp == GetCurrentProcess()) {
        if (!spm_add_handle(*out, o)) { CloseHandle(*out); *out = NULL; ok = FALSE; SetLastError(ERROR_TOO_MANY_OPEN_FILES); }
    }
    LeaveCriticalSection(&g_spm_cs);
    if (ok && (opt & DUPLICATE_CLOSE_SOURCE)) h_spm_CloseHandle(h);
    return ok;
}

static BOOL WINAPI h_spm_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait)
{
    spm_open *o;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    LeaveCriticalSection(&g_spm_cs);
    if (o) {   /* our reads always complete at once */
        if (got) *got = ov ? (DWORD)ov->InternalHigh : 0;
        SetLastError(NO_ERROR);
        return TRUE;
    }
    return spm_GetOverlappedResult(h, ov, got, wait);
}

/* ---- missing-file list ---- */
static int spm_missing_candidate(const char *key)
{
    size_t dl = strlen(g_exe_dir), kl = strlen(key);
    const char *rest;
    if (kl <= dl || _strnicmp(key, g_exe_dir, dl)) return 0;
    rest = key + dl;
    if (!strncmp(rest, "speech\\", 7)) return 1;
    return !strchr(rest, '\\') && kl > 4 && (!strcmp(key + kl - 4, ".dat") || !strcmp(key + kl - 4, ".hdr")) &&
           strcmp(rest, "install.dat");
}

/* 1 if this read-only open is of a file known to be missing (sets the error) */
static int spm_known_missing(const char *name, DWORD access, DWORD disp)
{
    char key[MAX_PATH];
    int i, hit = 0;
    if (!g_spm_on || !name || disp != OPEN_EXISTING || (access & ~(GENERIC_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE)))
        return 0;
    spm_key(name, key, sizeof key);
    EnterCriticalSection(&g_spm_cs);
    for (i = 0; i < g_nspm_missing; i++) if (!strcmp(g_spm_missing[i], key)) { hit = 1; break; }
    if (hit) g_spm_missing_hits++;
    LeaveCriticalSection(&g_spm_cs);
    if (hit) SetLastError(ERROR_FILE_NOT_FOUND);
    return hit;
}

/* after a real open: remember a read-only open that found nothing; forget the list on any write */
static void spm_after_open(const char *name, DWORD access, DWORD disp, HANDLE h, DWORD err)
{
    char key[MAX_PATH];
    if (!g_spm_on || !name) return;
    if (disp != OPEN_EXISTING || (access & ~(GENERIC_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE))) {
        EnterCriticalSection(&g_spm_cs);
        g_nspm_missing = 0;
        LeaveCriticalSection(&g_spm_cs);
        return;
    }
    if (h != INVALID_HANDLE_VALUE || (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)) return;
    spm_key(name, key, sizeof key);
    if (!spm_missing_candidate(key)) return;
    EnterCriticalSection(&g_spm_cs);
    if (g_spm_missing && g_nspm_missing < SPM_MISSING) strcpy(g_spm_missing[g_nspm_missing++], key);
    LeaveCriticalSection(&g_spm_cs);
}

/* DeleteFile / MoveFile by the game: the folder changed, forget the list */
static void spm_forget_missing(void)
{
    if (!g_spm_on) return;
    EnterCriticalSection(&g_spm_cs);
    g_nspm_missing = 0;
    LeaveCriticalSection(&g_spm_cs);
}

typedef BOOL (WINAPI *spm_DeleteFileA_fn)(LPCSTR);
typedef BOOL (WINAPI *spm_MoveFileA_fn)(LPCSTR, LPCSTR);
static spm_DeleteFileA_fn spm_DeleteFileA;
static spm_MoveFileA_fn spm_MoveFileA;
static BOOL WINAPI h_spm_DeleteFileA(LPCSTR n) { BOOL r = spm_DeleteFileA(n); spm_forget_missing(); return r; }
static BOOL WINAPI h_spm_MoveFileA(LPCSTR a, LPCSTR b) { BOOL r = spm_MoveFileA(a, b); spm_forget_missing(); return r; }

/* NBA Live 99 maps some banks (98 never did). The first mapping of a bank moves its memory copy
   into an unnamed section backed by the page file (the copy is moved, not doubled); every mapping
   of that bank is then a duplicate handle of the section, which MapViewOfFile, UnmapViewOfFile and
   CloseHandle treat like any other. The game opened the bank read only, so a real mapping could
   only be read too; the section is created read/write so read and copy-on-write views both work. */
static long g_spm_maps;
static HANDLE spm_section(spm_file *f)   /* call with the lock held */
{
    HANDLE s;
    unsigned char *v;
    if (f->map) return f->map;
    s = spm_CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, f->size ? f->size : 1, NULL);
    if (!s) return NULL;
    v = (unsigned char *)MapViewOfFile(s, FILE_MAP_WRITE, 0, 0, 0);
    if (!v) { spm_CloseHandle(s); return NULL; }
    memcpy(v, f->buf, f->size);
    VirtualFree(f->buf, 0, MEM_RELEASE);
    f->buf = v;   /* reads are answered from the section's view from now on */
    f->map = s;
    return s;
}

static HANDLE WINAPI h_spm_CreateFileMappingA(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name)
{
    spm_open *o;
    HANDLE s, d = NULL;
    spm_file *f;
    EnterCriticalSection(&g_spm_cs);
    o = spm_lookup(h, NULL);
    if (!o) { LeaveCriticalSection(&g_spm_cs); return spm_CreateFileMappingA(h, sa, prot, hi, lo, name); }
    f = &g_spm[o->file];
    s = spm_section(f);
    if (s && !spm_DuplicateHandle(GetCurrentProcess(), s, GetCurrentProcess(), &d, 0, sa && sa->bInheritHandle, DUPLICATE_SAME_ACCESS))
        d = NULL;
    if (d) g_spm_maps++;
    LeaveCriticalSection(&g_spm_cs);
    if (g_spm_maps <= 8 || !d)
        LOG("speech in memory: file mapping of %s (protection 0x%lx, size %lu) %s", f->path, prot, lo,
            d ? "answered from memory" : "failed");
    if (!d) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    SetLastError(NO_ERROR);
    return d;
}

/* read one bank into memory (background thread) */
static void spm_load(const char *path)
{
    char key[MAX_PATH];
    HANDLE f;
    DWORD size, hi = 0, got, done = 0;
    unsigned char *buf;
    BY_HANDLE_FILE_INFORMATION info;
    int i;
    spm_key(path, key, sizeof key);
    EnterCriticalSection(&g_spm_cs);
    i = spm_find_file(key);
    LeaveCriticalSection(&g_spm_cs);
    if (i >= 0 || g_nspm >= SPM_FILES) return;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(f, &hi);
    if (size == INVALID_FILE_SIZE || hi || !GetFileInformationByHandle(f, &info) || g_spm_total + size > SPM_LIMIT) {
        CloseHandle(f);
        if (g_spm_total + size > SPM_LIMIT) LOG("speech in memory: %s left on disk (memory limit)", path);
        return;
    }
    buf = (unsigned char *)VirtualAlloc(NULL, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { CloseHandle(f); LOG("speech in memory: no memory for %s (%lu bytes)", path, size); return; }
    while (done < size) {
        DWORD want = size - done > (1u << 20) ? (1u << 20) : size - done;
        if (!ReadFile(f, buf + done, want, &got, NULL) || !got) break;
        done += got;
    }
    CloseHandle(f);
    if (done != size) { VirtualFree(buf, 0, MEM_RELEASE); LOG("speech in memory: reading %s failed", path); return; }
    EnterCriticalSection(&g_spm_cs);
    if (spm_find_file(key) < 0 && g_nspm < SPM_FILES) {
        spm_file *s = &g_spm[g_nspm];
        strcpy(s->path, key);
        s->buf = buf;
        s->size = size;
        s->info = info;
        g_spm_total += size;
        InterlockedExchange(&s->ready, 1);
        g_nspm++;
        buf = NULL;
    }
    LeaveCriticalSection(&g_spm_cs);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
}

/* every file in one folder */
static void spm_load_dir(const char *dir)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    _snprintf(pat, sizeof pat, "%s\\*", dir);
    fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        spm_load(path);
    } while (FindNextFileA(fh, &fd));
    FindClose(fh);
}

static DWORD WINAPI spm_thread(LPVOID arg)
{
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE fh;
    double t0;
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
    Sleep(3000);   /* let the game start up first */
    t0 = (double)GetTickCount();
    _snprintf(pat, sizeof pat, "%sspeech\\*", g_exe_dir);
    fh = FindFirstFileA(pat, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            char dir[MAX_PATH], sub[MAX_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            _snprintf(dir, sizeof dir, "%sspeech\\%s", g_exe_dir, fd.cFileName);
            spm_load_dir(dir);
            _snprintf(sub, sizeof sub, "%s\\update", dir);
            spm_load_dir(sub);
            _snprintf(sub, sizeof sub, "%s\\user", dir);
            spm_load_dir(sub);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    g_spm_load_ms = (double)GetTickCount() - t0;
    LOG("speech in memory: %d files, %.1f MB, read in %.1f s (%ld bank opens before they were ready went to the disk)",
        g_nspm, g_spm_total / 1048576.0, g_spm_load_ms / 1000.0, g_spm_real_opens);
    return 0;
}

static void spchmem_setup(HMODULE exe)
{
    int n = 0;
    HANDLE t;
    if (!ini_int("SpeechInMemory", 1)) { LOG("speech in memory: off (SpeechInMemory=0)"); return; }
    InitializeCriticalSection(&g_spm_cs);
#define SPM_HOOK(fn) \
    spm_##fn = (spm_##fn##_fn)iat_hook(exe, "KERNEL32.dll", #fn, (void *)h_spm_##fn); \
    if (spm_##fn) n++; else spm_##fn = (spm_##fn##_fn)fn;
#define SPM_HOOKH(fn) \
    spm_##fn = (spm_handle_fn)iat_hook(exe, "KERNEL32.dll", #fn, (void *)h_spm_##fn); \
    if (spm_##fn) n++; else spm_##fn = (spm_handle_fn)fn;
    SPM_HOOK(ReadFile) SPM_HOOK(WriteFile) SPM_HOOK(SetFilePointer) SPM_HOOK(GetFileSize) SPM_HOOK(GetFileType)
    SPM_HOOKH(SetEndOfFile) SPM_HOOKH(FlushFileBuffers) SPM_HOOKH(CloseHandle)
    SPM_HOOK(GetFileInformationByHandle) SPM_HOOK(GetFileTime) SPM_HOOK(DuplicateHandle)
    SPM_HOOK(CreateFileMappingA) SPM_HOOK(GetOverlappedResult)
#undef SPM_HOOK
#undef SPM_HOOKH
    if (n != 13) {
        LOG("speech in memory: off (only %d of 13 file calls could be hooked)", n);
        return;
    }
    spm_DeleteFileA = (spm_DeleteFileA_fn)iat_hook(exe, "KERNEL32.dll", "DeleteFileA", (void *)h_spm_DeleteFileA);
    if (!spm_DeleteFileA) spm_DeleteFileA = DeleteFileA;
    spm_MoveFileA = (spm_MoveFileA_fn)iat_hook(exe, "KERNEL32.dll", "MoveFileA", (void *)h_spm_MoveFileA);
    if (!spm_MoveFileA) spm_MoveFileA = MoveFileA;
    g_spm_missing = (char (*)[MAX_PATH])HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)SPM_MISSING * MAX_PATH);
    g_spm_on = 1;
    t = CreateThread(NULL, 0, spm_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    LOG("speech in memory: on (banks are read in the background; opens of a bank in memory never touch the disk)");
}

static void spchmem_report(void)
{
    if (g_spm_opens || g_spm_missing_hits || g_spm_maps)
        LOG("speech in memory: %ld opens and %ld reads answered from memory, %ld bank opens went to the disk, "
            "%ld look-ups of missing files answered from the list, %ld file mappings", g_spm_opens, g_spm_reads,
            g_spm_real_opens, g_spm_missing_hits, g_spm_maps);
}
