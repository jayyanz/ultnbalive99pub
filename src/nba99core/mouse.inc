/*
 * Front-end mouse (included by nba99core.cpp).
 *
 * The front end is built from real Win32 child windows (EACT controls, made by
 * eacthdlr_create*), placed in the game window's client area in 640x480 game
 * pixels. EA's mouse hook (EACT_MouseHookProc, WH_MOUSE, set in
 * EACT_Initialize) takes the cursor in screen pixels, turns it into client
 * pixels with ScreenToClient, keeps the cursor inside 640x480 (MapWindowPoints
 * and SetCursorPos) and finds the control under it with WindowFromPoint and
 * ChildWindowFromPoint. Our glide2x.dll makes the game window as big as the
 * desktop and shows the 640x480 picture scaled and centred, so in real screen
 * pixels the picture and the child windows no longer line up: clicks missed
 * and the cursor was held in the top-left corner.
 *
 * The game keeps working in real screen pixels (as glide2x's ClientToScreen
 * already gives it for the game window). Where a screen point crosses into the
 * game's 640x480 world, the core moves it between the shown picture and where
 * the child windows really are (the client origin plus game pixels):
 *   - ScreenToClient and MapWindowPoints from the screen into a game window,
 *     WindowFromPoint: picture -> game pixels;
 *   - ClientToScreen and MapWindowPoints from a game window to the screen:
 *     game pixels -> picture.
 * Window-to-window MapWindowPoints and ChildWindowFromPoint (client pixels)
 * are left alone. Button messages: EA's hook lets them through to the window
 * under the real cursor, which is now the big game window instead of the
 * control; after the hook, the core posts the message to the control under the
 * matching picture point (or the control holding the capture), in its client
 * pixels, and drops the original. Hooked in the exe and in CTRLS.DLL. With a 640x480 client
 * (glide2x Windowed at 640x480, or another wrapper) nothing changes.
 * [nba99] MouseMap=1 (default), 0 = off.
 */
typedef BOOL (WINAPI *ScreenToClient_fn)(HWND, LPPOINT);
typedef BOOL (WINAPI *ClientToScreen_fn)(HWND, LPPOINT);
typedef int (WINAPI *MapWindowPoints_fn)(HWND, HWND, LPPOINT, UINT);
typedef HWND (WINAPI *WindowFromPoint_fn)(POINT);
typedef HHOOK (WINAPI *SetWindowsHookExA_fn)(int, HOOKPROC, HINSTANCE, DWORD);
static ScreenToClient_fn mm_ScreenToClient;
static ClientToScreen_fn mm_ClientToScreen;
static MapWindowPoints_fn mm_MapWindowPoints;
static WindowFromPoint_fn mm_WindowFromPoint;
static SetWindowsHookExA_fn mm_SetWindowsHookExA;
static int g_mouse_map;
static HWND g_main_wnd;

/* the game's top-level window for any window of ours, else NULL */
static HWND mm_root(HWND h)
{
    DWORD pid = 0;
    HWND r;
    if (!h || !IsWindow(h)) return NULL;
    r = GetAncestor(h, GA_ROOT);
    if (!r) return NULL;
    GetWindowThreadProcessId(r, &pid);
    if (pid != GetCurrentProcessId()) return NULL;
    return r;
}

/* picture placement in the root's client area: scale and offset; 0 = no mapping needed */
static int mm_view(HWND root, POINT *origin, double *s, double *vx, double *vy)
{
    RECT rc;
    double w, h;
    if (!g_mouse_map || !root || !GetClientRect(root, &rc)) return 0;   /* our import: the real GetClientRect */
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if ((w == 640 && h == 480) || w < 64 || h < 48) return 0;
    *s = w / 640.0 < h / 480.0 ? w / 640.0 : h / 480.0;
    *vx = (w - 640.0 * *s) / 2.0;
    *vy = (h - 480.0 * *s) / 2.0;
    origin->x = 0; origin->y = 0;
    mm_ClientToScreen(root, origin);
    return 1;
}

static long mm_round(double v) { return (long)(v < 0 ? v - 0.5 : v + 0.5); }

