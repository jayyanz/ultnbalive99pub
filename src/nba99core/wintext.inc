/* ------------------------------------------------------------------------- */
/* Front-end text labels (WindowTextFix=1, default)                          */
/*                                                                           */
/* The menus' text labels are NBATEXTCONTROL child windows with the game's   */
/* window procedure EACT_TextControlProc (0x51C250). The layout code sets    */
/* their text with SetWindowTextA (Layman_SetText 0x4C5370), with the        */
/* game's own string from the CSV language tables. On WM_SETTEXT, a label    */
/* without its own buffer keeps the lParam pointer as its text               */
/* (0x51D06A: mov [data+0x28], lParam) and draws from it later.              */
/*                                                                           */
/* Windows 95/98 (and Wine) pass the caller's pointer to the window          */
/* procedure. Windows NT to 11 pass WM_SETTEXT through a temporary copy that */
/* is freed or reused once the message returns, so the label is left         */
/* pointing at stale memory and draws whatever is there later: the bytes of  */
/* 0x0051C250 read as "P\xC2Q" (P, A circumflex, Q); other labels show "I". */
/*                                                                           */
/* The layout code sets the labels from another thread than the one that   */
/* owns them (seen under Wine: Game Setup's STYLE and QUARTER labels and its */
/* NBAMULTIBUTTON values); between threads NT always copies WM_SETTEXT.      */
/*                                                                           */
/* Fix: the exe's SetWindowTextA, SendMessageA and SendNotifyMessageA are    */
/* hooked. A WM_SETTEXT to a window whose window procedure is game code      */
/* (NBAWIN.EXE or CTRLS.DLL) goes to that procedure with CallWindowProcA     */
/* and the game's own pointer, as on Windows 98:                             */
/*  - same thread: called directly;                                          */
/*  - other thread: sent as WM_APP (never copied by Windows) to a hidden     */
/*    helper window of the owning thread, which makes the call there. The    */
/*    send waits for the answer, like the original. The exe's CreateWindowExA*/
/*    is hooked to make one helper for each thread that creates windows.     */
/* Anything else goes through as normal.                                     */
/* ------------------------------------------------------------------------- */
typedef BOOL (WINAPI *SetWindowTextA_fn)(HWND, LPCSTR);
typedef LRESULT (WINAPI *SendMessageA_fn)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *SendNotifyMessageA_fn)(HWND, UINT, WPARAM, LPARAM);
static SetWindowTextA_fn o_SetWindowTextA;
static SendMessageA_fn o_SendMessageA;
static SendNotifyMessageA_fn o_SendNotifyMessageA;
static HMODULE g_wt_exe;
static LONG g_wt_direct, g_wt_logged;

static int wt_game_proc(WNDPROC p)
{
    HMODULE m = NULL;
    if (!p) return 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &m) || !m)
        return 0;
    if (m == g_wt_exe) return 1;
    {
        HMODULE c = GetModuleHandleA("ctrls.dll");
        return c && m == c;
    }
}

/* 1 and *res set if the message went straight to the game's procedure */
static LONG g_wt_passed;

static void wt_pass_log(HWND h, LPCSTR text, const char *via, const char *why, WNDPROC p)
{
    char cls[32] = "", t[48] = "";
    if (!NBA99_DEBUG || InterlockedIncrement(&g_wt_passed) > 12) return;
    GetClassNameA(h, cls, sizeof cls);
    if (text && !IsBadStringPtrA(text, sizeof t)) { strncpy(t, text, sizeof t - 1); t[sizeof t - 1] = 0; }
    LOG("text: %s to %s 0x%08lx \"%s\" passed on as normal (%s, procedure 0x%08lx)",
        via, cls, (unsigned long)(uintptr_t)h, t, why, (unsigned long)(uintptr_t)p);
}

/* hidden helper window for each thread that creates windows */
#define WT_MSG (WM_APP + 0x99)
#define WT_MAX_HELPERS 16
static struct { DWORD tid; HWND helper; } g_wt_help[WT_MAX_HELPERS];
static CRITICAL_SECTION g_wt_cs;
static ATOM g_wt_class;

static LRESULT CALLBACK wt_helper_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WT_MSG) {   /* wp = target window, lp = the game's text */
        HWND t = (HWND)wp;
        WNDPROC p = IsWindow(t) ? (WNDPROC)GetWindowLongA(t, GWL_WNDPROC) : NULL;
        return p ? CallWindowProcA(p, t, WM_SETTEXT, 0, lp) : 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static HWND wt_helper_of(DWORD tid)
{
    HWND r = NULL;
    int i;
    EnterCriticalSection(&g_wt_cs);
    for (i = 0; i < WT_MAX_HELPERS; i++)
        if (g_wt_help[i].tid == tid) { r = g_wt_help[i].helper; break; }
    LeaveCriticalSection(&g_wt_cs);
    return r && IsWindow(r) ? r : NULL;
}

static void wt_make_helper(void)   /* on the thread that just created a window */
{
    DWORD tid = GetCurrentThreadId();
    HWND h;
    int i, slot = -1;
    if (!g_wt_class || wt_helper_of(tid)) return;
    h = CreateWindowExA(0, (LPCSTR)(uintptr_t)g_wt_class, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                        GetModuleHandleA(NULL), NULL);
    if (!h) { LOG("text: helper window for thread %lu not made (error %lu)", tid, GetLastError()); return; }
    EnterCriticalSection(&g_wt_cs);
    for (i = 0; i < WT_MAX_HELPERS; i++) {
        if (g_wt_help[i].tid == tid || !g_wt_help[i].tid || !IsWindow(g_wt_help[i].helper)) { slot = i; break; }
    }
    if (slot >= 0) { g_wt_help[slot].tid = tid; g_wt_help[slot].helper = h; }
    LeaveCriticalSection(&g_wt_cs);
    if (slot < 0) { DestroyWindow(h); LOG("text: no room for a helper window for thread %lu", tid); return; }
    LOG("text: helper window for thread %lu", tid);
}

typedef HWND (WINAPI *CreateWindowExA_fn)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static CreateWindowExA_fn o_CreateWindowExA;

static HWND WINAPI h_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int hh,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND r = o_CreateWindowExA(ex, cls, name, style, x, y, w, hh, parent, menu, inst, param);
    if (r) wt_make_helper();
    return r;
}

