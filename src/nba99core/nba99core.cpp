/*
 * nba99core.dll - NBA Live 99 compatibility core (release 0.1, stage 1).
 *
 * Loaded into the unmodified NBAWIN.EXE v1.1 by the launcher before any game
 * code runs (process started suspended, LoadLibraryA queued as an APC). Makes a
 * plain copy of the disc's game folder run on Windows 11 with no disc, no
 * installer, no install.dat and no registry entries. Nothing in the game's own
 * files or in the registry is changed. Addresses are for the v1.1 NBAWIN.EXE
 * (2,488,320 bytes, SHA-256 3563b929..., PE timestamp 0x36C878D2); function
 * names are the game's own (tools/watcom_names.py).
 *
 *  1. CD check: check_for_cd (0x4013D0) has no result: its only caller,
 *     init_system (call at 0x40193F), ignores EAX. It looks for a CD-ROM at the
 *     drive letter of the CD folder from install.dat and for NBAWIN.EXE there,
 *     and otherwise shows GLOBALTXT_INVALID_CDROM_DRIVE / GLOBALTXT_INSERT_CD
 *     and quits. Its first byte becomes "ret" (it has pushed nothing yet). No
 *     other disc check exists in the exe (GetDriveTypeA is only called there;
 *     no GetVolumeInformationA or GetLogicalDriveStringsA calls).
 *  2. install.dat: the game checks that <exe folder>install.dat exists
 *     (file_checkinstalldat, through GetFileAttributesA) and reads
 *     "install.dat" from the current folder (file_init, fscanf "%s %s %s").
 *     Both are redirected to a generated %TEMP%\NBALive99\INSTALL.DAT: every
 *     folder as an HD line, then END, the CD folder and the language. Tokens are
 *     whitespace separated (no fixed columns, unlike 98), so the CD folder must
 *     not contain spaces: we write ".\" (the game folder, relative), which is
 *     where the CD paths are used (check_for_cd, file_getvpath for CD lines,
 *     UserInput_LoadConfig for inpdevs.ini). The speech and text-art "update"
 *     folders are not in install.dat: file_init adds them itself.
 *  3. Registry: the exe's RegOpenKeyExA / RegQueryValueExA / RegSetValueExA /
 *     RegFlushKey / RegCloseKey are hooked. HKLM\SOFTWARE\EA Sports\NBA Live 99
 *     (any case, with or without WOW6432Node) is answered from a virtual key and
 *     never touches the real registry. Other keys are read as normal; writes to
 *     them are logged and not made (the game turns off Internet Explorer's
 *     autodial at start, NET_setautodial), so the real registry stays as it was.
 *     With the "3dfx" argument the game does not read its key at all
 *     (check_commandline skips getregistry3dsetting and keeps
 *     3rash\voodooa.dll); the virtual key covers a start without it.
 *  4. GlobalMemoryStatus clamped to 512 MB (as 98).
 *  5. DirectDraw (KeepDesktopMode): EA's VOODOOA.DLL and the CTRLS.DLL
 *     front-end library import DirectDrawCreate; the exe imports it too but
 *     never calls it. Every module the exe loads with LoadLibraryA gets its
 *     DirectDrawCreate import patched: no display mode change, normal
 *     cooperative level, so our glide2x.dll keeps the desktop mode.
 *  6. Threads: every thread the exe creates, every timer callback it sets and
 *     every TerminateThread call is logged; the process gets all CPUs back if
 *     it was started on fewer. (98's CPU pinning and safe thread termination
 *     were removed in 0.3.0: 99 never showed the problems they fixed.)
 *  7. Front-end mouse (mouse.inc): the menus are 640x480 Win32 child windows;
 *     screen points are moved between the scaled picture and those windows.
 *  8. THRASH driver log (Debug tool): [nba99] ThrashLog=1 makes the game load
 *     3rash\voodoologa.dll (thrash_log) instead of 3rash\voodooa.dll; it logs
 *     every THRASH call and forwards to the real driver.
 *     [nba99] EacsndLog=1 does the same for the sound DLL: the exe's
 *     LoadLibraryA("eacsnd.dll") loads eacsnd_log.dll, which logs every call
 *     to EA's eacsnd.dll and its callbacks into the game.
 *  9. Front-end labels (wintext.inc): WM_SETTEXT to the game's text controls
 *     goes straight to their window procedure with the game's own string
 *     pointer, as on Windows 98 (NT passes a temporary copy, which the
 *     controls keep: garbled labels such as "P\xC2Q" (P, A circumflex, Q) and "I").
 * 10. Alt-Tab in the menus (alttab.inc): the front end waits for good once
 *     Windows minimises the game window; the exe's IsIconic answers "not
 *     minimised" to ferender_drawframe only, so the menus keep drawing.
 * 11. Gamepads (pad_input.inc, 0.4.0): the exe's WINMM joystick calls are
 *     answered from SDL3 gamepads (up to the game's 4 controller slots), with
 *     our own pad names, 12 buttons, triggers as buttons and the D-pad on X/Y.
 *     pad_ini.inc serves inpdevs.ini with our section per pad family (button
 *     names and our default layout) and makes A/Cross accept, B/Circle back in
 *     the menus. rumble.inc vibrates the pad of the human concerned on dunks,
 *     blocks, steals, contact and other named game events.
 *
 * Settings: nba99fix.ini next to the exe, section [nba99] (all optional);
 * gamepads in [input].
 * Log: logs\<session>\nba99core.log (Debug build, or Log=1).
 */
#define WIN32_LEAN_AND_MEAN
#include "nba99_cxx.h"   /* C++ build settings, first */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "nba99_build.h"

NBA99_C_BEGIN
/* ------------------------------------------------------------------------- */
/* logging                                                                   */
/* ------------------------------------------------------------------------- */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static LARGE_INTEGER g_qpf, g_qpc0;
static char g_exe_dir[MAX_PATH];      /* with trailing backslash */
static char g_ini[MAX_PATH];
static int g_pad_on;                  /* gamepads through SDL3 (pad_input.inc) */
static int pad_name_key(const char *sub);

/* case-insensitive strstr (ASCII) */
static const char *strstr_i(const char *s, const char *f)
{
    size_t n = strlen(f);
    for (; *s; s++) if (!_strnicmp(s, f, n)) return s;
    return NULL;
}

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void logf_(const char *fmt, ...)
{
    char buf[1200];
    int n;
    va_list ap;
    DWORD w;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = _snprintf(buf, 32, "[%9.1f ms] ", now_ms());
    va_start(ap, fmt);
    n += _vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    WriteFile(g_log, buf, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&g_cs);
}
#define LOG logf_

static int ini_int(const char *key, int def)
{
    return (int)GetPrivateProfileIntA("nba99", key, def, g_ini);
}

/* "EXE!0x00401234" for the game exe, "name.dll+0x1234" for other modules */
static void addr_name(uint32_t a, char *out, int cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH], *b;
    if (!a) { _snprintf(out, cap, "0"); return; }
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
#define CALLER(buf) addr_name((uint32_t)(uintptr_t)__builtin_return_address(0), buf, sizeof buf)

