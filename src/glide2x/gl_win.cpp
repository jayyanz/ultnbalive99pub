/*
 * glide2x.dll for NBA Live 99 - game window glue.
 *
 * The game believes its window is the 640x480 Glide screen: it reads the
 * mouse from WM_MOUSEMOVE / button messages in client pixels and moves the
 * cursor with ClientToScreen + SetCursorPos. Our window covers the monitor
 * with a scaled 4:3 image, so:
 *  - mouse messages are mapped from window pixels to 640x480 Glide pixels
 *    (window subclass), clamped to the screen;
 *  - the exe's ClientToScreen maps 640x480 Glide pixels back to the window;
 *  - the exe's GetClientRect reports 640x480 for the game window.
 * The exe's import table entries are restored when the window closes or the
 * DLL unloads (the game unloads and reloads glide2x.dll during detection).
 */
#include "nba99_cxx.h"   /* C++ build settings, first (0.7.15) */
#include "gl_internal.h"

NBA99_C_BEGIN
typedef BOOL (WINAPI *ClientToScreen_fn)(HWND, LPPOINT);
typedef BOOL (WINAPI *GetClientRect_fn)(HWND, LPRECT);

static HWND g_hwnd;
static WNDPROC g_orig_proc;
static ClientToScreen_fn o_ClientToScreen;
static GetClientRect_fn o_GetClientRect;
static void **g_slot_c2s, **g_slot_gcr;

static void **find_iat_slot(HMODULE mod, const char *dll, const char *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!dd->VirtualAddress) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *addrs;
        if (lstrcmpiA((const char *)(base + imp->Name), dll) != 0) continue;
        names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn) == 0) return (void **)&addrs->u1.Function;
        }
    }
    return NULL;
}

static void *swap_slot(void **slot, void *value)
{
    DWORD old;
    void *prev;
    if (!slot) return NULL;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return NULL;
    prev = *slot;
    *slot = value;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return prev;
}

/* window pixels -> 640x480 Glide pixels */
static LPARAM map_mouse(LPARAM lp)
{
    int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
    int px = G.pic_w > 0 ? G.pic_x : G.view_x, py = G.pic_w > 0 ? G.pic_y : G.view_y;
    int pw = G.pic_w > 0 ? G.pic_w : G.view_w, ph = G.pic_w > 0 ? G.pic_h : G.view_h;
    if (pw > 0 && ph > 0) {
        x = (int)(((long long)(x - px) * FB_W) / pw);
        y = (int)(((long long)(y - py) * FB_H) / ph);
    }
    if (x < 0) x = 0;
    if (x > FB_W - 1) x = FB_W - 1;
    if (y < 0) y = 0;
    if (y > FB_H - 1) y = FB_H - 1;
    return MAKELPARAM(x, y);
}

/* The game sizes its own window for a 640x480 screen (seen on Windows: the
 * window shrank to 640x480 in the top-left corner after we had made it cover
 * the monitor). Keep the size we chose: the whole monitor when full screen,
 * our window size when windowed. */
static void keep_our_size(HWND h, WINDOWPOS *p)
{
    MONITORINFO mi;
    if (!p || (p->flags & SWP_NOSIZE && p->flags & SWP_NOMOVE)) return;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    if (!G.cfg_windowed) {
        p->x = mi.rcMonitor.left;
        p->y = mi.rcMonitor.top;
        p->cx = mi.rcMonitor.right - mi.rcMonitor.left;
        p->cy = mi.rcMonitor.bottom - mi.rcMonitor.top;
        p->flags &= ~(UINT)SWP_NOMOVE;
    } else {
        RECT r;
        SetRect(&r, 0, 0, G.cfg_width > 0 ? G.cfg_width : 1280, G.cfg_height > 0 ? G.cfg_height : 960);
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(h, GWL_STYLE), FALSE);
        p->cx = r.right - r.left;
        p->cy = r.bottom - r.top;
        if (!(p->flags & SWP_NOMOVE) && (p->x < mi.rcWork.left || p->y < mi.rcWork.top)) {
            p->x = mi.rcWork.left + 40;
            p->y = mi.rcWork.top + 40;
        }
    }
    p->flags &= ~(UINT)(SWP_NOSIZE);
}

/* Set when glide2x is closed but could not take its procedure out of the window (another one
   was put in front of it): from then on every message goes straight to the original one. */
static volatile LONG g_passthrough;
static WNDPROC g_pass_proc;

