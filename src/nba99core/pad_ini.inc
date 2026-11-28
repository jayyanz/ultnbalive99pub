/*
 * Our pad sections in a served inpdevs.ini, and the front-end accept/back
 * buttons (included by nba99core.cpp after pad_input.inc; 0.4.0).
 *
 * UserInput_LoadConfig (0x4EFFA0) reads a pad's settings with
 * GetPrivateProfileIntA/StringA, first from .\devcfgs.ini (the user's own
 * changes, "<name>_<buttons>_Slot<n>"), then from .\inpdevs.ini
 * ("<name>_<buttons>_Slot<n>", "<name>_<buttons>", then DefaultNN by button
 * count), then a built-in numbered default. GetPrivateProfile* opens files
 * itself, so the exe's two imports are hooked: any read of inpdevs.ini goes to
 * our copy, %TEMP%\NBALive99\inpdevs.ini = the user's own inpdevs.ini with our
 * sections appended. EA's file is never changed or shipped; the text we append
 * is ours. A section of the same name in the user's file comes first in the
 * copy, so it wins over ours.
 *
 * Our sections (one per pad family, pad_input.inc) name the 12 buttons as that
 * family labels them and hold our default layout, by button position:
 * the NBA Live 99 PlayStation layout with R1/R2 and L1/L2 swapped, so turbo is
 * on the right trigger (decided 26 Nov 2026). Button numbers (pad_input.inc):
 *   1 A/Cross  2 B/Circle  3 X/Square  4 Y/Triangle  5 LB/L1  6 RB/R1
 *   7 Back/Select  8 Start  9 L3  10 R3  11 LT/L2  12 RT/R2
 * Functions the PS1 manual does not list (Direct Shoot = dunk, Fake = drive,
 * Play Calling) were placed with the PS1 buttons nearest in use, then swapped
 * the same way.
 *
 * Front end: the game turns a slot's "shoot" binding into Enter and its "pass"
 * binding into Esc in the menus (ControllerUpdater, 0x4EDF2B). With this layout
 * that would be B/Circle = accept and A/Cross = back, so in the menus only,
 * A/Cross presses whatever shoot is bound to and B/Circle whatever pass is
 * bound to (decided 26 Nov 2026). Not while the Configure Controller screen
 * waits for a button, and not in a match.
 */

static const char *const g_pad_btn_names[4][PAD_BUTTONS] = {
    { "A", "B", "X", "Y", "LB", "RB", "View", "Menu", "LS", "RS", "LT", "RT" },
    { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "Select/Share/Create", "Start/Options", "L3", "R3",
      "L2", "R2" },
    { "B", "A", "Y", "X", "L", "R", "Minus", "Plus", "LS", "RS", "ZL", "ZR" },
    { "South", "East", "West", "North", "LB", "RB", "Back", "Start", "LS", "RS", "LT", "RT" },
};

/* our default layout: function, button number */
static const struct { const char *fn; int button; } g_pad_layout[] = {
    { "turbo", 12 }, { "playcalling", 7 }, { "direct_pass_switch", 11 }, { "pause", 8 },
    { "shoot", 2 }, { "pass", 1 }, { "pivot", 4 }, { "dunk", 5 }, { "drive", 6 }, { "crossover", 3 },
    { "steal", 3 }, { "jump", 4 }, { "switch", 1 }, { "handcheck", 2 }, { "lastmanback", 5 }, { "faceup", 6 },
};

static char g_inpdevs_served[MAX_PATH];

typedef UINT (WINAPI *GetPrivateProfileIntA_fn)(LPCSTR, LPCSTR, INT, LPCSTR);
typedef DWORD (WINAPI *GetPrivateProfileStringA_fn)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
static GetPrivateProfileIntA_fn o_GetPrivateProfileIntA;
static GetPrivateProfileStringA_fn o_GetPrivateProfileStringA;

static int is_inpdevs(LPCSTR file)
{
    const char *b;
    if (!file || !g_inpdevs_served[0]) return 0;
    b = strrchr(file, '\\');
    b = b ? b + 1 : file;
    return !_stricmp(b, "inpdevs.ini");
}

#if NBA99_DEBUG
static int is_devcfgs(LPCSTR file)
{
    const char *b;
    if (!file) return 0;
    b = strrchr(file, '\\');
    b = b ? b + 1 : file;
    return !_stricmp(b, "devcfgs.ini");
}
#endif

static UINT WINAPI h_GetPrivateProfileIntA(LPCSTR sec, LPCSTR key, INT def, LPCSTR file)
{
    int ours = is_inpdevs(file);
    UINT r = o_GetPrivateProfileIntA(sec, key, def, ours ? g_inpdevs_served : file);
#if NBA99_DEBUG
    /* each lookup of a section starts with numbuttons: the log shows which section a slot got */
    if (key && sec && !_stricmp(key, "numbuttons") && (ours || is_devcfgs(file)))
        LOG("controller config: [%s] in %s: %s", sec, ours ? "inpdevs.ini (served)" : "devcfgs.ini",
            (int)r > 0 ? "found" : "not there");
#endif
    return r;
}

static DWORD WINAPI h_GetPrivateProfileStringA(LPCSTR sec, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    return o_GetPrivateProfileStringA(sec, key, def, out, size, is_inpdevs(file) ? g_inpdevs_served : file);
}

static int append(char *buf, int n, int cap, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (n >= cap) return n;
    va_start(ap, fmt);
    k = _vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    return k < 0 ? cap : n + k;
}

