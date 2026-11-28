/*
 * Gamepads for NBA Live 99 through SDL3 (included by nba99core.cpp; 0.4.0).
 *
 * How the game reads controllers (v1.1 exe, static analysis 26 Nov 2026):
 *  - EA's input library calls WINMM: joyGetNumDevs once at start (0x585360,
 *    from UserInput_Initialize; the count is kept at 0x6EB420 and later ids at
 *    or above it are refused), joyGetDevCapsA for the device info (0x5853B0)
 *    and joyGetPosEx for the state (0x585630).
 *  - ControllerUpdater (0x4EDC90), the game's input thread, keeps 4 device
 *    records (0x308 bytes at 0x683620) and re-reads the device info of all four
 *    every 1,000 ms, so the game has its own hot-plug: on a change it reloads
 *    that slot's config (UserInput_LoadConfig) and tells the front end.
 *  - The device name is the registry's Windows 9x DirectInput name
 *    (MediaResources\Joystick\DINPUT.DLL\CurrentJoystickSettings\
 *    Joystick<n>OEMName), else JOYCAPS szPname. The config is looked up as
 *    "<name>_<buttons>" (devcfgs.ini, then inpdevs.ini; see pad_ini.inc).
 *  - Each poll keeps X, Y and the 32 button bits only: Z, R, U, V and the POV
 *    are read and thrown away. Directions are digital with fixed thresholds:
 *    X below 0x6000 = left, above 0xA000 = right; Y the same for up and down.
 *  - 4 local controller slots: slots 1 and 2 are pads only; slots 3 and 4 are
 *    pad 3 / pad 4 when plugged in, otherwise the two keyboard sets. (The
 *    game's "8 users" are 4 per PC in an online game.) Decided
 *    26 Nov 2026: keep the 4.
 *
 * We answer those three WINMM calls from SDL3 gamepads (98's pad_input.inc
 * method), so any pad SDL knows works:
 *  - 4 devices reported from the start; pads go into free slots as they are
 *    plugged in. A replugged pad goes back to the slot the same model last had
 *    when that slot is free (so a pad unplugged in a match keeps its player).
 *  - Each pad reports a name of ours by its SDL3 type through szPname (the
 *    registry name is hidden from the game while gamepads are on, see
 *    pad_name_key), so the game picks our inpdevs.ini section for it and shows
 *    that pad family's own button names:
 *      Xbox pads           "Xbox controller"
 *      PlayStation pads    "PlayStation controller"
 *      Nintendo pads       "Switch controller" (Pro Controller, Joy-Cons)
 *      anything else       "Gamepad"
 *  - 12 buttons in the Windows XInput joystick order (98's order), by button
 *    position, so the same number means the same place on every pad:
 *      1 A/Cross  2 B/Circle  3 X/Square  4 Y/Triangle  5 LB/L1  6 RB/R1
 *      7 Back/Select  8 Start  9 L3  10 R3  11 LT/L2  12 RT/R2
 *    Triggers are buttons (pressed past about a third).
 *  - X/Y 0 to 65535 from the left stick and the D-pad (the D-pad wins).
 *    [input] Stick=auto (default; decided 28 Nov 2026) follows the game's own
 *    Analog Mode for that slot: off = clean 45 degree sectors (diagonals behave
 *    like a D-pad), on = the stick as it is, so the game can steer between the
 *    8 directions. Stick=8way or Stick=raw forces one.
 *
 * SDL3.dll is the one our eacsnd.dll already loads (run time, no import). We
 * add the gamepad subsystem only and never quit anything (the sound DLL uses
 * the audio subsystem).
 */

typedef uint32_t SDL_JoystickID;
typedef struct SDL_Gamepad SDL_Gamepad;
#define SDL_INIT_GAMEPAD 0x00002000u

/* SDL_GamepadType values (SDL 3.2) */
enum { PADT_XBOX360 = 2, PADT_XBOXONE = 3, PADT_PS3 = 4, PADT_PS4 = 5, PADT_PS5 = 6,
       PADT_SWITCH_PRO = 7, PADT_JOYCON_LEFT = 8, PADT_JOYCON_RIGHT = 9, PADT_JOYCON_PAIR = 10 };