/* Debug: focus, activation and window-state messages of the game window, in order (Alt-Tab) */
#if NBA99_DEBUG
static LONG g_focus_logs;
#endif
static void focus_log(HWND h, UINT msg, WPARAM wp, LPARAM lp, const WINDOWPOS *before)
{
#if NBA99_DEBUG
    const char *n = NULL;
    if (!NBA99_DEBUG || g_focus_logs > 400) return;
    switch (msg) {
    case WM_ACTIVATEAPP: n = "WM_ACTIVATEAPP"; break;
    case WM_ACTIVATE: n = "WM_ACTIVATE"; break;
    case WM_NCACTIVATE: n = "WM_NCACTIVATE"; break;
    case WM_SETFOCUS: n = "WM_SETFOCUS"; break;
    case WM_KILLFOCUS: n = "WM_KILLFOCUS"; break;
    case WM_SIZE: n = "WM_SIZE"; break;
    case WM_SHOWWINDOW: n = "WM_SHOWWINDOW"; break;
    case WM_SYSCOMMAND: if ((wp & 0xFFF0) == SC_MINIMIZE || (wp & 0xFFF0) == SC_RESTORE) n = "WM_SYSCOMMAND"; break;
    case WM_WINDOWPOSCHANGING: n = "WM_WINDOWPOSCHANGING"; break;
    case WM_WINDOWPOSCHANGED: n = "WM_WINDOWPOSCHANGED"; break;
    default: break;
    }
    if (!n) return;
    InterlockedIncrement(&g_focus_logs);
    if (msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED) {
        const WINDOWPOS *p = (const WINDOWPOS *)lp;
        if (!p || (p->flags & SWP_NOSIZE && p->flags & SWP_NOMOVE)) { InterlockedDecrement(&g_focus_logs); return; }
        if (before && (before->x != p->x || before->y != p->y || before->cx != p->cx || before->cy != p->cy))
            glog("focus: %s %d,%d %dx%d flags 0x%x changed by glide2x to %d,%d %dx%d (iconic %d, thread %lu)", n,
                 before->x, before->y, before->cx, before->cy, p->flags, p->x, p->y, p->cx, p->cy, IsIconic(h) ? 1 : 0,
                 GetCurrentThreadId());
        else
            glog("focus: %s %d,%d %dx%d flags 0x%x (iconic %d, thread %lu)", n, p->x, p->y, p->cx, p->cy, p->flags,
                 IsIconic(h) ? 1 : 0, GetCurrentThreadId());
        return;
    }
    glog("focus: %s wParam 0x%lx lParam 0x%lx (iconic %d, foreground %s, thread %lu)", n, (unsigned long)wp,
         (unsigned long)lp, IsIconic(h) ? 1 : 0, GetForegroundWindow() == h ? "game" : "other", GetCurrentThreadId());
#else
    (void)h; (void)msg; (void)wp; (void)lp; (void)before;
#endif
}

static LRESULT CALLBACK game_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_passthrough) return CallWindowProcA(g_pass_proc, h, msg, wp, lp);
    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS before;
        if (NBA99_DEBUG && lp) before = *(WINDOWPOS *)lp;
        keep_our_size(h, (WINDOWPOS *)lp);
        if (NBA99_DEBUG) focus_log(h, msg, wp, lp, lp ? &before : NULL);
    } else if (NBA99_DEBUG) {
        focus_log(h, msg, wp, lp, NULL);
    }
    /* Ctrl+F11: show or hide the overlay (the game never sees this key) */
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F11 && GetKeyState(VK_CONTROL) < 0) {
        if (!(lp & 0x40000000)) InterlockedIncrement(&G.overlay_toggles);   /* ignore auto-repeat */
        return 0;
    }
    if (NBA99_DEBUG && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_F10 && GetKeyState(VK_CONTROL) < 0 &&
        !(lp & 0x40000000)) {
        InterlockedIncrement(&G.trace_req);   /* Debug: trace the next frame's draws (logs\glide_trace_N.txt) */
        return 0;
    }
    /* Page Down: screenshot (0.7.12; the game never sees this key) */
    if (G.cfg_shots && (msg == WM_KEYDOWN || msg == WM_KEYUP) && wp == VK_NEXT) {
        if (msg == WM_KEYDOWN && !(lp & 0x40000000)) InterlockedIncrement(&G.shot_req);
        return 0;
    }
    if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK) lp = map_mouse(lp);
    return CallWindowProcA(g_orig_proc, h, msg, wp, lp);
}

static BOOL WINAPI h_ClientToScreen(HWND h, LPPOINT p)
{
    if (h == g_hwnd && p && G.view_w > 0 && G.view_h > 0) {
        int px = G.pic_w > 0 ? G.pic_x : G.view_x, py = G.pic_w > 0 ? G.pic_y : G.view_y;
        int pw = G.pic_w > 0 ? G.pic_w : G.view_w, ph = G.pic_w > 0 ? G.pic_h : G.view_h;
        p->x = px + (LONG)(((long long)p->x * pw) / FB_W);
        p->y = py + (LONG)(((long long)p->y * ph) / FB_H);
    }
    return o_ClientToScreen(h, p);
}

/* Our keys through a keyboard hook on the game window's thread (NBA Live 99): the 99 front end
   is made of child windows, and the one with the focus gets the key messages, so the game
   window's own procedure (above) never saw Page Down, Ctrl+F11 or Ctrl+F10 in the menus.
   The hook sees them for every window of that thread and keeps them from the game. */
