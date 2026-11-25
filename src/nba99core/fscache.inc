/*
 * Drive information cache (0.3.0, from NBA Live 98 0.7.x; included by
 * nba99core.cpp, both builds). [nba99] CacheDriveInfo=1 (default).
 *
 * EA's file library in NBA Live 99 asks Windows for the free space of the
 * drive (GetDiskFreeSpaceA, 0x590ADC) on every file it opens, while it holds
 * its per-drive lock (0x590A8D): the 0.3.0 measurements counted one query per
 * open, 5 to 30 ms each from a network drive (up to 4.4 s a minute), and the
 * game's main thread waits on that lock meanwhile. The answers do not change
 * while the game runs (free space only slowly), so the first answer for each
 * drive root is kept and handed back from then on:
 *   - volume details: kept for the whole session;
 *   - free space: kept for 5 seconds, then asked again.
 *   - drive type (GetDriveTypeA): kept for the whole session.
 * (Names use an fsc_ prefix: profile.inc in the Debug build wraps the same
 * imports; the names must not clash.)
 * Failures are not cached (a CD drive with no disc, for instance, is asked
 * again next time).
 */
#define FSC_MAX 16

typedef BOOL (WINAPI *fsc_GetVolumeInformationA_fn)(LPCSTR, LPSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPSTR, DWORD);
typedef BOOL (WINAPI *fsc_GetDiskFreeSpaceA_fn)(LPCSTR, LPDWORD, LPDWORD, LPDWORD, LPDWORD);
typedef UINT (WINAPI *fsc_GetDriveTypeA_fn)(LPCSTR);
static fsc_GetVolumeInformationA_fn fsc_GetVolumeInformationA;
static fsc_GetDiskFreeSpaceA_fn fsc_GetDiskFreeSpaceA;
static fsc_GetDriveTypeA_fn fsc_GetDriveTypeA;

static struct {
    char root[MAX_PATH];
    int have_vol, have_free, have_type;
    char vol_name[MAX_PATH], fs_name[MAX_PATH];
    DWORD serial, maxlen, flags;
    DWORD spc, bps, nfree, ntotal;
    DWORD free_tick;
    UINT type;
} g_fsc[FSC_MAX];
static int g_nfsc;
static CRITICAL_SECTION g_fsc_cs;
static long g_fsc_hits, g_fsc_misses;

/* the cache entry for a root path (NULL = current drive), created if new */
static int fsc_slot(LPCSTR root)
{
    char key[MAX_PATH];
    int i;
    _snprintf(key, sizeof key, "%s", root ? root : "");
    key[sizeof key - 1] = 0;
    CharLowerA(key);
    for (i = 0; i < g_nfsc; i++) if (!strcmp(g_fsc[i].root, key)) return i;
    if (g_nfsc >= FSC_MAX) return -1;
    memset(&g_fsc[g_nfsc], 0, sizeof g_fsc[0]);
    strcpy(g_fsc[g_nfsc].root, key);
    return g_nfsc++;
}

static void copy_str(LPSTR dst, DWORD cap, const char *src)
{
    if (!dst || !cap) return;
    lstrcpynA(dst, src, (int)cap);
}

static BOOL WINAPI h_GetVolumeInformationA(LPCSTR root, LPSTR name, DWORD name_len, LPDWORD serial, LPDWORD maxlen,
                                           LPDWORD flags, LPSTR fs, DWORD fs_len)
{
    int i;
    EnterCriticalSection(&g_fsc_cs);
    i = fsc_slot(root);
    if (i >= 0 && g_fsc[i].have_vol) {
        copy_str(name, name_len, g_fsc[i].vol_name);
        copy_str(fs, fs_len, g_fsc[i].fs_name);
        if (serial) *serial = g_fsc[i].serial;
        if (maxlen) *maxlen = g_fsc[i].maxlen;
        if (flags) *flags = g_fsc[i].flags;
        g_fsc_hits++;
        LeaveCriticalSection(&g_fsc_cs);
        return TRUE;
    }
    LeaveCriticalSection(&g_fsc_cs);
    {
        char n[MAX_PATH] = "", f[MAX_PATH] = "";
        DWORD s = 0, m = 0, fl = 0;
        BOOL ok = fsc_GetVolumeInformationA(root, n, sizeof n, &s, &m, &fl, f, sizeof f);
        if (ok) {
            EnterCriticalSection(&g_fsc_cs);
            if (i >= 0) {
                strcpy(g_fsc[i].vol_name, n);
                strcpy(g_fsc[i].fs_name, f);
                g_fsc[i].serial = s; g_fsc[i].maxlen = m; g_fsc[i].flags = fl;
                g_fsc[i].have_vol = 1;
            }
            g_fsc_misses++;
            LeaveCriticalSection(&g_fsc_cs);
            copy_str(name, name_len, n);
            copy_str(fs, fs_len, f);
            if (serial) *serial = s;
            if (maxlen) *maxlen = m;
            if (flags) *flags = fl;
            /* a buffer too small for the real answer must still fail as before */
            if ((name && name_len && strlen(n) >= name_len) || (fs && fs_len && strlen(f) >= fs_len))
                return fsc_GetVolumeInformationA(root, name, name_len, serial, maxlen, flags, fs, fs_len);
        }
        return ok;
    }
}