/* pad families (26 Nov 2026: a Nintendo section of its own, "Gamepad" for the rest) */
enum { PADF_XBOX, PADF_PS, PADF_SWITCH, PADF_GENERIC };
static const char *const g_pad_family_name[4] = { "Xbox controller", "PlayStation controller", "Switch controller",
                                                  "Gamepad" };
#define PAD_BUTTONS 12

static struct {
    HMODULE dll;
    bool (__cdecl *Init)(uint32_t);
    bool (__cdecl *SetHint)(const char *, const char *);
    const char *(__cdecl *GetError)(void);
    SDL_JoystickID *(__cdecl *GetGamepads)(int *);
    SDL_Gamepad *(__cdecl *OpenGamepad)(SDL_JoystickID);
    void (__cdecl *CloseGamepad)(SDL_Gamepad *);
    bool (__cdecl *GamepadConnected)(SDL_Gamepad *);
    bool (__cdecl *GetGamepadButton)(SDL_Gamepad *, int);
    int16_t (__cdecl *GetGamepadAxis)(SDL_Gamepad *, int);
    void (__cdecl *UpdateGamepads)(void);
    const char *(__cdecl *GetGamepadName)(SDL_Gamepad *);
    int (__cdecl *GetGamepadType)(SDL_Gamepad *);
    uint16_t (__cdecl *GetGamepadVendor)(SDL_Gamepad *);
    uint16_t (__cdecl *GetGamepadProduct)(SDL_Gamepad *);
    void (__cdecl *free)(void *);
    void (__cdecl *FlushEvents)(uint32_t, uint32_t);
} psdl;

#if NBA99_DEBUG
/* Test pad (developer only, for runs without a real pad): NBA99_TEST_PAD=<file>
 * attaches an SDL virtual gamepad (reported as an Xbox pad) whose state is read
 * from that file whenever it changes: "<SDL button mask hex> <left x> <left y>"
 * (SDL button numbers: 0 South, 1 East, 2 West, 3 North, 4 Back, 6 Start,
 * 7 L3, 8 R3, 9 L1, 10 R1, 11 to 14 D-pad up/down/left/right; sticks -32768 to
 * 32767). */
typedef struct {
    uint32_t version; uint16_t type, padding, vendor_id, product_id, naxes, nbuttons, nballs, nhats,
        ntouchpads, nsensors, padding2[2]; uint32_t button_mask, axis_mask; const char *name;
    const void *touchpads, *sensors; void *userdata; void *cb[8];
} VJoyDesc;
static struct {
    SDL_JoystickID (__cdecl *Attach)(const VJoyDesc *);
    void *(__cdecl *OpenJoystick)(SDL_JoystickID);
    bool (__cdecl *SetButton)(void *, int, bool);
    bool (__cdecl *SetAxis)(void *, int, int16_t);
    void *joy;
    char file[MAX_PATH];
    FILETIME ft;
} vpad;

static void vpad_init(void)
{
    VJoyDesc d;
    SDL_JoystickID id;
    if (!GetEnvironmentVariableA("NBA99_TEST_PAD", vpad.file, sizeof vpad.file)) return;
    *(FARPROC *)&vpad.Attach = GetProcAddress(psdl.dll, "SDL_AttachVirtualJoystick");
    *(FARPROC *)&vpad.OpenJoystick = GetProcAddress(psdl.dll, "SDL_OpenJoystick");
    *(FARPROC *)&vpad.SetButton = GetProcAddress(psdl.dll, "SDL_SetJoystickVirtualButton");
    *(FARPROC *)&vpad.SetAxis = GetProcAddress(psdl.dll, "SDL_SetJoystickVirtualAxis");
    if (!vpad.Attach || !vpad.OpenJoystick || !vpad.SetButton || !vpad.SetAxis) return;
    memset(&d, 0, sizeof d);
    d.version = sizeof d;
    d.type = 1;               /* gamepad */
    d.vendor_id = 0x045E;     /* Microsoft, so SDL types it as an Xbox pad */
    d.product_id = 0x02EA;
    d.naxes = 6;
    d.nbuttons = 26;
    d.name = "Test Pad";
    id = vpad.Attach(&d);
    vpad.joy = id ? vpad.OpenJoystick(id) : NULL;
    LOG("gamepads: test pad %s (state from %s)", vpad.joy ? "attached" : "FAILED", vpad.file);
}