static int wt_direct(HWND h, LPCSTR text, LRESULT *res, const char *via)
{
    WNDPROC p;
    DWORD tid;
    const char *how;
    if (!h || !IsWindow(h)) return 0;
    p = (WNDPROC)GetWindowLongA(h, GWL_WNDPROC);
    if (!wt_game_proc(p)) {
        wt_pass_log(h, text, via, "procedure not in the game", p);
        return 0;
    }
    tid = GetWindowThreadProcessId(h, NULL);
    if (tid == GetCurrentThreadId()) {
        *res = CallWindowProcA(p, h, WM_SETTEXT, 0, (LPARAM)text);
        how = "called directly";
    } else {
        HWND helper = wt_helper_of(tid);
        if (!helper) {
            wt_pass_log(h, text, via, "window of another thread with no helper window", p);
            return 0;
        }
        *res = o_SendMessageA(helper, WT_MSG, (WPARAM)h, (LPARAM)text);
        how = "through the owning thread's helper window";
    }
    InterlockedIncrement(&g_wt_direct);
    if (NBA99_DEBUG && InterlockedIncrement(&g_wt_logged) <= 8) {
        char cls[32] = "", t[48] = "";
        GetClassNameA(h, cls, sizeof cls);
        if (text && !IsBadStringPtrA(text, sizeof t)) { strncpy(t, text, sizeof t - 1); t[sizeof t - 1] = 0; }
        LOG("text: %s to %s 0x%08lx \"%s\" given to the game's procedure 0x%08lx with the game's pointer (%s)",
            via, cls, (unsigned long)(uintptr_t)h, t, (unsigned long)(uintptr_t)p, how);
    }
    return 1;
}

static BOOL WINAPI h_SetWindowTextA(HWND h, LPCSTR text)
{
    LRESULT r;
    if (wt_direct(h, text, &r, "SetWindowTextA")) return TRUE;
    return o_SetWindowTextA(h, text);
}

static LRESULT WINAPI h_SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (msg == WM_SETTEXT && wt_direct(h, (LPCSTR)lp, &r, "SendMessageA(WM_SETTEXT)")) return r;
    return o_SendMessageA(h, msg, wp, lp);
}

static BOOL WINAPI h_SendNotifyMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (msg == WM_SETTEXT && wt_direct(h, (LPCSTR)lp, &r, "SendNotifyMessageA(WM_SETTEXT)")) return TRUE;
    return o_SendNotifyMessageA(h, msg, wp, lp);
}

static void wintext_setup(HMODULE exe)
{
    WNDCLASSA wc;
    g_wt_exe = exe;
    if (!ini_int("WindowTextFix", 1)) { LOG("text: label fix off (WindowTextFix=0)"); return; }
    InitializeCriticalSection(&g_wt_cs);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wt_helper_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "NBA99CoreTextHelper";
    g_wt_class = RegisterClassA(&wc);
    o_CreateWindowExA = (CreateWindowExA_fn)iat_hook(exe, "USER32.dll", "CreateWindowExA", (void *)h_CreateWindowExA);
    if (!o_CreateWindowExA) { o_CreateWindowExA = CreateWindowExA; g_wt_class = 0; }
    o_SetWindowTextA = (SetWindowTextA_fn)iat_hook(exe, "USER32.dll", "SetWindowTextA", (void *)h_SetWindowTextA);
    o_SendMessageA = (SendMessageA_fn)iat_hook(exe, "USER32.dll", "SendMessageA", (void *)h_SendMessageA);
    o_SendNotifyMessageA = (SendNotifyMessageA_fn)iat_hook(exe, "USER32.dll", "SendNotifyMessageA",
                                                           (void *)h_SendNotifyMessageA);
    LOG("text: front-end labels get the game's own text pointer (SetWindowTextA %s, SendMessageA %s, SendNotifyMessageA %s, "
        "CreateWindowExA %s for the helper windows)",
        o_SetWindowTextA ? "ok" : "MISSING", o_SendMessageA ? "ok" : "MISSING", o_SendNotifyMessageA ? "ok" : "MISSING",
        g_wt_class ? "ok" : "MISSING");
    if (!o_SetWindowTextA) o_SetWindowTextA = SetWindowTextA;
    if (!o_SendMessageA) o_SendMessageA = SendMessageA;
    if (!o_SendNotifyMessageA) o_SendNotifyMessageA = SendNotifyMessageA;
}