/* ------------------------------------------------------------------------- */
/* import table hooking                                                      */
/* ------------------------------------------------------------------------- */
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
            if (orig == repl) return orig;   /* already ours */
            if (!VirtualProtect(&addrs->u1.Function, 4, PAGE_EXECUTE_READWRITE, &old)) return NULL;
            addrs->u1.Function = (DWORD)(uintptr_t)repl;
            VirtualProtect(&addrs->u1.Function, 4, old, &old);
            return orig;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* exe identification and the CD check                                       */
/* ------------------------------------------------------------------------- */
#define EXE_TIMESTAMP_V11 0x36C878D2u   /* NBAWIN.EXE v1.1 (EA patch) */
#define VA_CHECK_FOR_CD   0x004013D0u   /* check_for_cd */
#define VA_TXT_INSERT_CD  0x005D3510u   /* "GLOBALTXT_INSERT_CD" */
#define VA_INSTALL_DAT    0x005D78DCu   /* "install.dat" */
#define VA_DRIVER_PATH    0x005FD0C4u   /* "3rash\voodooa.dll", 256 bytes: the THRASH driver the game loads */
static int g_is_v11;

static int identify_exe(void)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    static const BYTE k_cd_start[9] = { 0x53, 0x51, 0x52, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00 };   /* push ebx/ecx/edx; sub esp, 0x104 */
    if ((uintptr_t)base != 0x400000) return 0;
    if (nt->FileHeader.TimeDateStamp != EXE_TIMESTAMP_V11) {
        LOG("exe timestamp 0x%08lx is not NBAWIN.EXE v1.1 (0x%08lx): no code patches applied",
            (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)EXE_TIMESTAMP_V11);
        return 0;
    }
    if (memcmp((const void *)(uintptr_t)VA_TXT_INSERT_CD, "GLOBALTXT_INSERT_CD", 20) ||
        memcmp((const void *)(uintptr_t)VA_INSTALL_DAT, "install.dat", 12) ||
        _stricmp((const char *)(uintptr_t)VA_DRIVER_PATH, "3rash\\voodooa.dll")) {
        LOG("exe v1.1 timestamp matches but its strings are not where expected: no code patches applied");
        return 0;
    }
    if (memcmp((const void *)(uintptr_t)VA_CHECK_FOR_CD, k_cd_start, sizeof k_cd_start)) {
        LOG("exe v1.1 timestamp matches but check_for_cd (0x%08X) starts %02X %02X: a changed exe (no-CD crack?); no code patches applied",
            VA_CHECK_FOR_CD, ((BYTE *)(uintptr_t)VA_CHECK_FOR_CD)[0], ((BYTE *)(uintptr_t)VA_CHECK_FOR_CD)[1]);
        return 0;
    }
    return 1;
}

static void patch_cd_check(void)
{
    BYTE *p = (BYTE *)(uintptr_t)VA_CHECK_FOR_CD;
    DWORD old;
    if (!ini_int("NoCDCheck", 1)) { LOG("CD check: left as is (NoCDCheck=0)"); return; }
    /* check_for_cd returns nothing (init_system ignores EAX) and has pushed nothing at its
       first byte, so "ret" there skips it exactly as if a CD had been found */
    if (VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) {
        p[0] = 0xC3;
        VirtualProtect(p, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 1);
        LOG("CD check: check_for_cd (0x%08X) skipped (ret)", VA_CHECK_FOR_CD);
    }
}

/* ThrashLog=1 (Debug build only): load our logging proxy 3rash\voodoologa.dll instead of
   3rash\voodooa.dll. The Main build never loads it, whatever nba99fix.ini or the folder holds:
   Main writes no logs. */
static int thrashlog_on(void) { return NBA99_DEBUG && ini_int("ThrashLog", 0); }

static void select_thrash_driver(void)
{
    char *drv = (char *)(uintptr_t)VA_DRIVER_PATH, path[MAX_PATH];
    DWORD old;
    if (!thrashlog_on()) return;
    _snprintf(path, sizeof path, "%s3rash\\voodoologa.dll", g_exe_dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        LOG("THRASH driver: ThrashLog=1 but 3rash\\voodoologa.dll is missing (it is in the Debug release); using voodooa.dll");
        return;
    }
    if (VirtualProtect(drv, 32, PAGE_READWRITE, &old)) {
        strcpy(drv, "3rash\\voodoologa.dll");   /* still starts with "voodoo": the game treats it as the Voodoo driver */
        VirtualProtect(drv, 32, old, &old);
        LOG("THRASH driver: 3rash\\voodoologa.dll (thrash_log, forwards to voodooa.dll)");
    }
}