/* screen point on the shown picture -> screen point where the 640x480 windows are */
static void mm_p2u(HWND root, POINT *p)
{
    POINT o;
    double s, vx, vy;
    if (!mm_view(root, &o, &s, &vx, &vy)) return;
    p->x = o.x + mm_round((p->x - o.x - vx) / s);
    p->y = o.y + mm_round((p->y - o.y - vy) / s);
}

/* the other way round */
static void mm_u2p(HWND root, POINT *p)
{
    POINT o;
    double s, vx, vy;
    if (!mm_view(root, &o, &s, &vx, &vy)) return;
    p->x = o.x + mm_round(vx + (p->x - o.x) * s);
    p->y = o.y + mm_round(vy + (p->y - o.y) * s);
}

static void mm_note(HWND root) { if (root && !g_main_wnd) { g_main_wnd = root; LOG("mouse: game window 0x%08lx", (unsigned long)(uintptr_t)root); } }

static BOOL WINAPI h_ScreenToClient(HWND h, LPPOINT p)
{
    HWND r = mm_root(h);
    if (r && p) { mm_note(r); mm_p2u(r, p); }
    return mm_ScreenToClient(h, p);
}

static BOOL WINAPI h_ClientToScreen(HWND h, LPPOINT p)
{
    HWND r = mm_root(h);
    BOOL ok = mm_ClientToScreen(h, p);
    if (ok && r && p) { mm_note(r); mm_u2p(r, p); }
    return ok;
}

static int WINAPI h_MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n)
{
    HWND rf = from && from != GetDesktopWindow() ? mm_root(from) : NULL;
    HWND rt = to && to != GetDesktopWindow() ? mm_root(to) : NULL;
    UINT i;
    int r;
    if (!rf && rt && p) {            /* screen -> game window */
        mm_note(rt);
        for (i = 0; i < n; i++) mm_p2u(rt, &p[i]);
        return mm_MapWindowPoints(from, to, p, n);
    }
    r = mm_MapWindowPoints(from, to, p, n);
    if (rf && !rt && p) {            /* game window -> screen */
        mm_note(rf);
        for (i = 0; i < n; i++) mm_u2p(rf, &p[i]);
    }
    return r;
}

static HWND WINAPI h_WindowFromPoint(POINT p)
{
    if (g_main_wnd && IsWindow(g_main_wnd)) {
        RECT rc;
        POINT o = {0, 0};
        GetClientRect(g_main_wnd, &rc);
        mm_ClientToScreen(g_main_wnd, &o);
        if (p.x >= o.x && p.y >= o.y && p.x < o.x + rc.right && p.y < o.y + rc.bottom) mm_p2u(g_main_wnd, &p);
    }
    return mm_WindowFromPoint(p);
}

#if NBA99_DEBUG
static BOOL CALLBACK mm_list_child(HWND h, LPARAM root)
{
    char cls[64] = "", txt[64] = "";
    RECT r;
    POINT a, b;
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, txt, sizeof txt);
    GetWindowRect(h, &r);
    a.x = r.left; a.y = r.top; b.x = r.right; b.y = r.bottom;
    mm_ScreenToClient((HWND)root, &a);
    mm_ScreenToClient((HWND)root, &b);
    LOG("  child 0x%08lx %-24s \"%s\" at %ld,%ld size %ldx%ld%s%s", (unsigned long)(uintptr_t)h, cls, txt, a.x, a.y,
        b.x - a.x, b.y - a.y, IsWindowVisible(h) ? ", visible" : "", (GetWindowLongA(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) ? ", transparent" : "");
    return TRUE;
}
#endif

#if NBA99_DEBUG
static BOOL CALLBACK mm_list_top(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    char cls[64] = "", txt[64] = "";
    RECT r;
    (void)lp;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    GetClassNameA(h, cls, sizeof cls);
    GetWindowTextA(h, txt, sizeof txt);
    GetWindowRect(h, &r);
    LOG(" window 0x%08lx %-24s \"%s\" screen %ld,%ld size %ldx%ld style 0x%08lx ex 0x%08lx%s", (unsigned long)(uintptr_t)h, cls, txt,
        r.left, r.top, r.right - r.left, r.bottom - r.top, (unsigned long)GetWindowLongA(h, GWL_STYLE),
        (unsigned long)GetWindowLongA(h, GWL_EXSTYLE), IsWindowVisible(h) ? ", visible" : "");
    EnumChildWindows(h, mm_list_child, (LPARAM)h);
    return TRUE;
}
#endif