static void vpad_poll(void)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    char buf[64];
    DWORD got = 0;
    HANDLE f;
    unsigned long mask;
    long x, y;
    char *p;
    int i;
    if (!vpad.joy || !GetFileAttributesExA(vpad.file, GetFileExInfoStandard, &a)) return;
    if (!CompareFileTime(&a.ftLastWriteTime, &vpad.ft)) return;
    vpad.ft = a.ftLastWriteTime;
    f = CreateFileA(vpad.file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    /* strtoul/strtol by hand: msvcrt's sscanf pulls in imports Windows 98 lacks */
    mask = strtoul(buf, &p, 16);
    if (p == buf) return;
    x = strtol(p, &p, 10);
    y = strtol(p, &p, 10);
    for (i = 0; i < 26; i++) vpad.SetButton(vpad.joy, i, (mask >> i) & 1);
    vpad.SetAxis(vpad.joy, 0, (int16_t)x);
    vpad.SetAxis(vpad.joy, 1, (int16_t)y);
    LOG("gamepads: test pad state 0x%lx %ld %ld", mask, x, y);
}
#endif

#define PAD_SLOTS 4
static struct {
    SDL_JoystickID id;
    SDL_Gamepad *gp;
    int family;
    uint32_t model;          /* vendor << 16 | product of the pad in (or last in) this slot */
} g_pad[PAD_SLOTS];
static int g_pad_ready, g_pad_failed, g_pad_deadzone, g_pad_first_call = 1;
enum { STICK_AUTO, STICK_8WAY, STICK_RAW };
static int g_pad_stick;

/* The game's own "Analog Mode" for a slot (Configure Controller; analogmode in devcfgs.ini): the int16 option 4
   of the slot record (UserInput_GetOptionValue 0x4EF0E9: 0x68343C + 0x74 per slot; -1 not set, 0 off, 1 on).
   With it on, tst_controller_value (0x4EE250) turns how far X/Y are past 0x6000/0xA000 into strengths 0 to 15
   and convertjoydir (0x4343B0) steers between the 8 directions with them; with it off only the 8 directions are
   used. Read only. */
static int pad_slot_analog(int slot)
{
    return g_is_v11 && slot >= 0 && slot < PAD_SLOTS && *(volatile int16_t *)(uintptr_t)(0x68343C + 0x74 * slot) > 0;
}
static CRITICAL_SECTION g_pad_cs;
static DWORD pad_menu_buttons(int slot, DWORD b);   /* pad_ini.inc */
static void rumble_tick(void);                        /* rumble.inc */
static void rumble_sdl_ready(void);
static DWORD g_pad_last_update;

typedef UINT (WINAPI *joyGetNumDevs_fn)(void);
typedef MMRESULT (WINAPI *joyGetDevCapsA_fn)(UINT_PTR, LPJOYCAPSA, UINT);
typedef MMRESULT (WINAPI *joyGetPosEx_fn)(UINT, LPJOYINFOEX);
static joyGetNumDevs_fn o_joyGetNumDevs;
static joyGetDevCapsA_fn o_joyGetDevCapsA;
static joyGetPosEx_fn o_joyGetPosEx;

static int pad_load(void)
{
    char path[MAX_PATH];
    if (g_pad_ready) return 1;
    if (g_pad_failed) return 0;
    g_pad_failed = 1;
    _snprintf(path, sizeof path, "%sSDL3.dll", g_exe_dir);
    psdl.dll = LoadLibraryA(path);
    if (!psdl.dll) { LOG("gamepads: SDL3.dll not found next to the game"); return 0; }
#define G(n) *(FARPROC *)&psdl.n = GetProcAddress(psdl.dll, "SDL_" #n); if (!psdl.n) { LOG("gamepads: SDL3.dll has no SDL_" #n); return 0; }
    G(Init) G(SetHint) G(GetError) G(GetGamepads) G(OpenGamepad) G(CloseGamepad) G(GamepadConnected)
    G(GetGamepadButton) G(GetGamepadAxis) G(UpdateGamepads) G(GetGamepadName) G(GetGamepadType)
    G(GetGamepadVendor) G(GetGamepadProduct) G(free) G(FlushEvents)
#undef G
    psdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    if (!psdl.Init(SDL_INIT_GAMEPAD)) { LOG("gamepads: SDL gamepad init failed: %s", psdl.GetError()); return 0; }
    g_pad_failed = 0;
    g_pad_ready = 1;
    rumble_sdl_ready();
#if NBA99_DEBUG
    vpad_init();
#endif
    LOG("gamepads: SDL3 gamepad support on (%d controller slots)", PAD_SLOTS);
    return 1;
}

