/*
 * Machine report (included by nba99core.cpp, both builds).
 *
 * Written to nba99core.log (when logging is on) and as a short header in
 * logs\timing.log, so every log someone sends says what it ran on:
 *   - Windows edition, version and build (or the Wine version)
 *   - PC maker and model, board, BIOS (from the firmware tables Windows keeps)
 *   - CPU name, logical CPUs, RAM
 *   - every graphics adapter: name, PCI ids, video memory, driver version
 *     (and the NVIDIA release number, e.g. 576.80)
 *   - every active display: resolution, refresh rate, colour depth, primary
 *   - versions of our files, sizes of the game's own DLLs, and the settings in nba99fix.ini
 * Runs on the timing thread, not in DllMain: it loads dxgi.dll, which must not
 * happen under the loader lock.
 */
#define COBJMACROS
#include <dxgi.h>

/* one-line summaries for crash reports and the timing log */
static char g_sysline[400] = "system: unknown";
static char g_machine[300] = "machine: unknown";
static char g_machine_pc[96] = "";   /* computer name and install drive type (0.7.10) */

static void reg_str(HKEY root, const char *key, const char *name, char *out, DWORD cap)
{
    HKEY k;
    DWORD type, n = cap;
    out[0] = 0;
    if (RegOpenKeyExA(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)out, &n) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        out[0] = 0;
    else
        out[cap - 1] = 0;
    RegCloseKey(k);
}