/* WH_MOUSE / WH_MOUSE_LL procedures from the game run through these. The point stays in real
   screen pixels (the game converts it with ScreenToClient, mapped above); the wrapper finds
   the game window and, in the Debug build, logs the first messages and the game's windows. */
#define MM_HOOKS 4
static HOOKPROC g_mm_proc[MM_HOOKS];
static int g_mm_ll[MM_HOOKS];

/* Mouse messages we posted to a control ourselves (the hook sees them again when the game
   takes them from its queue): passed straight through. */
#define MM_POSTED 16
static struct { HWND h; UINT msg; LPARAM lp; DWORD t; } g_mm_posted[MM_POSTED];   /* Wine never shows them to the hook again */
static volatile LONG g_mm_npost;

static int mm_was_posted(HWND h, UINT msg)
{
    int i;
    for (i = 0; i < MM_POSTED; i++)
        if (g_mm_posted[i].h == h && g_mm_posted[i].msg == msg && GetTickCount() - g_mm_posted[i].t < 1000) {
            g_mm_posted[i].h = NULL;
            return 1;
        }
    return 0;
}

static int mm_is_descendant(HWND root, HWND h)
{
    return h && root && h != root && IsChild(root, h);
}

/* The control the message belongs to: the one holding the capture, else the one under the
   matching point of the picture; NULL when that is the game window itself. */
static HWND mm_target(HWND root, POINT p)
{
    HWND cap = GetCapture(), to;
    if (cap && (cap == root || mm_is_descendant(root, cap))) to = cap;
    else { mm_p2u(root, &p); to = mm_WindowFromPoint(p); }
    return to && to != root && mm_is_descendant(root, to) ? to : NULL;
}

/* A button message for the window under the real cursor (the big game window): send it to the
   control under the matching point of the picture instead, as Windows did at 640x480. Also
   when a control holds the mouse capture (its message carried coordinates from the real
   cursor). Returns 1 when the original message is to be dropped. */
typedef struct { POINT pt; HWND real_hwnd; } mm_msg;
static int mm_retarget(HWND root, WPARAM msg, const mm_msg *m)
{
    POINT u = m->pt, c;
    HWND cap = GetCapture(), to;
    WPARAM keys = 0;
    if (msg < WM_MOUSEMOVE || msg > WM_MBUTTONDBLCLK || !root) return 0;
    to = mm_target(root, m->pt);
    if (!to) return 0;                                                /* the game window itself: glide2x maps it */
    if (to == m->real_hwnd && !mm_is_descendant(root, cap)) return 0; /* Windows chose it already (640x480 client) */
    mm_p2u(root, &u);
    c = u;
    mm_ScreenToClient(to, &c);
    if (GetKeyState(VK_LBUTTON) < 0) keys |= MK_LBUTTON;
    if (GetKeyState(VK_RBUTTON) < 0) keys |= MK_RBUTTON;
    if (GetKeyState(VK_MBUTTON) < 0) keys |= MK_MBUTTON;
    if (GetKeyState(VK_SHIFT) < 0) keys |= MK_SHIFT;
    if (GetKeyState(VK_CONTROL) < 0) keys |= MK_CONTROL;
    /* the key state can lag the message: the button this message presses or releases decides */
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) keys |= MK_LBUTTON;
    if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) keys |= MK_RBUTTON;
    if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK) keys |= MK_MBUTTON;
    if (msg == WM_LBUTTONUP) keys &= ~(WPARAM)MK_LBUTTON;
    if (msg == WM_RBUTTONUP) keys &= ~(WPARAM)MK_RBUTTON;
    if (msg == WM_MBUTTONUP) keys &= ~(WPARAM)MK_MBUTTON;
    {
        LONG k = (InterlockedIncrement(&g_mm_npost) - 1) % MM_POSTED;
        g_mm_posted[k].h = to; g_mm_posted[k].msg = (UINT)msg; g_mm_posted[k].lp = MAKELPARAM(c.x, c.y); g_mm_posted[k].t = GetTickCount();
    }
    PostMessageA(to, (UINT)msg, keys, MAKELPARAM(c.x, c.y));