static HHOOK g_kb_hook;
static LRESULT CALLBACK kb_hook(int code, WPARAM vk, LPARAM lp)
{
    if (code == HC_ACTION) {
        int down = !(lp & 0x80000000), first = down && !(lp & 0x40000000);
        if (vk == VK_F11 && GetKeyState(VK_CONTROL) < 0) {
            if (first) InterlockedIncrement(&G.overlay_toggles);
            return 1;
        }
        if (NBA99_DEBUG && vk == VK_F10 && GetKeyState(VK_CONTROL) < 0) {
            if (first) InterlockedIncrement(&G.trace_req);
            return 1;
        }
        if (G.cfg_shots && vk == VK_NEXT) {
            if (first) InterlockedIncrement(&G.shot_req);
            return 1;
        }
    }
    return CallNextHookEx(g_kb_hook, code, vk, lp);
}

static BOOL WINAPI h_GetClientRect(HWND h, LPRECT r)
{
    if (h == g_hwnd && r) { SetRect(r, 0, 0, FB_W, FB_H); return TRUE; }
    return o_GetClientRect(h, r);
}

void win_attach(HWND hwnd)
{
    HMODULE exe = GetModuleHandleA(NULL);
    if (g_hwnd) win_detach();
    g_hwnd = hwnd;
    g_passthrough = 0;
    g_orig_proc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)game_wndproc);
    {   /* Once our procedure is in the game window, glide2x stays loaded for the rest of the
           process: EA's THRASH driver unloads glide2x when the game closes its 3D (at quit),
           while the game's window thread still runs, and if anything put its own procedure in
           front of ours, ours cannot be taken out and must still be there (seen with NBA Live
           99 0.1.0: a crash in the unloaded glide2x after quitting). One extra reference with
           LoadLibraryA of our own file, never released (not GetModuleHandleEx with PIN: that is
           Windows XP and later, and glide2x imports nothing newer than Windows 98). */
        static int pinned;
        MEMORY_BASIC_INFORMATION mbi;
        char path[MAX_PATH];
        if (!pinned && VirtualQuery((void *)game_wndproc, &mbi, sizeof mbi) && mbi.AllocationBase &&
            GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof path) && LoadLibraryA(path)) {
            pinned = 1;
            glog("window: glide2x.dll stays loaded while the game runs (window procedure in place)");
        }
    }
    if (!o_ClientToScreen) {
        g_slot_c2s = find_iat_slot(exe, "USER32.dll", "ClientToScreen");
        o_ClientToScreen = (ClientToScreen_fn)swap_slot(g_slot_c2s, (void *)h_ClientToScreen);
        if (!o_ClientToScreen) { o_ClientToScreen = ClientToScreen; g_slot_c2s = NULL; }
    }
    if (!o_GetClientRect) {
        g_slot_gcr = find_iat_slot(exe, "USER32.dll", "GetClientRect");
        o_GetClientRect = (GetClientRect_fn)swap_slot(g_slot_gcr, (void *)h_GetClientRect);
        if (!o_GetClientRect) { o_GetClientRect = GetClientRect; g_slot_gcr = NULL; }
    }
    if (!g_kb_hook) g_kb_hook = SetWindowsHookExA(WH_KEYBOARD, kb_hook, NULL, GetWindowThreadProcessId(hwnd, NULL));
    glog("window: mouse mapped to the 640x480 Glide screen (subclass %s, ClientToScreen %s, GetClientRect %s); "
         "keys Page Down, Ctrl+F11, Ctrl+F10 through a keyboard hook (%s)",
         g_orig_proc ? "ok" : "FAILED", g_slot_c2s ? "hooked" : "not hooked", g_slot_gcr ? "hooked" : "not hooked",
         g_kb_hook ? "ok" : "FAILED, only in the game window");
}

void win_detach(void)
{
    if (g_hwnd && g_orig_proc && IsWindow(g_hwnd)) {
        if ((WNDPROC)GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC) == game_wndproc) {
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig_proc);
            glog("window: our window procedure taken out");
        } else {
            /* something put its own procedure in front of ours and still calls ours */
            g_pass_proc = g_orig_proc;
            InterlockedExchange(&g_passthrough, 1);
            glog("window: another window procedure is in front of ours: ours stays and passes every message on");
        }
    }
    if (g_slot_c2s && o_ClientToScreen) swap_slot(g_slot_c2s, (void *)o_ClientToScreen);
    if (g_slot_gcr && o_GetClientRect) swap_slot(g_slot_gcr, (void *)o_GetClientRect);
    g_slot_c2s = g_slot_gcr = NULL;
    if (g_kb_hook) { UnhookWindowsHookEx(g_kb_hook); g_kb_hook = NULL; }
    o_ClientToScreen = NULL;
    o_GetClientRect = NULL;
    g_hwnd = NULL;
    g_orig_proc = NULL;
}
NBA99_C_END