static BOOL WINAPI h_GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD nfree, LPDWORD ntotal)
{
    int i;
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_fsc_cs);
    i = fsc_slot(root);
    if (i >= 0 && g_fsc[i].have_free && now - g_fsc[i].free_tick < 5000) {
        if (spc) *spc = g_fsc[i].spc;
        if (bps) *bps = g_fsc[i].bps;
        if (nfree) *nfree = g_fsc[i].nfree;
        if (ntotal) *ntotal = g_fsc[i].ntotal;
        g_fsc_hits++;
        LeaveCriticalSection(&g_fsc_cs);
        return TRUE;
    }
    LeaveCriticalSection(&g_fsc_cs);
    {
        DWORD a = 0, b = 0, c = 0, d = 0;
        BOOL ok = fsc_GetDiskFreeSpaceA(root, &a, &b, &c, &d);
        if (ok) {
            EnterCriticalSection(&g_fsc_cs);
            if (i >= 0) {
                g_fsc[i].spc = a; g_fsc[i].bps = b; g_fsc[i].nfree = c; g_fsc[i].ntotal = d;
                g_fsc[i].free_tick = now;
                g_fsc[i].have_free = 1;
            }
            g_fsc_misses++;
            LeaveCriticalSection(&g_fsc_cs);
            if (spc) *spc = a;
            if (bps) *bps = b;
            if (nfree) *nfree = c;
            if (ntotal) *ntotal = d;
        }
        return ok;
    }
}

static UINT WINAPI h_GetDriveTypeA(LPCSTR root)
{
    int i;
    UINT t;
    EnterCriticalSection(&g_fsc_cs);
    i = fsc_slot(root);
    if (i >= 0 && g_fsc[i].have_type) {
        t = g_fsc[i].type;
        g_fsc_hits++;
        LeaveCriticalSection(&g_fsc_cs);
        return t;
    }
    LeaveCriticalSection(&g_fsc_cs);
    t = fsc_GetDriveTypeA(root);
    if (t != DRIVE_UNKNOWN && t != DRIVE_NO_ROOT_DIR) {
        EnterCriticalSection(&g_fsc_cs);
        if (i >= 0) { g_fsc[i].type = t; g_fsc[i].have_type = 1; }
        g_fsc_misses++;
        LeaveCriticalSection(&g_fsc_cs);
    }
    return t;
}

static void fscache_setup(HMODULE exe)
{
    int n = 0;
    if (!ini_int("CacheDriveInfo", 1)) { LOG("drive info cache: off (CacheDriveInfo=0)"); return; }
    InitializeCriticalSection(&g_fsc_cs);
    fsc_GetVolumeInformationA = (fsc_GetVolumeInformationA_fn)iat_hook(exe, "KERNEL32.dll", "GetVolumeInformationA", (void *)h_GetVolumeInformationA);
    fsc_GetDiskFreeSpaceA = (fsc_GetDiskFreeSpaceA_fn)iat_hook(exe, "KERNEL32.dll", "GetDiskFreeSpaceA", (void *)h_GetDiskFreeSpaceA);
    fsc_GetDriveTypeA = (fsc_GetDriveTypeA_fn)iat_hook(exe, "KERNEL32.dll", "GetDriveTypeA", (void *)h_GetDriveTypeA);
    n = !!fsc_GetVolumeInformationA + !!fsc_GetDiskFreeSpaceA + !!fsc_GetDriveTypeA;
    if (!fsc_GetVolumeInformationA) fsc_GetVolumeInformationA = GetVolumeInformationA;
    if (!fsc_GetDiskFreeSpaceA) fsc_GetDiskFreeSpaceA = GetDiskFreeSpaceA;
    if (!fsc_GetDriveTypeA) fsc_GetDriveTypeA = GetDriveTypeA;
    LOG("drive info cache: on (%d of 3 calls hooked: volume details and drive type kept, free space kept 5 s)", n);
}

/* for the log at exit or in the profiler */
static void fscache_report(void)
{
    if (g_fsc_hits || g_fsc_misses)
        LOG("drive info cache: %ld answered from the cache, %ld asked Windows", g_fsc_hits, g_fsc_misses);
}