#if NBA99_DEBUG
    {
        static LONG n;
        char cls[64] = "";
        GetClassNameA(to, cls, sizeof cls);
        if (InterlockedIncrement(&n) <= 30)
            LOG("mouse: message 0x%04lx sent to control 0x%08lx %s at %ld,%ld (was for 0x%08lx)%s", (unsigned long)msg,
                (unsigned long)(uintptr_t)to, cls, c.x, c.y, (unsigned long)(uintptr_t)m->real_hwnd, cap ? ", control has the capture" : "");
    }
#endif
    return 1;
}

#if NBA99_DEBUG
static int g_mm_trace = -1;   /* [nba99] MouseTrace=N: log the next N hook calls after the first button press */
static volatile LONG g_mm_trace_left;
static void mm_trace(const char *what, int code, WPARAM wp, const MOUSEHOOKSTRUCT *m, LRESULT res)
{
    char cls[48] = "";
    if (g_mm_trace_left <= 0) return;
    InterlockedDecrement(&g_mm_trace_left);
    if (m) GetClassNameA(m->hwnd, cls, sizeof cls);
    LOG("mouse trace %s: code %d msg 0x%04lx hwnd 0x%08lx %s pt %ld,%ld hit %u -> %ld (capture 0x%08lx)", what, code,
        (unsigned long)wp, m ? (unsigned long)(uintptr_t)m->hwnd : 0ul, cls, m ? m->pt.x : 0l, m ? m->pt.y : 0l,
        m ? m->wHitTestCode : 0u, (long)res, (unsigned long)(uintptr_t)GetCapture());
}
#define MM_TRACE(w, c, wp, m, r) mm_trace(w, c, wp, m, r)
#else
#define MM_TRACE(w, c, wp, m, r) ((void)0)
#endif

static LRESULT mm_hook_call(int i, int code, WPARAM wp, LPARAM lp)
{
#if NBA99_DEBUG
    if (g_mm_trace < 0) g_mm_trace = ini_int("MouseTrace", 0);
    if (g_mm_trace && wp == WM_LBUTTONDOWN && code == HC_ACTION && !g_mm_trace_left) { g_mm_trace_left = g_mm_trace; g_mm_trace = 0; }
#endif
    if (code == HC_ACTION && lp && !g_mm_ll[i]) {
        MOUSEHOOKSTRUCT m = *(MOUSEHOOKSTRUCT *)lp;
        HWND r = mm_root(m.hwnd);
        LRESULT res;
        if (r) mm_note(r);
        if (r && wp >= WM_MOUSEMOVE && wp <= WM_MBUTTONDBLCLK && mm_was_posted(m.hwnd, (UINT)wp))
        {
            MM_TRACE("ours", code, wp, &m, 0);
            return 0;                                           /* one of ours: the game's hook saw the original */
        }
#if NBA99_DEBUG
        {
            static LONG n, downs;
            LONG d = wp == WM_LBUTTONDOWN ? InterlockedIncrement(&downs) : 0;
            if (InterlockedIncrement(&n) <= 12 || (d && d <= 6)) {
                char cls[64] = "";
                POINT u = m.pt;
                HWND under;
                if (r) mm_p2u(r, &u);
                under = mm_WindowFromPoint(u);
                GetClassNameA(under, cls, sizeof cls);
                LOG("mouse hook: msg 0x%04lx hwnd 0x%08lx (root 0x%08lx) screen %ld,%ld -> %ld,%ld, window there 0x%08lx %s",
                    (unsigned long)wp, (unsigned long)(uintptr_t)m.hwnd, (unsigned long)(uintptr_t)r, m.pt.x, m.pt.y, u.x, u.y,
                    (unsigned long)(uintptr_t)under, cls);
                if (d == 1) { LOG("mouse: windows of the game at the first click:"); EnumWindows(mm_list_top, 0); }
            }
        }
#endif
        {   /* the game's hook sees real screen pixels, and the control under the picture point
               as the window (it tracks the control under the mouse with it) */
            MOUSEHOOKSTRUCT g = m;
            mm_msg mm;
            HWND to = r && wp >= WM_MOUSEMOVE && wp <= WM_MBUTTONDBLCLK ? mm_target(r, m.pt) : NULL;
            if (to && to != m.hwnd) { g.hwnd = to; g.wHitTestCode = HTCLIENT; }
            res = g_mm_proc[i](code, wp, (LPARAM)&g);
            MM_TRACE("game", code, wp, &g, res);
            if (res) return res;                                /* it handled the message itself */
            mm.pt = m.pt; mm.real_hwnd = m.hwnd;
            if (r && mm_retarget(r, wp, &mm)) return 1;         /* sent to the control instead */
            return res;
        }
    }
    {
        LRESULT res = g_mm_proc[i](code, wp, lp);
        MM_TRACE(lp ? "other" : "other(no data)", code, wp, (const MOUSEHOOKSTRUCT *)lp, res);
        return res;
    }
}