/* ------------------------------------------------------------------------- */
/* install.dat                                                               */
/* ------------------------------------------------------------------------- */
typedef HANDLE (WINAPI *CreateFileA_fn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef DWORD (WINAPI *GetFileAttributesA_fn)(LPCSTR);
typedef HANDLE (WINAPI *FindFirstFileA_fn)(LPCSTR, LPWIN32_FIND_DATAA);
static CreateFileA_fn o_CreateFileA;
static GetFileAttributesA_fn id_GetFileAttributesA;
static FindFirstFileA_fn id_FindFirstFileA;
static char g_install_dat[MAX_PATH];   /* generated copy */
static int g_install_ready;

/* The retail layout: "HD", a space, the name padded to 18 characters, the folder
   relative to the game folder. The parser (file_init) reads whitespace-separated
   tokens, so the columns are only kept to match the retail file. */
static const char *const k_install_dirs[][2] = {
    {"febgs", "art\\frontend\\backgrnd\\febgs"}, {"logos", "art\\frontend\\foregrnd\\logos"},
    {"ovllogos", "art\\frontend\\foregrnd\\logos\\overlay"}, {"feanims", "art\\frontend\\foregrnd\\anims"},
    {"buttons", "art\\frontend\\foregrnd\\buttons"}, {"fonts", "art\\frontend\\foregrnd\\fonts"},
    {"textart", "art\\frontend\\foregrnd\\etextart"}, {"miscfe", "art\\frontend\\foregrnd\\misc"},
    {"sound", "sound"}, {"songs", "songs"}, {"ditty", "ditty"}, {"playspch", "speech\\playspch"},
    {"gamespch", "speech\\gamespch"}, {"anims", "art\\ingame\\anims"}, {"plrdata", "art\\ingame\\players"},
    {"updateplrdata", "art\\ingame\\players\\update"}, {"teamdata", "art\\ingame\\teams"},
    {"miscgame", "art\\ingame\\misc"}, {"movies", "movies"}, {"csv", "csv"}, {"dbdata", "dbdata"},
    {"origdb", "origdb"}, {"saves", "saves"}, {"layouts", "art\\frontend\\layout\\layoute"},
    {"userplrdata", "art\\ingame\\players\\user"}, {"userlogos", "art\\frontend\\foregrnd\\logos\\user"},
    {"usergamespch", "speech\\gamespch\\user"}, {"userplayspch", "speech\\playspch\\user"},
};

static int dir_exists(const char *rel)
{
    char p[MAX_PATH];
    DWORD a;
    _snprintf(p, sizeof p, "%s%s", g_exe_dir, rel);
    p[sizeof p - 1] = 0;
    a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void build_install_dat(void)
{
    char tmpdir[MAX_PATH], own[MAX_PATH], out[4096];
    HANDLE f;
    DWORD w;
    size_t len = 0;
    int i, missing = 0;

    _snprintf(own, sizeof own, "%sinstall.dat", g_exe_dir);
    if (GetFileAttributesA(own) != INVALID_FILE_ATTRIBUTES)
        LOG("install.dat: the game folder has its own install.dat; it is not used (the game reads ours)");
#define EMIT(...) do { int _n = _snprintf(out + len, sizeof out - len, __VA_ARGS__); if (_n > 0) len += (size_t)_n; } while (0)
    for (i = 0; i < (int)(sizeof k_install_dirs / sizeof k_install_dirs[0]); i++) {
        EMIT("HD %-18s%s\r\n", k_install_dirs[i][0], k_install_dirs[i][1]);
        if (!dir_exists(k_install_dirs[i][1])) {
            missing++;
            LOG("install.dat: folder %s is missing from the game folder (see the README's install steps)", k_install_dirs[i][1]);
        }
    }
    /* after END: the CD folder (no spaces allowed: tokens are whitespace separated), then the language */
    EMIT("END\r\n.\\\r\nenglish\r\n");
#undef EMIT
    GetTempPathA(sizeof tmpdir, tmpdir);
    strncat(tmpdir, "NBALive99", sizeof tmpdir - strlen(tmpdir) - 1);
    CreateDirectoryA(tmpdir, NULL);
    _snprintf(g_install_dat, sizeof g_install_dat, "%s\\INSTALL.DAT", tmpdir);
    f = o_CreateFileA(g_install_dat, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, out, (DWORD)len, &w, NULL);
        CloseHandle(f);
        g_install_ready = 1;
        LOG("install.dat: %d folders (all on the hard disk%s), CD folder .\\ (the game folder), english; the game reads %s",
            (int)(sizeof k_install_dirs / sizeof k_install_dirs[0]), missing ? ", some missing" : "", g_install_dat);
    } else {
        LOG("install.dat: could not write %s (error %lu); the game will stop with \"INSTALL.DAT corrupted\"",
            g_install_dat, GetLastError());
    }
}

static int is_install_dat(const char *path)
{
    const char *b1, *b2, *b = path;
    if (!path) return 0;
    b1 = strrchr(path, '\\'); b2 = strrchr(path, '/');
    if (b1 && b1 + 1 > b) b = b1 + 1;
    if (b2 && b2 + 1 > b) b = b2 + 1;
    return _stricmp(b, "install.dat") == 0;
}

static void (*g_prof_open)(const char *name, double ms);   /* Debug stall profiler (profile.inc) */
static int g_log_opens;

/* 0.3.0 file speed (from NBA Live 98): drive info cache, temp file buffer, speech in memory */
#include "fscache.inc"
#include "tempbuf.inc"
#include "spchmem.inc"

static HANDLE WINAPI h_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h;
    DWORD err;
    if (g_install_ready && is_install_dat(name)) {
        LOG("install.dat: game opened %s (access 0x%lx); redirected to %s", name, access, g_install_dat);
        return o_CreateFileA(g_install_dat, access, share, sa, disp, flags, tmpl);
    }
    if (g_log_opens && name) LOG("open %s (access 0x%lx, disposition %lu, flags 0x%lx)", name, access, disp, flags);
    if ((h = spm_try_open(name, access, disp, flags)) != NULL) {
        if (g_log_opens) LOG("open %s: in memory", name);
        if (g_prof_open && name) g_prof_open(name, 0.0);
        return h;
    }
    if (spm_known_missing(name, access, disp)) {
        if (g_log_opens) LOG("open %s: known missing", name);
        if (g_prof_open && name) g_prof_open(name, 0.0);
        return INVALID_HANDLE_VALUE;
    }
    if (g_tmb_on) tmb_before_open(name);
    if (g_prof_open && name) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        h = o_CreateFileA(name, access, share, sa, disp, flags, tmpl);
        err = GetLastError();
        QueryPerformanceCounter(&b);
        g_prof_open(name, (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpf.QuadPart);
    } else {
        h = o_CreateFileA(name, access, share, sa, disp, flags, tmpl);
        err = GetLastError();
    }
    spm_after_open(name, access, disp, h, err);
    if (g_tmb_on) tmb_opened(h, name, access, disp, flags);
    SetLastError(err);
    return h;
}

static DWORD WINAPI h_GetFileAttributesA(LPCSTR name)
{
    if (g_install_ready && is_install_dat(name)) {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1)) LOG("install.dat: game checked %s; answered from %s", name, g_install_dat);
        return id_GetFileAttributesA(g_install_dat);
    }
    return id_GetFileAttributesA(name);
}

static HANDLE WINAPI h_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd)
{
    if (g_install_ready && is_install_dat(name)) {
        LOG("install.dat: game looked for %s; answered from %s", name, g_install_dat);
        return id_FindFirstFileA(g_install_dat, fd);
    }
    return id_FindFirstFileA(name, fd);
}

/* ------------------------------------------------------------------------- */
/* registry: a virtual HKLM\SOFTWARE\EA Sports\NBA Live 99                   */
/* ------------------------------------------------------------------------- */
typedef LONG (WINAPI *RegOpenKeyExA_fn)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI *RegQueryValueExA_fn)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *RegSetValueExA_fn)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);
typedef LONG (WINAPI *RegKey_fn)(HKEY);
static RegOpenKeyExA_fn o_RegOpenKeyExA;
static RegQueryValueExA_fn o_RegQueryValueExA;
static RegSetValueExA_fn o_RegSetValueExA;
static RegKey_fn o_RegFlushKey, o_RegCloseKey;

#define VKEY_BASE 0x7A990000u          /* virtual key handles: VKEY_BASE + n (never real handles) */
#define VKEY_MAX 16
static volatile LONG g_vkey_open[VKEY_MAX];

typedef struct { char name[40]; DWORD type; char sz[MAX_PATH]; DWORD dw; } vval;
#define VVAL_MAX 24
static vval g_vvals[VVAL_MAX];
static int g_nvvals;
static CRITICAL_SECTION g_reg_cs;

static void vval_set(const char *name, DWORD type, const char *sz, DWORD dw)
{
    int i;
    for (i = 0; i < g_nvvals; i++) if (!_stricmp(g_vvals[i].name, name)) break;
    if (i == g_nvvals) { if (g_nvvals >= VVAL_MAX) return; g_nvvals++; }
    _snprintf(g_vvals[i].name, sizeof g_vvals[i].name, "%s", name);
    g_vvals[i].type = type;
    _snprintf(g_vvals[i].sz, sizeof g_vvals[i].sz, "%s", sz ? sz : "");
    g_vvals[i].dw = dw;
}