/* write %TEMP%\NBALive99\inpdevs.ini: the user's file, then our sections */
static void pad_serve_inpdevs(void)
{
    char src[MAX_PATH], dir[MAX_PATH], extra[6144];
    HANDLE in, out;
    DWORD got, w, user_bytes = 0;
    char chunk[4096];
    int f, i, n = 0;
    _snprintf(src, sizeof src, "%sinpdevs.ini", g_exe_dir);
    GetTempPathA(sizeof dir, dir);
    strncat(dir, "NBALive99", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    _snprintf(g_inpdevs_served, sizeof g_inpdevs_served, "%s\\inpdevs.ini", dir);
    out = CreateFileA(g_inpdevs_served, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        LOG("gamepads: could not write %s; the game reads its own inpdevs.ini (no pad names)", g_inpdevs_served);
        g_inpdevs_served[0] = 0;
        return;
    }
    in = o_CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in != INVALID_HANDLE_VALUE) {
        while (ReadFile(in, chunk, sizeof chunk, &got, NULL) && got) {
            WriteFile(out, chunk, got, &w, NULL);
            user_bytes += got;
        }
        CloseHandle(in);
    }
    n = append(extra, n, sizeof extra, "\r\n; Added by the NBA Live 99 fix (0.4.0): gamepads through SDL3.\r\n");
    for (f = 0; f < 4; f++) {
        n = append(extra, n, sizeof extra, "\r\n[%s_%d]\r\nnumbuttons=%d\r\n", g_pad_family_name[f], PAD_BUTTONS, PAD_BUTTONS);
        for (i = 0; i < PAD_BUTTONS; i++)
            n = append(extra, n, sizeof extra, "button%02dname=%s\r\n", i + 1, g_pad_btn_names[f][i]);
        for (i = 0; i < (int)(sizeof g_pad_layout / sizeof g_pad_layout[0]); i++)
            n = append(extra, n, sizeof extra, "%s=%d\r\n", g_pad_layout[i].fn, g_pad_layout[i].button);
    }
    if (n >= (int)sizeof extra) n = (int)sizeof extra - 1;
    WriteFile(out, extra, (DWORD)n, &w, NULL);
    CloseHandle(out);
    LOG("gamepads: inpdevs.ini served from %s (the game's %lu bytes%s plus our 4 pad sections)", g_inpdevs_served,
        (unsigned long)user_bytes, in == INVALID_HANDLE_VALUE ? ", not found" : "");
}

/* Front end only (v1.1 exe): what ControllerUpdater checks before turning the
   shoot/pass bindings into Enter/Esc. Reads game state, writes nothing. */
#define FE_RENDER_UP   0x61384D   /* byte: ferender_isinitialized */
#define FE_WAIT_BUTTON 0x612F20   /* byte: Configure Controller waits for a button */
#define FE_STATE       0x615174   /* dword: GetFEState; 0x1B = not a menu state */
#define SLOT_BINDINGS  0x683444   /* 20 dwords per slot, 0x74 apart; 8 = shoot, 9 = pass */

static int pad_in_menus(void)
{
    return g_is_v11 && *(volatile uint8_t *)FE_RENDER_UP && !*(volatile uint8_t *)FE_WAIT_BUTTON &&
           *(volatile uint32_t *)FE_STATE != 0x1B;
}

/* the button bits a slot's binding uses, if it is a button of this pad */
static DWORD pad_binding_buttons(int slot, int binding)
{
    uint32_t v = *(volatile uint32_t *)(uintptr_t)(SLOT_BINDINGS + 0x74 * slot + 4 * binding);
    if ((v >> 28) != (uint32_t)(slot + 2)) return 0;      /* not this pad (keyboard or unbound) */
    v &= 0x0FFFFFFFu;
    return v < 0x10000u ? v : 0;                            /* directions are 0x10000 and up */
}

/* A/Cross = accept, B/Circle = back in the menus, whatever shoot and pass are on */
static DWORD pad_menu_buttons(int slot, DWORD b)
{
    DWORD south = b & 1, east = b & 2, shoot, pass;
    if (!pad_in_menus()) return b;
    shoot = pad_binding_buttons(slot, 8);
    pass = pad_binding_buttons(slot, 9);
    if (!shoot || !pass) return b;
    b &= ~(shoot | pass | 3u);
    if (south) b |= shoot;
    if (east) b |= pass;
    return b;
}

static void pad_ini_setup(HMODULE exe)
{
    if (!g_pad_on) return;
    pad_serve_inpdevs();
    o_GetPrivateProfileIntA = (GetPrivateProfileIntA_fn)iat_hook(exe, "KERNEL32.dll", "GetPrivateProfileIntA",
                                                                (void *)h_GetPrivateProfileIntA);
    o_GetPrivateProfileStringA = (GetPrivateProfileStringA_fn)iat_hook(exe, "KERNEL32.dll", "GetPrivateProfileStringA",
                                                                      (void *)h_GetPrivateProfileStringA);
    LOG("gamepads: hooks GetPrivateProfileIntA %s, GetPrivateProfileStringA %s",
        o_GetPrivateProfileIntA ? "ok" : "MISSING", o_GetPrivateProfileStringA ? "ok" : "MISSING");
    if (!o_GetPrivateProfileIntA) o_GetPrivateProfileIntA = GetPrivateProfileIntA;
    if (!o_GetPrivateProfileStringA) o_GetPrivateProfileStringA = GetPrivateProfileStringA;
}