static int pad_family(SDL_Gamepad *gp)
{
    int t = psdl.GetGamepadType(gp);
    if (t == PADT_XBOX360 || t == PADT_XBOXONE) return PADF_XBOX;
    if (t == PADT_PS3 || t == PADT_PS4 || t == PADT_PS5) return PADF_PS;
    if (t >= PADT_SWITCH_PRO && t <= PADT_JOYCON_PAIR) return PADF_SWITCH;
    return PADF_GENERIC;
}

/* open new gamepads into free slots, drop disconnected ones (slot keeps its model) */
static void pad_refresh(void)
{
    DWORD now = GetTickCount();
    int i, j, n = 0;
    SDL_JoystickID *ids;
    if (now - g_pad_last_update < 2) return;
    g_pad_last_update = now;
#if NBA99_DEBUG
    vpad_poll();
#endif
    psdl.UpdateGamepads();
    psdl.FlushEvents(0x600, 0x6FF);   /* joystick and gamepad events: nobody reads them */
    rumble_tick();
    for (i = 0; i < PAD_SLOTS; i++)
        if (g_pad[i].gp && !psdl.GamepadConnected(g_pad[i].gp)) {
            LOG("gamepads: pad in slot %d disconnected", i + 1);
            psdl.CloseGamepad(g_pad[i].gp);
            g_pad[i].gp = NULL;
            g_pad[i].id = 0;
        }
    ids = psdl.GetGamepads(&n);
    for (j = 0; ids && j < n; j++) {
        int have = 0, slot = -1;
        SDL_Gamepad *gp;
        uint32_t model;
        for (i = 0; i < PAD_SLOTS; i++) if (g_pad[i].gp && g_pad[i].id == ids[j]) have = 1;
        if (have) continue;
        gp = psdl.OpenGamepad(ids[j]);
        if (!gp) continue;
        model = ((uint32_t)psdl.GetGamepadVendor(gp) << 16) | psdl.GetGamepadProduct(gp);
        for (i = 0; i < PAD_SLOTS && slot < 0; i++) if (!g_pad[i].gp && g_pad[i].model == model) slot = i;
        for (i = 0; i < PAD_SLOTS && slot < 0; i++) if (!g_pad[i].gp && !g_pad[i].model) slot = i;
        for (i = 0; i < PAD_SLOTS && slot < 0; i++) if (!g_pad[i].gp) slot = i;
        if (slot < 0) { psdl.CloseGamepad(gp); continue; }   /* more than 4 pads: the rest wait */
        g_pad[slot].gp = gp;
        g_pad[slot].id = ids[j];
        g_pad[slot].model = model;
        g_pad[slot].family = pad_family(gp);
        LOG("gamepads: \"%s\" (%04lx:%04lx, type %d) in slot %d as \"%s\"",
            psdl.GetGamepadName(gp) ? psdl.GetGamepadName(gp) : "?", (unsigned long)(model >> 16),
            (unsigned long)(model & 0xFFFF), psdl.GetGamepadType(gp), slot + 1, g_pad_family_name[g_pad[slot].family]);
    }
    if (ids) psdl.free(ids);
}

/* The game counts joysticks once, in UserInput_Initialize (main thread, after
   our DllMain), so SDL starts here. */
static UINT WINAPI h_joyGetNumDevs(void)
{
    if (!g_pad_on) return o_joyGetNumDevs();
    EnterCriticalSection(&g_pad_cs);
    if (g_pad_first_call) {
        g_pad_first_call = 0;
        if (pad_load()) {
            DWORD t0 = GetTickCount(), seen = 0;
            int i, n;
            /* give SDL a moment to report pads that are already plugged in */
            for (;;) {
                g_pad_last_update = GetTickCount() - 10;
                pad_refresh();
                for (n = 0, i = 0; i < PAD_SLOTS; i++) n += g_pad[i].gp != NULL;
                if (n && !seen) seen = GetTickCount();
                if (seen ? GetTickCount() - seen > 100 : GetTickCount() - t0 > 500) break;
                Sleep(10);
            }
            LOG("gamepads: %d connected at start-up", n);
        }
    }
    LeaveCriticalSection(&g_pad_cs);
    return PAD_SLOTS;
}