static void registry_values(void)
{
    char root[MAX_PATH], drive[4] = "";
    strcpy(root, g_exe_dir);
    if (strlen(root) > 3) root[strlen(root) - 1] = 0;   /* no trailing backslash, as an installer writes it */
    if (g_exe_dir[1] == ':') _snprintf(drive, sizeof drive, "%c:", g_exe_dir[0]);
    InitializeCriticalSection(&g_reg_cs);
    vval_set("Install Dir", REG_SZ, root, 0);
    vval_set("CD Drive", REG_SZ, drive, 0);
    vval_set("Language", REG_SZ, "english", 0);
    vval_set("Thrash Driver", REG_SZ, thrashlog_on() ? "voodoolog" : "voodoo", 0);
    vval_set("3D Card", REG_SZ, "3Dfx Voodoo", 0);
    vval_set("3D Device Description", REG_SZ, "3Dfx Voodoo", 0);
    vval_set("Group", REG_SZ, "3Dfx", 0);
    vval_set("Hardware Acceleration", REG_DWORD, NULL, 1);
    vval_set("Triple Buffer", REG_DWORD, NULL, 0);
    vval_set("D3D Device", REG_DWORD, NULL, 0);
}

static int is_vkey(HKEY k)
{
    uint32_t v = (uint32_t)(uintptr_t)k;
    return v >= VKEY_BASE && v < VKEY_BASE + VKEY_MAX && g_vkey_open[v - VKEY_BASE];
}

/* "SOFTWARE\EA Sports\NBA Live 99", any case, with or without WOW6432Node, under HKLM */
static int is_our_key(HKEY root, const char *sub)
{
    char s[MAX_PATH], *p;
    size_t n;
    if (root != HKEY_LOCAL_MACHINE || !sub) return 0;
    _snprintf(s, sizeof s, "%s", sub);
    s[sizeof s - 1] = 0;
    for (p = s; *p; p++) if (*p == '/') *p = '\\';
    n = strlen(s);
    while (n && s[n - 1] == '\\') s[--n] = 0;
    p = s;
    while (*p == '\\') p++;
    return !_stricmp(p, "SOFTWARE\\EA Sports\\NBA Live 99") || !_stricmp(p, "SOFTWARE\\WOW6432Node\\EA Sports\\NBA Live 99");
}

static const char *root_name(HKEY k)
{
    if (k == HKEY_LOCAL_MACHINE) return "HKLM";
    if (k == HKEY_CURRENT_USER) return "HKCU";
    if (k == HKEY_CLASSES_ROOT) return "HKCR";
    if (k == HKEY_USERS) return "HKU";
    return "key";
}

static LONG WINAPI h_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    char at[96];
    CALLER(at);
    if (pad_name_key(sub)) return ERROR_FILE_NOT_FOUND;
    if (is_our_key(root, sub)) {
        int i;
        for (i = 0; i < VKEY_MAX; i++)
            if (!InterlockedCompareExchange(&g_vkey_open[i], 1, 0)) break;
        if (i == VKEY_MAX) { LOG("registry: too many open virtual keys"); return ERROR_TOO_MANY_OPEN_FILES; }
        if (out) *out = (HKEY)(uintptr_t)(VKEY_BASE + i);
        LOG("registry: open %s\\%s (access 0x%lx) from %s: virtual key", root_name(root), sub, (unsigned long)sam, at);
        return ERROR_SUCCESS;
    }
    {
        LONG r = o_RegOpenKeyExA(root, sub, opt, sam, out);
        LOG("registry: open %s\\%s (access 0x%lx) from %s: real key, result %ld", root_name(root), sub ? sub : "",
            (unsigned long)sam, at, r);
        return r;
    }
}

static LONG WINAPI h_RegQueryValueExA(HKEY k, LPCSTR name, LPDWORD res, LPDWORD type, LPBYTE data, LPDWORD cb)
{
    char at[96];
    (void)res;
    CALLER(at);
    if (is_vkey(k)) {
        int i;
        LONG r = ERROR_FILE_NOT_FOUND;
        char shown[MAX_PATH + 16] = "not found";
        EnterCriticalSection(&g_reg_cs);
        for (i = 0; i < g_nvvals; i++) if (name && !_stricmp(g_vvals[i].name, name)) break;
        if (i < g_nvvals) {
            const vval *v = &g_vvals[i];
            DWORD need = v->type == REG_DWORD ? 4 : (DWORD)strlen(v->sz) + 1;
            if (type) *type = v->type;
            if (!data) r = ERROR_SUCCESS;
            else if (!cb || *cb < need) r = ERROR_MORE_DATA;
            else {
                if (v->type == REG_DWORD) memcpy(data, &v->dw, 4); else memcpy(data, v->sz, need);
                r = ERROR_SUCCESS;
            }
            if (cb) *cb = need;
            if (v->type == REG_DWORD) _snprintf(shown, sizeof shown, "%lu", (unsigned long)v->dw);
            else _snprintf(shown, sizeof shown, "\"%s\"", v->sz);
        }
        LeaveCriticalSection(&g_reg_cs);
        LOG("registry: read \"%s\" from %s: %s (result %ld)", name ? name : "(default)", at, shown, r);
        return r;
    }
    if (pad_name_key(name)) return ERROR_FILE_NOT_FOUND;   /* the game's once-a-second joystick name read */
    {
        LONG r = o_RegQueryValueExA(k, name, res, type, data, cb);
        LOG("registry: read \"%s\" from a real key, from %s: result %ld", name ? name : "(default)", at, r);
        return r;
    }
}

static LONG WINAPI h_RegSetValueExA(HKEY k, LPCSTR name, DWORD res, DWORD type, const BYTE *data, DWORD cb)
{
    char at[96];
    (void)res;
    CALLER(at);
    if (is_vkey(k)) {
        EnterCriticalSection(&g_reg_cs);
        if (type == REG_DWORD && data && cb >= 4) vval_set(name ? name : "", REG_DWORD, NULL, *(const DWORD *)data);
        else if ((type == REG_SZ || type == REG_EXPAND_SZ) && data) {
            char sz[MAX_PATH];
            _snprintf(sz, sizeof sz, "%.*s", (int)(cb < sizeof sz ? cb : sizeof sz - 1), (const char *)data);
            vval_set(name ? name : "", REG_SZ, sz, 0);
        }
        LeaveCriticalSection(&g_reg_cs);
        LOG("registry: write \"%s\" (type %lu, %lu bytes) from %s: kept in the virtual key for this session",
            name ? name : "(default)", (unsigned long)type, (unsigned long)cb, at);
        return ERROR_SUCCESS;
    }
    if (ini_int("RegistryWrites", 0)) {
        LONG r = o_RegSetValueExA(k, name, res, type, data, cb);
        LOG("registry: write \"%s\" to a real key from %s: written (RegistryWrites=1), result %ld", name ? name : "(default)", at, r);
        return r;
    }
    LOG("registry: write \"%s\" (type %lu) to a real key from %s: not written (the registry is left as it was)",
        name ? name : "(default)", (unsigned long)type, at);
    return ERROR_SUCCESS;
}