static LRESULT CALLBACK mm_hook0(int c, WPARAM w, LPARAM l) { return mm_hook_call(0, c, w, l); }
static LRESULT CALLBACK mm_hook1(int c, WPARAM w, LPARAM l) { return mm_hook_call(1, c, w, l); }
static LRESULT CALLBACK mm_hook2(int c, WPARAM w, LPARAM l) { return mm_hook_call(2, c, w, l); }
static LRESULT CALLBACK mm_hook3(int c, WPARAM w, LPARAM l) { return mm_hook_call(3, c, w, l); }
static const HOOKPROC k_mm_hooks[MM_HOOKS] = { mm_hook0, mm_hook1, mm_hook2, mm_hook3 };

static HHOOK WINAPI h_SetWindowsHookExA(int id, HOOKPROC proc, HINSTANCE mod, DWORD tid)
{
    char at[96];
    CALLER(at);
    if ((id == WH_MOUSE || id == WH_MOUSE_LL) && proc && g_mouse_map) {
        int i;
        for (i = 0; i < MM_HOOKS; i++) if (g_mm_proc[i] == proc || !g_mm_proc[i]) break;
        if (i < MM_HOOKS) {
            HHOOK h;
            g_mm_proc[i] = proc;
            g_mm_ll[i] = id == WH_MOUSE_LL;
            h = mm_SetWindowsHookExA(id, k_mm_hooks[i], mod, tid);
            LOG("mouse: SetWindowsHookEx(%s, 0x%08lx) from %s: through our mapping (slot %d), hook 0x%08lx",
                id == WH_MOUSE ? "WH_MOUSE" : "WH_MOUSE_LL", (unsigned long)(uintptr_t)proc, at, i, (unsigned long)(uintptr_t)h);
            return h;
        }
    }
    LOG("SetWindowsHookEx(%d, 0x%08lx) from %s", id, (unsigned long)(uintptr_t)proc, at);
    return mm_SetWindowsHookExA(id, proc, mod, tid);
}

/* exe at start; CTRLS.DLL (and any other module the game loads) from hook_module */
static void mouse_hook_module(HMODULE m, const char *name)
{
    void *p;
    int n = 0;
    if (!g_mouse_map) return;
#define MMH(fn) p = iat_hook(m, "USER32.dll", #fn, (void *)h_##fn); \
                if (p) { n++; if (!mm_##fn && p != (void *)h_##fn) mm_##fn = (fn##_fn)p; }
    MMH(ScreenToClient) MMH(ClientToScreen) MMH(MapWindowPoints) MMH(WindowFromPoint) MMH(SetWindowsHookExA)
#undef MMH
    if (n) LOG("mouse: %d of the window/point imports of %s mapped to the picture", n, name);
}

static void mouse_setup(HMODULE exe)
{
    g_mouse_map = ini_int("MouseMap", 1);
    mm_ScreenToClient = ScreenToClient; mm_ClientToScreen = ClientToScreen; mm_MapWindowPoints = MapWindowPoints;
    mm_WindowFromPoint = WindowFromPoint; mm_SetWindowsHookExA = SetWindowsHookExA;
    if (!g_mouse_map) { LOG("mouse: mapping off (MouseMap=0)"); return; }
    mouse_hook_module(exe, "the exe");
}
