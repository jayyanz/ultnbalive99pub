/* ------------------------------------------------------------------------- */
/* Alt-Tab in the menus ([nba99] AltTabFix=1, default; v1.1 exe only)        */
/*                                                                           */
/* The front end draws each frame from the main thread: ferender_drawframe   */
/* (0x487780) posts message 0x5FE to the game window, whose thread answers   */
/* in eacthdlr_renderupdate (0x516400) by setting one of two events          */
/* (0x6218A0 to draw, 0x6218A4 to skip), and the main thread waits for them  */
/* (WaitForMultipleObjectsEx, INFINITE). When the game window is minimised   */
/* (IsIconic) or the game is not the active app (dword 0x6E466C = 0) it      */
/* skips the post and only waits. The game window's WM_ACTIVATEAPP handler   */
/* (0x549D40, registered at 0x54AC3C) only sets 0x6E466C to wParam; the      */
/* handler that would also set the skip event (eacthdlr_activateapp          */
/* 0x515DA0) is never registered in this exe. So once the game loses focus   */
/* the main thread waits for good, and getting the focus back does not wake  */
/* it (desktop test PC, 22 Nov 2026: minimised by Windows on Alt-Tab, no     */
/* call from the game, WM_ACTIVATEAPP 0 then 1, main thread waiting from     */
/* ferender_drawframe+0x186 throughout).                                     */
/*                                                                           */
/* Fix, in the exe's IsIconic for that one call (return address 0x48779B,    */
/* ferender_drawframe+0x1B, on the main thread):                             */
/*  - minimised: answer FALSE, so the front end goes on drawing (into the    */
/*    minimised window) while it is the active app;                          */
/*  - not the active app: post 0x5FE to the game window ourselves after a    */
/*    15 ms pause. The window thread sees 0x6E466C = 0 and sets the skip     */
/*    event, the main thread wakes, draws nothing and comes back here: the   */
/*    front end idles until WM_ACTIVATEAPP 1 and then draws as normal.       */
/* Every other caller gets the real answer: the window thread uses it before */
/* SetForegroundWindow (0x54AB0B), which must not take the focus back while  */
/* the player is elsewhere.                                                  */
/* ------------------------------------------------------------------------- */
#define AT_DRAWFRAME_RET 0x0048779Bu
#define AT_ACTIVE_FLAG ((volatile LONG *)(uintptr_t)0x006E466Cu)   /* WM_ACTIVATEAPP wParam */
#define AT_RENDERUPDATE 0x5FE

typedef BOOL (WINAPI *IsIconic_fn)(HWND);
static IsIconic_fn o_IsIconic;
static LONG g_at_logged;

static LONG g_at_idle_logged;

static BOOL WINAPI h_IsIconic(HWND h)
{
    BOOL r = o_IsIconic(h);
    if ((uint32_t)(uintptr_t)__builtin_return_address(0) != AT_DRAWFRAME_RET) return r;
    if (*AT_ACTIVE_FLAG == 0) {
        /* not the active app: the game will only wait; answer its wait ourselves */
        if (NBA99_DEBUG && InterlockedIncrement(&g_at_idle_logged) <= 4)
            LOG("Alt-Tab: game not active; the front end idles until it is active again");
        Sleep(15);
        PostMessageA(h, AT_RENDERUPDATE, 0, 0);
        return r;
    }
    if (r) {
        if (NBA99_DEBUG && InterlockedIncrement(&g_at_logged) <= 4)
            LOG("Alt-Tab: game window minimised; the front end keeps drawing instead of waiting for good");
        return FALSE;
    }
    return r;
}

static void alttab_setup(HMODULE exe)
{
    if (!g_is_v11) return;
    if (!ini_int("AltTabFix", 1)) { LOG("Alt-Tab: fix off (AltTabFix=0)"); return; }
    o_IsIconic = (IsIconic_fn)iat_hook(exe, "USER32.dll", "IsIconic", (void *)h_IsIconic);
    if (!o_IsIconic) { LOG("Alt-Tab: IsIconic import not found, fix not applied"); return; }
    LOG("Alt-Tab: the front end no longer waits for good while the game is minimised or not the active app");
}