static LONG WINAPI h_RegFlushKey(HKEY k)
{
    if (is_vkey(k)) return ERROR_SUCCESS;
    return o_RegFlushKey(k);
}

static LONG WINAPI h_RegCloseKey(HKEY k)
{
    uint32_t v = (uint32_t)(uintptr_t)k;
    if (is_vkey(k)) { InterlockedExchange(&g_vkey_open[v - VKEY_BASE], 0); return ERROR_SUCCESS; }
    return o_RegCloseKey(k);
}

static void registry_setup(HMODULE exe)
{
    registry_values();
    o_RegOpenKeyExA = (RegOpenKeyExA_fn)iat_hook(exe, "ADVAPI32.dll", "RegOpenKeyExA", (void *)h_RegOpenKeyExA);
    o_RegQueryValueExA = (RegQueryValueExA_fn)iat_hook(exe, "ADVAPI32.dll", "RegQueryValueExA", (void *)h_RegQueryValueExA);
    o_RegSetValueExA = (RegSetValueExA_fn)iat_hook(exe, "ADVAPI32.dll", "RegSetValueExA", (void *)h_RegSetValueExA);
    o_RegFlushKey = (RegKey_fn)iat_hook(exe, "ADVAPI32.dll", "RegFlushKey", (void *)h_RegFlushKey);
    o_RegCloseKey = (RegKey_fn)iat_hook(exe, "ADVAPI32.dll", "RegCloseKey", (void *)h_RegCloseKey);
    LOG("registry: virtual HKLM\\SOFTWARE\\EA Sports\\NBA Live 99 %s (Thrash Driver \"%s\")",
        o_RegOpenKeyExA && o_RegQueryValueExA && o_RegSetValueExA && o_RegFlushKey && o_RegCloseKey ? "on" : "INCOMPLETE",
        thrashlog_on() ? "voodoolog" : "voodoo");
    if (!o_RegOpenKeyExA) o_RegOpenKeyExA = RegOpenKeyExA;
    if (!o_RegQueryValueExA) o_RegQueryValueExA = RegQueryValueExA;
    if (!o_RegSetValueExA) o_RegSetValueExA = RegSetValueExA;
    if (!o_RegFlushKey) o_RegFlushKey = RegFlushKey;
    if (!o_RegCloseKey) o_RegCloseKey = RegCloseKey;
}

/* ------------------------------------------------------------------------- */
/* GlobalMemoryStatus clamp (replaces the GlobalMemoryStatusLie shim)        */
/* ------------------------------------------------------------------------- */
typedef void (WINAPI *GlobalMemoryStatus_fn)(LPMEMORYSTATUS);
static GlobalMemoryStatus_fn o_GlobalMemoryStatus;
#define MEM_CAP 0x20000000u   /* 512 MB: far more than the game needs, safe in signed 32-bit maths */

static void WINAPI h_GlobalMemoryStatus(LPMEMORYSTATUS ms)
{
    static LONG logged;
    o_GlobalMemoryStatus(ms);
    if (ms->dwTotalPhys > MEM_CAP) ms->dwTotalPhys = MEM_CAP;
    if (ms->dwAvailPhys > MEM_CAP) ms->dwAvailPhys = MEM_CAP;
    if (ms->dwTotalPageFile > MEM_CAP) ms->dwTotalPageFile = MEM_CAP;
    if (ms->dwAvailPageFile > MEM_CAP) ms->dwAvailPageFile = MEM_CAP;
    if (ms->dwTotalVirtual > MEM_CAP) ms->dwTotalVirtual = MEM_CAP;
    if (ms->dwAvailVirtual > MEM_CAP) ms->dwAvailVirtual = MEM_CAP;
    if (!InterlockedExchange(&logged, 1))
        LOG("GlobalMemoryStatus: reporting at most 512 MB to the game");
}

/* ------------------------------------------------------------------------- */
/* threads: logging                                                          */
/* ------------------------------------------------------------------------- */
/* NBA Live 98 needed two thread fixes: CPU pinning (PinGameThreads, GameCPU) and
   safe thread termination (SafeTerminate). Both were carried into 99 switched off
   and removed in 0.3.0 (25 Nov 2026): in every 99 log (both test PCs, up to
   7.7 hours) the game never called TerminateThread, never set an affinity itself
   and ran on all CPUs without a freeze. TerminateThread calls are still logged. */
typedef BOOL (WINAPI *TerminateThread_fn)(HANDLE, DWORD);
static TerminateThread_fn o_TerminateThread;

static BOOL WINAPI h_TerminateThread(HANDLE h, DWORD code)
{
    char at[96];
    CALLER(at);
    /* GetThreadId is Vista and later: looked up, not imported (Windows 98 rule) */
    static DWORD (WINAPI *get_tid)(HANDLE);
    static int looked;
    if (!looked++) get_tid = (DWORD (WINAPI *)(HANDLE))GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadId");
    LOG("TerminateThread(tid %lu) from %s", get_tid ? get_tid(h) : 0, at);
    return o_TerminateThread(h, code);
}

typedef HANDLE (WINAPI *CreateThread_fn)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
static CreateThread_fn o_CreateThread;
static volatile LONG g_nknown;
static DWORD g_known_tid[64];
static void note_thread(DWORD tid) { LONG i = InterlockedIncrement(&g_nknown) - 1; if (i < 64) g_known_tid[i] = tid; }

static HANDLE WINAPI h_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T ss, LPTHREAD_START_ROUTINE fn,
                                    LPVOID arg, DWORD flags, LPDWORD tid_out)
{
    DWORD tid = 0;
    HANDLE h;
    char at[96], entry[96];
    CALLER(at);
    h = o_CreateThread(sa, ss, fn, arg, flags, &tid);
    if (tid_out) *tid_out = tid;
    if (tid) note_thread(tid);
    addr_name((uint32_t)(uintptr_t)fn, entry, sizeof entry);
    LOG("game thread created: tid %lu, entry %s (arg 0x%08lx), from %s", tid, entry,
        (unsigned long)(uintptr_t)arg, at);
    return h;
}

#include "sysinfo.inc"
#include "timing.inc"

/* timeSetEvent: the game's timer callbacks run through timer_thunk (counted for
 * the timing log) */
typedef MMRESULT (WINAPI *timeSetEvent_fn)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
static timeSetEvent_fn o_timeSetEvent;
#define MAX_TIMERS 16
static struct { LPTIMECALLBACK cb; DWORD_PTR user; } g_timers[MAX_TIMERS];
static LONG g_ntimers;
static DWORD g_timer_tid[8];
static LONG g_ntimer_tid;