static MMRESULT WINAPI h_joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA caps, UINT size)
{
    MMRESULT r = JOYERR_UNPLUGGED;
    if (!g_pad_on) return o_joyGetDevCapsA(id, caps, size);
    EnterCriticalSection(&g_pad_cs);
    if (pad_load() && id < PAD_SLOTS) {
        pad_refresh();
        if (g_pad[id].gp && caps && size >= sizeof(JOYCAPSA)) {
            memset(caps, 0, size);
            caps->wMid = 0xFFFF;
            caps->wPid = 0xFFFF;
            lstrcpynA(caps->szPname, g_pad_family_name[g_pad[id].family], sizeof caps->szPname);
            caps->wXmax = caps->wYmax = 65535;
            caps->wNumButtons = PAD_BUTTONS;
            caps->wPeriodMin = 1;
            caps->wPeriodMax = 1000;
            caps->wMaxAxes = caps->wNumAxes = 2;
            caps->wMaxButtons = PAD_BUTTONS;
            r = JOYERR_NOERROR;
        }
    }
    LeaveCriticalSection(&g_pad_cs);
    return r;
}

static DWORD axis_to_winmm(int16_t v)
{
    int a = v;
    if (a > -g_pad_deadzone && a < g_pad_deadzone) a = 0;
    return (DWORD)((a + 32768) & 0xFFFF);
}

/* left stick -> one of 8 directions (45 degree sectors), centre inside the
   dead zone; tan(22.5 degrees) is about 53/128 */
static void stick_8way(int16_t sx, int16_t sy, DWORD *x, DWORD *y)
{
    int a = sx, b = sy, aa = a < 0 ? -a : a, ab = b < 0 ? -b : b;
    *x = *y = 32767;
    if ((long long)a * a + (long long)b * b < (long long)g_pad_deadzone * g_pad_deadzone) return;
    if (ab * 128 > aa * 53) *y = b < 0 ? 0 : 65535;
    if (aa * 128 > ab * 53) *x = a < 0 ? 0 : 65535;
}

/* the 12 buttons of a slot's pad as WINMM button bits (0 if no pad) */
static DWORD pad_buttons(SDL_Gamepad *gp)
{
    static const int order[10] = { 0 /*South*/, 1 /*East*/, 2 /*West*/, 3 /*North*/, 9 /*L1*/, 10 /*R1*/,
                                   4 /*Back*/, 6 /*Start*/, 7 /*L3*/, 8 /*R3*/ };
    DWORD b = 0;
    int i;
    for (i = 0; i < 10; i++) if (psdl.GetGamepadButton(gp, order[i])) b |= 1u << i;
    if (psdl.GetGamepadAxis(gp, 4) > 12000) b |= 1u << 10;   /* L2 trigger */
    if (psdl.GetGamepadAxis(gp, 5) > 12000) b |= 1u << 11;   /* R2 trigger */
    return b;
}