static DWORD reg_dword(HKEY root, const char *key, const char *name)
{
    HKEY k;
    DWORD type, v = 0, n = sizeof v;
    if (RegOpenKeyExA(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return 0;
    if (RegQueryValueExA(k, name, NULL, &type, (BYTE *)&v, &n) != ERROR_SUCCESS || type != REG_DWORD) v = 0;
    RegCloseKey(k);
    return v;
}

/* trim leading/trailing spaces in place (CPU names are padded) */
static char *trim(char *s)
{
    char *e;
    while (*s == ' ') s++;
    e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;
    return s;
}

static void file_version(const char *name, char *out, int cap)
{
    char path[MAX_PATH];
    DWORD h, n;
    void *buf, *val;
    UINT len;
    _snprintf(path, sizeof path, "%s%s", g_exe_dir, name);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { _snprintf(out, cap, "missing"); return; }
    n = GetFileVersionInfoSizeA(path, &h);
    if (!n || !(buf = malloc(n))) { _snprintf(out, cap, "no version"); return; }
    if (GetFileVersionInfoA(path, 0, n, buf) &&
        (VerQueryValueA(buf, "\\StringFileInfo\\0c0904b0\\FileVersion", &val, &len) ||
         VerQueryValueA(buf, "\\StringFileInfo\\040904b0\\FileVersion", &val, &len)) && len)
        _snprintf(out, cap, "%s", (char *)val);
    else {
        VS_FIXEDFILEINFO *fi;
        if (VerQueryValueA(buf, "\\", (void **)&fi, &len) && len)
            _snprintf(out, cap, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                      HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        else
            _snprintf(out, cap, "no version");
    }
    out[cap - 1] = 0;
    free(buf);
}

static const GUID k_IID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID k_IID_IDXGIDevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

/* graphics adapters through DXGI; the first hardware one goes into g_machine */
static void gpu_report(char *first, int cap)
{
    typedef HRESULT (WINAPI *CreateDXGIFactory1_fn)(REFIID, void **);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    CreateDXGIFactory1_fn create = dxgi ? (CreateDXGIFactory1_fn)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *f = NULL;
    UINT i;
    first[0] = 0;
    if (!create || FAILED(create(k_IID_IDXGIFactory1, (void **)&f)) || !f) {
        LOG("gpu: could not list graphics adapters (no DXGI 1.1)");
        return;
    }
    for (i = 0; i < 8; i++) {
        IDXGIAdapter1 *a = NULL;
        DXGI_ADAPTER_DESC1 d;
        LARGE_INTEGER umd;
        char name[128], drv[64] = "unknown", nv[24] = "";
        if (IDXGIFactory1_EnumAdapters1(f, i, &a) == DXGI_ERROR_NOT_FOUND) break;
        if (!a) break;
        memset(&d, 0, sizeof d);
        IDXGIAdapter1_GetDesc1(a, &d);
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, NULL, NULL);
        if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(a, k_IID_IDXGIDevice, &umd))) {
            unsigned p1 = HIWORD(umd.HighPart), p2 = LOWORD(umd.HighPart), p3 = HIWORD(umd.LowPart), p4 = LOWORD(umd.LowPart);
            _snprintf(drv, sizeof drv, "%u.%u.%u.%u", p1, p2, p3, p4);
            if (d.VendorId == 0x10DE) {   /* NVIDIA release: last digit of part 3 and part 4, e.g. 32.0.15.7680 -> 576.80 */
                unsigned r = (p3 % 10) * 10000 + p4;
                _snprintf(nv, sizeof nv, " (NVIDIA %u.%02u)", r / 100, r % 100);
            }
        }
        /* DXGI reports memory as SIZE_T, so a 32-bit program (like this game) sees at most about 4 GB */
        LOG("gpu %u: %s, vendor 0x%04x device 0x%04x rev %u, %llu MB video memory%s, %llu MB shared, driver %s%s%s", i, name,
            d.VendorId, d.DeviceId, d.Revision, (unsigned long long)(d.DedicatedVideoMemory >> 20),
            d.DedicatedVideoMemory >= ((SIZE_T)3 << 30) ? " (or more: 32-bit view)" : "",
            (unsigned long long)(d.SharedSystemMemory >> 20), drv, nv,
            (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : "");
        if (!first[0] && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414)
            _snprintf(first, cap, "%s, driver %s%s", name, drv, nv);
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}

/* active displays; the primary one's mode goes into 'primary' */
static void display_report(char *primary, int cap)
{
    DISPLAY_DEVICEA dd;
    DWORD i;
    primary[0] = 0;
    for (i = 0; i < 16; i++) {
        DISPLAY_DEVICEA mon;
        DEVMODEA dm;
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        memset(&mon, 0, sizeof mon);
        mon.cb = sizeof mon;
        if (!EnumDisplayDevicesA(dd.DeviceName, 0, &mon, 0)) mon.DeviceString[0] = 0;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
        LOG("display: %s %s, %lux%lu %lu Hz %lu-bit at %ld,%ld%s (on %s)", dd.DeviceName,
            mon.DeviceString[0] ? mon.DeviceString : "(monitor unknown)", dm.dmPelsWidth, dm.dmPelsHeight,
            dm.dmDisplayFrequency, dm.dmBitsPerPel, dm.dmPosition.x, dm.dmPosition.y,
            (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? ", primary" : "", dd.DeviceString);
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
            _snprintf(primary, cap, "%lux%lu %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    }
}

static void sys_report(void)
{
    typedef LONG (WINAPI *RtlGetVersion_fn)(OSVERSIONINFOW *);
    typedef const char *(__cdecl *wine_get_version_fn)(void);
    static const char *cv = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    static const char *bios = "HARDWARE\\DESCRIPTION\\System\\BIOS";
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    RtlGetVersion_fn rgv = (RtlGetVersion_fn)GetProcAddress(nt, "RtlGetVersion");
    wine_get_version_fn wgv = (wine_get_version_fn)GetProcAddress(nt, "wine_get_version");
    OSVERSIONINFOW ov;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    char os[200], prod[96], disp[32], v[128], line[600], maker[96], model[96], board[96], biosv[96], cpu[128], gpu[200], mon[64];
    static const char *files[] = { "NBALive99.exe", "nba99core.dll", "glide2x.dll", "3rash\\voodoologa.dll" };
    static const char *game_files[] = { "NBAWIN.EXE", "EACSND.DLL", "CTRLS.DLL", "FONTRES.DLL", "3rash\\VOODOOA.DLL" };
    int i;
    FILE *ini;

    memset(&ov, 0, sizeof ov);
    ov.dwOSVersionInfoSize = sizeof ov;
    if (rgv) rgv(&ov);
    reg_str(HKEY_LOCAL_MACHINE, cv, "ProductName", prod, sizeof prod);
    reg_str(HKEY_LOCAL_MACHINE, cv, "DisplayVersion", disp, sizeof disp);
    /* Windows 11 still says "Windows 10" in ProductName; the build number tells them apart */
    if (ov.dwBuildNumber >= 22000 && !strncmp(prod, "Windows 10", 10)) { prod[8] = '1'; prod[9] = '1'; }
    if (wgv)
        _snprintf(os, sizeof os, "Wine %s (reports Windows %lu.%lu.%lu)", wgv(), ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber);
    else
        _snprintf(os, sizeof os, "%s %s, build %lu.%lu", prod[0] ? prod : "Windows", disp, ov.dwBuildNumber,
                  reg_dword(HKEY_LOCAL_MACHINE, cv, "UBR"));
    LOG("os: %s", os);

    reg_str(HKEY_LOCAL_MACHINE, bios, "SystemManufacturer", maker, sizeof maker);
    reg_str(HKEY_LOCAL_MACHINE, bios, "SystemProductName", model, sizeof model);
    reg_str(HKEY_LOCAL_MACHINE, bios, "BaseBoardProduct", board, sizeof board);
    reg_str(HKEY_LOCAL_MACHINE, bios, "BIOSVersion", biosv, sizeof biosv);
    LOG("pc: %s %s, board %s, BIOS %s", maker[0] ? maker : "(maker unknown)", model[0] ? model : "(model unknown)",
        board[0] ? board : "unknown", biosv[0] ? biosv : "unknown");

    reg_str(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", cpu, sizeof cpu);
    GetNativeSystemInfo(&si);
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    LOG("cpu: %s, %lu logical CPUs, %llu MB RAM", cpu[0] ? trim(cpu) : "unknown", si.dwNumberOfProcessors,
        (unsigned long long)(ms.ullTotalPhys >> 20));

    gpu_report(gpu, sizeof gpu);
    display_report(mon, sizeof mon);

    {   /* which PC, on mains or battery, and where the game is installed (0.7.10) */
        char name[64] = "unknown", root[8], ses[128];
        DWORD n = sizeof name;
        SYSTEM_POWER_STATUS ps;
        UINT dt;
        static const char *const k_dt[] = {"unknown", "no root", "removable", "local", "network", "CD/DVD", "RAM disk"};
        GetComputerNameA(name, &n);
        nba99_log_session(ses, sizeof ses);
        LOG("computer: %s (log folder logs\\%s)", name, ses);
        if (GetSystemPowerStatus(&ps)) {
            if (ps.BatteryFlag == 128 || ps.BatteryFlag == 255)
                LOG("power: mains (no battery)");
            else
                LOG("power: %s, battery %d%%%s", ps.ACLineStatus == 1 ? "mains" : ps.ACLineStatus == 0 ? "on battery" : "unknown",
                    ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : -1, ps.SystemStatusFlag & 1 ? ", battery saver on" : "");
        }
        _snprintf(root, sizeof root, "%.3s", g_exe_dir);
        dt = GetDriveTypeA(root);
        LOG("install: %s (%s drive)", g_exe_dir, dt < 7 ? k_dt[dt] : "unknown");
        _snprintf(g_machine_pc, sizeof g_machine_pc, "%s, %s drive", name, dt < 7 ? k_dt[dt] : "unknown");
    }

    _snprintf(g_sysline, sizeof g_sysline, "%s, %s, %lu CPUs, %llu MB RAM, %s, desktop %s", os,
              cpu[0] ? trim(cpu) : "CPU unknown", si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20),
              gpu[0] ? gpu : "GPU unknown", mon[0] ? mon : "unknown");
    _snprintf(g_machine, sizeof g_machine, "%s | %s%s%s | %s | %s | desktop %s | %s", g_machine_pc, maker[0] ? maker : "",
              maker[0] ? " " : "", model[0] ? model : "PC", cpu[0] ? trim(cpu) : "CPU unknown", gpu[0] ? gpu : "GPU unknown",
              mon[0] ? mon : "unknown", os);

    for (i = 0; i < 4; i++) {
        file_version(files[i], v, sizeof v);
        LOG("file: %-20s %s", files[i], v);
    }
    for (i = 0; i < 5; i++) {   /* the game's own files: size only (the exe is checked in full by the launcher) */
        WIN32_FILE_ATTRIBUTE_DATA fa;
        _snprintf(line, sizeof line, "%s%s", g_exe_dir, game_files[i]);
        if (GetFileAttributesExA(line, GetFileExInfoStandard, &fa))
            LOG("game file: %-20s %lu bytes", game_files[i], (unsigned long)fa.nFileSizeLow);
        else
            LOG("game file: %-20s missing", game_files[i]);
    }
    _snprintf(line, sizeof line, "%snba99fix.ini", g_exe_dir);
    ini = fopen(line, "r");
    if (!ini) { LOG("settings: no nba99fix.ini (all defaults)"); return; }
    while (fgets(line, sizeof line, ini)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (line[0] && line[0] != ';') LOG("settings: %s", line);
    }
    fclose(ini);
}