static void CALLBACK timer_thunk(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR d1, DWORD_PTR d2)
{
    LONG slot = (LONG)user;
    DWORD me = GetCurrentThreadId();
    LONG i, n = g_ntimer_tid;
    int known = 0;
    for (i = 0; i < n && i < 8; i++) if (g_timer_tid[i] == me) { known = 1; break; }
    if (!known) {
        i = InterlockedIncrement(&g_ntimer_tid) - 1;
        if (i < 8) g_timer_tid[i] = me;
        note_thread(me);
        LOG("timer thread %lu runs game timer callback 0x%08lx", me,
            (unsigned long)(uintptr_t)g_timers[slot].cb);
    }
    timing_on_timer(g_timers[slot].cb);
    g_timers[slot].cb(id, msg, g_timers[slot].user, d1, d2);
    timing_after_timer(g_timers[slot].cb);
}

static MMRESULT WINAPI h_timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK cb, DWORD_PTR user, UINT flags)
{
    LONG slot, n;
    if (flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE) || !cb)
        return o_timeSetEvent(delay, res, cb, user, flags);
    n = g_ntimers;
    for (slot = 0; slot < n; slot++)
        if (g_timers[slot].cb == cb && g_timers[slot].user == user) break;
    if (slot == n) {
        EnterCriticalSection(&g_cs);
        n = g_ntimers;
        for (slot = 0; slot < n; slot++)
            if (g_timers[slot].cb == cb && g_timers[slot].user == user) break;
        if (slot == n) {
            if (n >= MAX_TIMERS) {
                LeaveCriticalSection(&g_cs);
                return o_timeSetEvent(delay, res, cb, user, flags);
            }
            g_timers[n].cb = cb;
            g_timers[n].user = user;
            InterlockedExchange(&g_ntimers, n + 1);
            LOG("timeSetEvent: new timer callback 0x%08lx (user 0x%08lx), first delay %u ms, resolution %u, flags 0x%x",
                (unsigned long)(uintptr_t)cb, (unsigned long)user, delay, res, flags);
        }
        LeaveCriticalSection(&g_cs);
    }
    return o_timeSetEvent(delay, res, timer_thunk, (DWORD_PTR)slot, flags);
}

/* ------------------------------------------------------------------------- */
/* DirectDraw: keep the desktop display mode                                 */
/* EA's Voodoo THRASH driver (and CTRLS.DLL) create a DirectDraw object and  */
/* may take exclusive full-screen control and set 640x480. Our glide2x.dll   */
/* draws to a desktop-sized window, so with KeepDesktopMode=1 (default) the  */
/* mode change is skipped and the cooperative level made normal. The         */
/* interfaces are patched in place (SetCooperativeLevel, SetDisplayMode,     */
/* RestoreDisplayMode), including IDirectDraw2/4/7 from QueryInterface.      */
/* ------------------------------------------------------------------------- */
typedef HRESULT (WINAPI *DDCreate_fn)(GUID *, void **, void *);
typedef HRESULT (WINAPI *DDQI_fn)(void *, const GUID *, void **);
typedef HRESULT (WINAPI *DDCoop_fn)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *DDRestore_fn)(void *);
static DDCreate_fn o_DirectDrawCreate;
static int g_keep_desktop;

#define DD_MAXVT 8
static struct { void **vt; int v2; DDQI_fn qi; DDCoop_fn coop; void *mode; DDRestore_fn restore; } g_ddvt[DD_MAXVT];
static int g_nddvt;

static int dd_find(void *obj)
{
    void **vt = *(void ***)obj;
    int i;
    for (i = 0; i < g_nddvt; i++) if (g_ddvt[i].vt == vt) return i;
    return -1;
}
static void dd_patch(void *obj, int v2);

static HRESULT WINAPI h_dd_qi(void *self, const GUID *iid, void **out)
{
    int i = dd_find(self);
    HRESULT hr = i >= 0 ? g_ddvt[i].qi(self, iid, out) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && out && *out) {
        /* IDirectDraw = 6C14DB80, IDirectDraw2 = B3A6F3E0, IDirectDraw4 = 9C59509A, IDirectDraw7 = 15E65EC0 */
        DWORD d1 = iid->Data1;
        if (d1 == 0x6C14DB80) dd_patch(*out, 0);
        else if (d1 == 0xB3A6F3E0 || d1 == 0x9C59509A || d1 == 0x15E65EC0) dd_patch(*out, 1);
    }
    return hr;
}
static HRESULT WINAPI h_dd_coop(void *self, HWND hwnd, DWORD flags)
{
    int i = dd_find(self);
    DWORD nf = (flags & ~(DWORD)(0x1 | 0x10 | 0x40)) | 0x8;  /* drop FULLSCREEN, EXCLUSIVE, ALLOWMODEX; NORMAL */
    char at[96];
    CALLER(at);
    LOG("DirectDraw SetCooperativeLevel(hwnd 0x%08lx, 0x%lx) from %s -> 0x%lx (desktop mode kept)",
        (unsigned long)(uintptr_t)hwnd, flags, at, nf);
    return i >= 0 ? g_ddvt[i].coop(self, hwnd, nf) : E_FAIL;
}
static HRESULT WINAPI h_dd_mode1(void *self, DWORD w, DWORD h, DWORD bpp)
{
    char at[96];
    (void)self;
    CALLER(at);
    LOG("DirectDraw SetDisplayMode(%lux%lu, %lu bit) from %s skipped: desktop mode kept", w, h, bpp, at);
    return 0;
}
static HRESULT WINAPI h_dd_mode2(void *self, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD fl)
{
    char at[96];
    (void)self; (void)fl;
    CALLER(at);
    LOG("DirectDraw SetDisplayMode(%lux%lu, %lu bit, %lu Hz) from %s skipped: desktop mode kept", w, h, bpp, hz, at);
    return 0;
}
static HRESULT WINAPI h_dd_restore(void *self) { (void)self; return 0; }

static void dd_patch(void *obj, int v2)
{
    void **vt = *(void ***)obj;
    DWORD old;
    int i;
    if (dd_find(obj) >= 0 || g_nddvt >= DD_MAXVT) return;
    i = g_nddvt;
    g_ddvt[i].vt = vt;
    g_ddvt[i].v2 = v2;
    g_ddvt[i].qi = (DDQI_fn)vt[0];
    g_ddvt[i].restore = (DDRestore_fn)vt[19];
    g_ddvt[i].coop = (DDCoop_fn)vt[20];
    g_ddvt[i].mode = vt[21];
    if (!VirtualProtect(vt, 22 * sizeof(void *), PAGE_READWRITE, &old)) { LOG("DirectDraw: cannot patch the interface"); return; }
    vt[0] = (void *)h_dd_qi;
    vt[19] = (void *)h_dd_restore;
    vt[20] = (void *)h_dd_coop;
    vt[21] = v2 ? (void *)h_dd_mode2 : (void *)h_dd_mode1;
    VirtualProtect(vt, 22 * sizeof(void *), old, &old);
    g_nddvt++;
    LOG("DirectDraw: interface %s patched to keep the desktop mode", v2 ? "IDirectDraw2+" : "IDirectDraw");
}