static MMRESULT WINAPI h_joyGetPosEx(UINT id, LPJOYINFOEX ji)
{
    MMRESULT r = JOYERR_UNPLUGGED;
    if (!g_pad_on) return o_joyGetPosEx(id, ji);
    EnterCriticalSection(&g_pad_cs);
    if (pad_load() && id < PAD_SLOTS && ji) {
        SDL_Gamepad *gp;
        pad_refresh();
        gp = g_pad[id].gp;
        if (gp) {
            DWORD b = pad_menu_buttons((int)id, pad_buttons(gp)), x, y, size = ji->dwSize;
            int i;
            if (g_pad_stick == STICK_8WAY || (g_pad_stick == STICK_AUTO && !pad_slot_analog((int)id)))
                stick_8way(psdl.GetGamepadAxis(gp, 0), psdl.GetGamepadAxis(gp, 1), &x, &y);
            else { x = axis_to_winmm(psdl.GetGamepadAxis(gp, 0)); y = axis_to_winmm(psdl.GetGamepadAxis(gp, 1)); }
            if (psdl.GetGamepadButton(gp, 13)) x = 0;          /* D-pad left */
            if (psdl.GetGamepadButton(gp, 14)) x = 65535;      /* D-pad right */
            if (psdl.GetGamepadButton(gp, 11)) y = 0;          /* D-pad up */
            if (psdl.GetGamepadButton(gp, 12)) y = 65535;      /* D-pad down */
            memset(ji, 0, size >= sizeof *ji ? sizeof *ji : size);
            ji->dwSize = size;
            ji->dwXpos = x;
            ji->dwYpos = y;
            ji->dwZpos = ji->dwRpos = ji->dwUpos = ji->dwVpos = 32767;
            ji->dwButtons = b;
            for (i = 0; i < 32; i++) if (b & (1u << i)) ji->dwButtonNumber++;
            ji->dwPOV = JOY_POVCENTERED;
            r = JOYERR_NOERROR;
        }
    }
    LeaveCriticalSection(&g_pad_cs);
    return r;
}

/* While gamepads are on, the Windows 9x joystick name keys are "not found" for
   the game, so it always takes our names from szPname (a stale DirectInput
   name left in the registry would otherwise pick another inpdevs.ini section).
   Called by the registry hook; the game asks once a second, so logged once. */
static int pad_name_key(const char *sub)
{
    static LONG logged;
    if (!g_pad_on || !sub) return 0;
    if (!strstr_i(sub, "MediaResources\\Joystick") && !strstr_i(sub, "PrivateProperties\\Joystick") &&
        !(!_strnicmp(sub, "Joystick", 8) && strstr_i(sub, "OEMName")))   /* the value read, Joystick<n>OEMName */
        return 0;
    if (!InterlockedExchange(&logged, 1))
        LOG("gamepads: the game's joystick name lookup in the registry (%s) answered \"not found\" (logged once)", sub);
    return 1;
}

static void pad_reload_live(void)
{
    char v[16];
    int dz = (int)GetPrivateProfileIntA("input", "Deadzone", 20, g_ini);
    if (dz < 0) dz = 0;
    if (dz > 90) dz = 90;
    g_pad_deadzone = 32768 * dz / 100;
    GetPrivateProfileStringA("input", "Stick", "auto", v, sizeof v, g_ini);
    g_pad_stick = !lstrcmpiA(v, "raw") ? STICK_RAW : !lstrcmpiA(v, "8way") ? STICK_8WAY : STICK_AUTO;
    LOG("gamepads: dead zone %d%%, stick %s", dz,
        g_pad_stick == STICK_RAW ? "raw" : g_pad_stick == STICK_8WAY ? "8-way" : "auto (raw with the game's Analog Mode on, else 8-way)");
}

static void pad_setup(HMODULE exe)
{
    g_pad_on = (int)GetPrivateProfileIntA("input", "Gamepads", 1, g_ini);
    if (!g_pad_on) { LOG("gamepads: off (Windows joysticks used directly)"); return; }
    pad_reload_live();
    InitializeCriticalSection(&g_pad_cs);
    o_joyGetNumDevs = (joyGetNumDevs_fn)iat_hook(exe, "WINMM.dll", "joyGetNumDevs", (void *)h_joyGetNumDevs);
    o_joyGetDevCapsA = (joyGetDevCapsA_fn)iat_hook(exe, "WINMM.dll", "joyGetDevCapsA", (void *)h_joyGetDevCapsA);
    o_joyGetPosEx = (joyGetPosEx_fn)iat_hook(exe, "WINMM.dll", "joyGetPosEx", (void *)h_joyGetPosEx);
    LOG("gamepads: hooks joyGetNumDevs %s, joyGetDevCapsA %s, joyGetPosEx %s", o_joyGetNumDevs ? "ok" : "MISSING",
        o_joyGetDevCapsA ? "ok" : "MISSING", o_joyGetPosEx ? "ok" : "MISSING");
    if (!o_joyGetNumDevs) o_joyGetNumDevs = joyGetNumDevs;
    if (!o_joyGetDevCapsA) o_joyGetDevCapsA = joyGetDevCapsA;
    if (!o_joyGetPosEx) o_joyGetPosEx = joyGetPosEx;
}