static HRESULT WINAPI h_DirectDrawCreate(GUID *g, void **out, void *outer)
{
    HRESULT hr;
    char at[96];
    CALLER(at);
    if (!o_DirectDrawCreate) o_DirectDrawCreate = (DDCreate_fn)GetProcAddress(LoadLibraryA("ddraw.dll"), "DirectDrawCreate");
    hr = o_DirectDrawCreate ? o_DirectDrawCreate(g, out, outer) : E_FAIL;
    LOG("DirectDrawCreate(guid %s) from %s: 0x%08lx", g ? "given" : "NULL", at, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out && g_keep_desktop) dd_patch(*out, 0);
    return hr;
}

#include "mouse.inc"          /* front-end mouse: the picture vs the 640x480 child windows */
#include "wintext.inc"        /* front-end labels: WM_SETTEXT with the game's own pointer */
#include "alttab.inc"         /* Alt-Tab in the menus: no wait for good while minimised */
#include "pad_input.inc"      /* gamepads through SDL3 (0.4.0) */
#include "pad_ini.inc"        /* our pad sections in a served inpdevs.ini; menu accept/back (0.4.0) */
#include "rumble.inc"         /* rumble from the game's named events (0.4.0) */

/* ------------------------------------------------------------------------- */
/* LoadLibraryA: modules the game loads (the THRASH driver, CTRLS.DLL,       */
/* FONTRES.DLL, EACSND.DLL, ...) get their DirectDrawCreate and LoadLibraryA */
/* imports hooked, so the THRASH driver's own LoadLibrary("glide2x.dll") is  */
/* logged with the file it found.                                            */
/* ------------------------------------------------------------------------- */
typedef HMODULE (WINAPI *LoadLibraryA_fn)(LPCSTR);
static LoadLibraryA_fn o_LoadLibraryA;
static HMODULE WINAPI h_LoadLibraryA(LPCSTR name);

static void hook_module(HMODULE m, const char *name)
{
    void *p;
    if (!m || m == GetModuleHandleA(NULL)) return;
    if (g_keep_desktop) {
        p = iat_hook(m, "DDRAW.dll", "DirectDrawCreate", (void *)h_DirectDrawCreate);
        if (p) {
            if (!o_DirectDrawCreate && p != (void *)h_DirectDrawCreate) o_DirectDrawCreate = (DDCreate_fn)p;
            LOG("module %s: DirectDrawCreate import hooked (keep desktop mode)", name);
        }
    }
    p = iat_hook(m, "KERNEL32.dll", "LoadLibraryA", (void *)h_LoadLibraryA);
    if (p && !o_LoadLibraryA) o_LoadLibraryA = (LoadLibraryA_fn)p;
    {   /* the front-end control library works in the game's 640x480 window like the exe */
        const char *b = strrchr(name, '\\');
        if (!_stricmp(b ? b + 1 : name, "ctrls.dll")) mouse_hook_module(m, name);
    }
}

/* EacsndLog=1 (Debug build only): the exe's LoadLibraryA("eacsnd.dll") loads our logging
   proxy eacsnd_log.dll from the game folder instead; it loads EA's eacsnd.dll by full path and
   passes every call on. Only the exact name "eacsnd.dll" is redirected, so the proxy's own
   load of the real DLL (a full path) is not. Main never loads it: Main writes no logs. */
static int eacsndlog_on(void) { return NBA99_DEBUG && ini_int("EacsndLog", 0); }

static HMODULE WINAPI h_LoadLibraryA(LPCSTR name)
{
    HMODULE before, m;
    char at[96], path[MAX_PATH] = "", proxy[MAX_PATH];
    CALLER(at);
    if (name && !_stricmp(name, "eacsnd.dll") && eacsndlog_on()) {
        _snprintf(proxy, sizeof proxy, "%seacsnd_log.dll", g_exe_dir);
        if (GetFileAttributesA(proxy) == INVALID_FILE_ATTRIBUTES) {
            LOG("sound DLL: EacsndLog=1 but eacsnd_log.dll is missing (it is in the Debug release); using eacsnd.dll");
        } else {
            LOG("sound DLL: LoadLibrary(\"%s\") from %s redirected to %s (eacsnd_log, EacsndLog=1)", name, at, proxy);
            name = proxy;
        }
    }
    before = name ? GetModuleHandleA(name) : NULL;
    m = o_LoadLibraryA(name);
    if (m) GetModuleFileNameA(m, path, sizeof path);
    LOG("LoadLibrary(\"%s\") from %s: %s%s", name ? name : "", at, m ? path : "FAILED",
        m && before ? " (already loaded)" : "");
    if (!m) LOG("LoadLibrary(\"%s\"): error %lu", name ? name : "", GetLastError());
    if (m && !before) hook_module(m, path);
    return m;
}

static void setup_affinity(void)
{
    /* a compatibility setting can start the game on one CPU (seen on the handheld test PC): give it all of them */
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    if (sm && pm != sm) {
        SetProcessAffinityMask(GetCurrentProcess(), sm);
        LOG("process affinity was 0x%lx (system 0x%lx): restored to all CPUs", (unsigned long)pm, (unsigned long)sm);
    }
}

#if NBA99_DEBUG
#include "diag.inc"          /* Debug build: crash reports, thread dumps, watches, stall profiler */
#else
static void start_thread_dump(void) {}
#endif

/* ------------------------------------------------------------------------- */
/* stale nbatemp\ cleanup                                                    */
/* The game keeps scratch files in nbatemp\ in the game folder and deletes   */
/* the folder on a normal quit (tempdir_remove). Alt+F4 or a crash leaves it */
/* behind. Before any game code runs we delete the files directly in it and  */
/* then the folder. A named mutex per game folder makes sure a second copy   */
/* started from the same folder never empties the folder of one running.    */
/* [nba99] CleanTempFolder=1 (default).                                      */
/* ------------------------------------------------------------------------- */
static HANDLE g_folder_mutex;

static void clean_stale_nbatemp(void)
{
    char dir[MAX_PATH], pat[MAX_PATH], f[MAX_PATH], name[96];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD hash = 2166136261u;
    const char *p;
    int files = 0, failed = 0, subdirs = 0;
    if (!ini_int("CleanTempFolder", 1)) { LOG("nbatemp cleanup: off (CleanTempFolder=0)"); return; }
    for (p = g_exe_dir; *p; p++) hash = (hash ^ (unsigned char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p)) * 16777619u;
    _snprintf(name, sizeof name, "NBA99_GAME_FOLDER_%08lx", (unsigned long)hash);
    g_folder_mutex = CreateMutexA(NULL, FALSE, name);
    if (g_folder_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG("nbatemp cleanup: skipped (another copy of the game is running from this folder)");
        return;
    }
    _snprintf(dir, sizeof dir, "%snbatemp", g_exe_dir);
    dir[sizeof dir - 1] = 0;
    {
        DWORD a = GetFileAttributesA(dir);
        if (a == INVALID_FILE_ATTRIBUTES) { LOG("nbatemp cleanup: no stale nbatemp folder"); return; }
        if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            LOG("nbatemp cleanup: %s is not a plain folder, left alone", dir);
            return;
        }
    }
    _snprintf(pat, sizeof pat, "%s\\*", dir);
    pat[sizeof pat - 1] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) subdirs++;
                continue;
            }
            _snprintf(f, sizeof f, "%s\\%s", dir, fd.cFileName);
            f[sizeof f - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesA(f, fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (DeleteFileA(f)) files++; else failed++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!failed && !subdirs && RemoveDirectoryA(dir))
        LOG("nbatemp cleanup: removed a stale nbatemp folder (%d file(s)), left by Alt+F4 or a crash", files);
    else
        LOG("nbatemp cleanup: deleted %d file(s) from a stale nbatemp folder; %d could not be deleted, %d subfolder(s) left alone; folder kept",
            files, failed, subdirs);
}

/* ------------------------------------------------------------------------- */
/* entry                                                                     */
/* ------------------------------------------------------------------------- */
static void init(HINSTANCE self)
{
    char path[MAX_PATH], *s;
    HMODULE exe = GetModuleHandleA(NULL);
    SYSTEMTIME st;

    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    InitializeCriticalSection(&g_cs);
    GetModuleFileNameA(exe, path, sizeof path);
    strcpy(g_exe_dir, path);
    s = strrchr(g_exe_dir, '\\');
    if (s) s[1] = 0;
    _snprintf(g_ini, sizeof g_ini, "%snba99fix.ini", g_exe_dir);
    if (ini_int("Log", NBA99_LOG_DEFAULT)) {
        char logp[MAX_PATH];
        nba99_log_path(g_exe_dir, "nba99core.log", logp, sizeof logp);
        g_log = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    GetLocalTime(&st);
    LOG("nba99core " NBA99_VERSION_LINE "  %04d-%02d-%02d %02d:%02d:%02d  exe %s  command line: %s",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, path, GetCommandLineA());
    (void)self;

    g_is_v11 = identify_exe();
    LOG("exe: %s", g_is_v11 ? "NBAWIN.EXE v1.1 (verified)" : "not the verified v1.1 build: only the generic fixes are applied");
    g_log_opens = NBA99_DEBUG ? ini_int("LogFileOpens", 0) : 0;

    o_CreateFileA = (CreateFileA_fn)iat_hook(exe, "KERNEL32.dll", "CreateFileA", (void *)h_CreateFileA);
    id_GetFileAttributesA = (GetFileAttributesA_fn)iat_hook(exe, "KERNEL32.dll", "GetFileAttributesA", (void *)h_GetFileAttributesA);
    id_FindFirstFileA = (FindFirstFileA_fn)iat_hook(exe, "KERNEL32.dll", "FindFirstFileA", (void *)h_FindFirstFileA);
    o_GlobalMemoryStatus = (GlobalMemoryStatus_fn)iat_hook(exe, "KERNEL32.dll", "GlobalMemoryStatus",
                                                           (void *)h_GlobalMemoryStatus);
    o_CreateThread = (CreateThread_fn)iat_hook(exe, "KERNEL32.dll", "CreateThread", (void *)h_CreateThread);
    o_timeSetEvent = (timeSetEvent_fn)iat_hook(exe, "WINMM.dll", "timeSetEvent", (void *)h_timeSetEvent);
    o_LoadLibraryA = (LoadLibraryA_fn)iat_hook(exe, "KERNEL32.dll", "LoadLibraryA", (void *)h_LoadLibraryA);
    LOG("hooks: CreateFileA %s, GetFileAttributesA %s, FindFirstFileA %s, GlobalMemoryStatus %s, CreateThread %s, timeSetEvent %s, LoadLibraryA %s",
        o_CreateFileA ? "ok" : "MISSING", id_GetFileAttributesA ? "ok" : "MISSING", id_FindFirstFileA ? "ok" : "MISSING",
        o_GlobalMemoryStatus ? "ok" : "MISSING", o_CreateThread ? "ok" : "MISSING", o_timeSetEvent ? "ok" : "MISSING",
        o_LoadLibraryA ? "ok" : "MISSING");
    if (!o_CreateFileA) o_CreateFileA = CreateFileA;
    if (!id_GetFileAttributesA) id_GetFileAttributesA = GetFileAttributesA;
    if (!id_FindFirstFileA) id_FindFirstFileA = FindFirstFileA;
    if (!o_GlobalMemoryStatus) o_GlobalMemoryStatus = GlobalMemoryStatus;
    if (!o_CreateThread) o_CreateThread = CreateThread;
    if (!o_LoadLibraryA) o_LoadLibraryA = LoadLibraryA;
    fscache_setup(exe);
    tempbuf_setup(exe);
    spchmem_setup(exe);
    registry_setup(exe);
    mouse_setup(exe);
    wintext_setup(exe);
    alttab_setup(exe);
    pad_setup(exe);
    pad_ini_setup(exe);
    rumble_setup();

    g_keep_desktop = ini_int("KeepDesktopMode", 1);
    if (g_keep_desktop) {
        void *p = iat_hook(exe, "DDRAW.dll", "DirectDrawCreate", (void *)h_DirectDrawCreate);
        if (p) o_DirectDrawCreate = (DDCreate_fn)p;
        LOG("DirectDraw: keep desktop mode on (exe import %s; THRASH driver and CTRLS.DLL hooked when loaded)",
            p ? "hooked" : "not found");
    } else {
        LOG("DirectDraw: keep desktop mode off (KeepDesktopMode=0)");
    }

    o_TerminateThread = (TerminateThread_fn)iat_hook(exe, "KERNEL32.dll", "TerminateThread", (void *)h_TerminateThread);
    if (!o_TerminateThread) o_TerminateThread = TerminateThread;

    if (g_is_v11) {
        patch_cd_check();
        select_thrash_driver();
    }
    build_install_dat();
    setup_affinity();
    note_thread(GetCurrentThreadId());   /* main thread (we run in an APC on it) */
    SetCurrentDirectoryA(g_exe_dir);     /* the game opens install.dat and others relative to the cwd */
    clean_stale_nbatemp();               /* before any game code runs */
    timing_setup();
    start_thread_dump();
    {
        char ev[64];
        HANDLE h;
        _snprintf(ev, sizeof ev, "NBA99CORE_READY_%lu", GetCurrentProcessId());
        h = OpenEventA(EVENT_MODIFY_STATE, FALSE, ev);
        if (h) { SetEvent(h); CloseHandle(h); }
    }
    LOG("setup done");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE pinned;
        DisableThreadLibraryCalls(inst);
        /* never unload: our hooks live here */
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)(void *)DllMain, &pinned);
        init(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        fscache_report();
        tempbuf_report();
        spchmem_report();
    }
    return TRUE;
}

/* exported so the launcher (or a test) can confirm the DLL is the right one */
__declspec(dllexport) const char *nba99core_version(void) { return "nba99core " NBA99_VERSION_LINE; }
NBA99_C_END
