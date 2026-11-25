/*
 * eacsnd_log (eacsnd_log.dll): Debug tool. Sits between NBAWIN.EXE and EA's
 * sound DLL (eacsnd.dll) and passes every call on unchanged.
 *
 * NBA Live 99 plays all its sound through eacsnd.dll: the exe loads it with
 * LoadLibraryA("eacsnd.dll") at 0x56AD31 and calls its 16 stdcall exports
 * (_iSNDdllversion@0 ... _iSNDdirectstopbuf@4). EA's DLL drives DirectSound:
 * one streaming buffer filled from the game's own mixer (serve), plus up to 16
 * DirectSound 3D voices made from sample sources (create3dbuf, play3d).
 * It calls back into the game through 8 function pointers given to
 * _iSNDdirectsetfunctions@32:
 *   1 mixer(dst, frames)          fills the stream
 *   2 voice_ended(voice)          a 3D voice has stopped
 *   3, 4, 6 message(text)         error and debug text (cdecl)
 *   5 tick()                      the game's 100 Hz sound tick
 *   7 position(a, b, float *xyz)  3D position of a voice
 *   8 decode(9 arguments)         writes a sample source into a new buffer
 * This proxy exports the same 16 names and ordinals, loads EA's DLL by full path
 * and logs to eacsnd_log.txt in the session's log folder:
 *   - every call to the setup exports (dllversion, setfunctions, caps, start,
 *     stop, the record functions) and to the 3D exports, with arguments,
 *     result, the exe address that called and the thread (the 3D exports and
 *     callbacks are logged in full up to [eacsndlog] First= calls each);
 *   - every call to callbacks 2, 3, 4, 6, 7 and 8 with arguments (7 with the
 *     position it returned);
 *   - one TICK line per second: serve calls and the gap between them
 *     (min avg max ms), mixer calls and frames, tick calls, 3D calls;
 *   - call counts at exit.
 *
 * Selected by the core: [nba99] EacsndLog=1 in nba99fix.ini (Debug build) makes
 * the exe's LoadLibraryA("eacsnd.dll") load eacsnd_log.dll from the game folder
 * instead. Optional, [eacsndlog] in nba99fix.ini:
 *   Real=             sound DLL in the game folder to pass the calls on to (default
 *                     eacsnd_ea.dll, EA's DLL kept under that name once our eacsnd.dll
 *                     is installed; eacsnd.dll if there is no eacsnd_ea.dll)
 *   First=400         calls logged in full per 3D export and callback
 *
 * C++ in the restricted subset like the rest of the Debug build (README "C++ rules").
 * Windows 98 imports only (no GetModuleHandleEx: callers are named with VirtualQuery).
 */
#include "nba99_cxx.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "nba99_build.h"

NBA99_C_BEGIN

/* ------------------------------------------------------------------------- */
/* log                                                                       */
/* ------------------------------------------------------------------------- */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;
static char g_buf[1 << 16];
static int g_buf_n;
static LARGE_INTEGER g_qpf, g_qpc0;
static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_real_path[MAX_PATH];
static int g_first = 400;
static volatile LONG g_stop;
static HANDLE g_ticker;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void flush_locked(void)
{
    DWORD w;
    if (g_buf_n && g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, g_buf, (DWORD)g_buf_n, &w, NULL);
    g_buf_n = 0;
}

static void flush(void)
{
    EnterCriticalSection(&g_cs);
    flush_locked();
    LeaveCriticalSection(&g_cs);
}

static void log_line(const char *fmt, ...)
{
    char line[1024];
    int n;
    va_list ap;
    if (g_log == INVALID_HANDLE_VALUE) return;
    n = _snprintf(line, 32, "[%10.1f] ", now_ms());
    va_start(ap, fmt);
    n += _vsnprintf(line + n, sizeof line - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    EnterCriticalSection(&g_cs);
    if (g_buf_n + n > (int)sizeof g_buf) flush_locked();
    memcpy(g_buf + g_buf_n, line, n);
    g_buf_n += n;
    LeaveCriticalSection(&g_cs);
}

/* "EXE!0x0056b24f" for the game exe, "name.dll+0x1234" for other modules (VirtualQuery: Windows 98) */
static void where(uint32_t a, char *out, int cap)
{
    MEMORY_BASIC_INFORMATION mb;
    char path[MAX_PATH], *b;
    HMODULE m;
    if (!VirtualQuery((LPCVOID)(uintptr_t)a, &mb, sizeof mb) || !mb.AllocationBase ||
        !GetModuleFileNameA((HMODULE)mb.AllocationBase, path, sizeof path)) {
        _snprintf(out, cap, "0x%08lx", (unsigned long)a);
        return;
    }
    m = (HMODULE)mb.AllocationBase;
    if (m == GetModuleHandleA(NULL)) { _snprintf(out, cap, "EXE!0x%08lx", (unsigned long)a); return; }
    b = strrchr(path, '\\');
    _snprintf(out, cap, "%s+0x%lx", b ? b + 1 : path, (unsigned long)(a - (uint32_t)(uintptr_t)m));
}

#define CALLER(buf) where((uint32_t)(uintptr_t)__builtin_return_address(0), buf, sizeof buf)

/* ------------------------------------------------------------------------- */
/* counters                                                                  */
/* ------------------------------------------------------------------------- */
enum {
    C_SERVE, C_VOL, C_POS3D, C_RATE, C_CREATE3D, C_REMOVE, C_PLAY3D, C_STOPBUF,
    C_MIXER, C_ENDED, C_MSG, C_TICK, C_POSITION, C_DECODE, C_COUNT
};
static const char *const g_cname[C_COUNT] = {
    "serve", "vol", "pos3d", "rate", "create3dbuf", "removebuf", "play3d", "stopbuf",
    "cb_mixer", "cb_voice_ended", "cb_message", "cb_tick", "cb_position", "cb_decode"
};
static volatile LONG g_count[C_COUNT], g_sec[C_COUNT];
static volatile LONG g_mix_frames_sec;
static LONG g_mix_frames_min = 0x7fffffff, g_mix_frames_max;
static double g_serve_last, g_serve_min = 1e9, g_serve_max, g_serve_sum;
static long g_serve_n;

/* counts the call; 1 if it is one of the first First= calls (log it in full) */
static int count(int c)
{
    LONG n = InterlockedIncrement(&g_count[c]);
    InterlockedIncrement(&g_sec[c]);
    return n <= g_first;
}

/* ------------------------------------------------------------------------- */
/* EA's DLL                                                                  */
/* ------------------------------------------------------------------------- */
typedef int (__stdcall *fn0)(void);
typedef int (__stdcall *fn1)(int);
typedef int (__stdcall *fn2)(int, int);
typedef int (__stdcall *fn3)(int, int, int);
typedef int (__stdcall *fn4)(int, int, int, int);
typedef int (__stdcall *fn7)(int, int, int, int, int, int, int);
typedef int (__stdcall *fn8)(int, int, int, int, int, int, int, int);
typedef int (__stdcall *fn9)(int, int, int, int, int, int, int, int, int);
typedef int (__cdecl *fnmsg)(const char *);

static HMODULE g_real;
static struct {
    fn0 dllversion; fn8 setfunctions; fn1 caps; fn2 start; fn0 stop;
    fn4 recordstart; fn1 recordpacket; fn0 recordstop; fn0 serve;
    fn2 vol; fn3 pos3d; fn2 rate; fn7 create3dbuf; fn1 removebuf; fn7 play3d; fn1 stopbuf;
} R;

static int load_real(void)
{
    if (g_real) return 1;
    g_real = LoadLibraryA(g_real_path);
    if (!g_real) {
        log_line("FATAL: could not load the real sound DLL %s (error %lu)", g_real_path, GetLastError());
        flush();
        MessageBoxA(NULL, "eacsnd_log could not load the real sound DLL (eacsnd_ea.dll or eacsnd.dll).\n"
                          "See eacsnd_log.txt in the logs folder.", "NBA Live 99 eacsnd_log", MB_ICONERROR);
        ExitProcess(2);
    }
    log_line("real sound DLL: %s loaded at 0x%08lx", g_real_path, (unsigned long)(uintptr_t)g_real);
#define GET(f, n) do { *(FARPROC *)&R.f = GetProcAddress(g_real, n); \
                       if (!R.f) log_line("WARNING: real DLL has no %s", n); } while (0)
    GET(dllversion, "_iSNDdllversion@0");
    GET(setfunctions, "_iSNDdirectsetfunctions@32");
    GET(caps, "_iSNDdirectcaps@4");
    GET(start, "_iSNDdirectstart@8");
    GET(stop, "_iSNDdirectstop@0");
    GET(recordstart, "_iSNDdirectrecordstart@16");
    GET(recordpacket, "_iSNDdirectrecordpacket@4");
    GET(recordstop, "_iSNDdirectrecordstop@0");
    GET(serve, "_iSNDdirectserve@0");
    GET(vol, "_iSNDdirectvol@8");
    GET(pos3d, "_iSNDdirectpos3d@12");
    GET(rate, "_iSNDdirectrate@8");
    GET(create3dbuf, "_iSNDdirectcreate3dbuf@28");
    GET(removebuf, "_iSNDdirectremovebuf@4");
    GET(play3d, "_iSNDdirectplay3d@28");
    GET(stopbuf, "_iSNDdirectstopbuf@4");
#undef GET
    flush();
    return 1;
}

/* a missing real function: log once per call and answer "failed" */
#define NEED(f) do { load_real(); if (!R.f) { log_line("%s: not in the real DLL, returning -1", #f); return -1; } } while (0)

/* ------------------------------------------------------------------------- */
/* the game's callbacks, wrapped                                             */
/* ------------------------------------------------------------------------- */
static fn2 g_cb_mixer;
static fn1 g_cb_ended;
static fnmsg g_cb_msg3, g_cb_msg4, g_cb_msg6;
static fn0 g_cb_tick;
static fn3 g_cb_position;
static fn9 g_cb_decode;

static int __stdcall w_mixer(int dst, int frames)
{
    LONG f = frames;
    int full = count(C_MIXER) && g_count[C_MIXER] <= 20;
    InterlockedExchangeAdd(&g_mix_frames_sec, f);
    if (f < g_mix_frames_min) g_mix_frames_min = f;
    if (f > g_mix_frames_max) g_mix_frames_max = f;
    if (full) log_line("cb_mixer(dst 0x%08x, frames %d)  tid %lu", dst, frames, GetCurrentThreadId());
    return g_cb_mixer ? g_cb_mixer(dst, frames) : 0;
}

static int __stdcall w_ended(int voice)
{
    int r;
    int full = count(C_ENDED);
    r = g_cb_ended ? g_cb_ended(voice) : 0;
    if (full) log_line("cb_voice_ended(voice %d) -> %d  tid %lu", voice, r, GetCurrentThreadId());
    return r;
}

static void msg(int slot, fnmsg f, const char *s)
{
    count(C_MSG);
    log_line("cb_message%d: \"%s\"", slot, s ? s : "(null)");
    if (f) f(s);
}
static int __cdecl w_msg3(const char *s) { msg(3, g_cb_msg3, s); return 0; }
static int __cdecl w_msg4(const char *s) { msg(4, g_cb_msg4, s); return 0; }
static int __cdecl w_msg6(const char *s) { msg(6, g_cb_msg6, s); return 0; }

static int __stdcall w_tick(void)
{
    count(C_TICK);
    return g_cb_tick ? g_cb_tick() : 0;
}

static int __stdcall w_position(int a, int b, int out)
{
    int r = g_cb_position ? g_cb_position(a, b, out) : 0;
    if (count(C_POSITION)) {
        float *v = (float *)(uintptr_t)out;
        if (v && !IsBadReadPtr(v, 12))
            log_line("cb_position(%d, %d) -> %d  xyz %.2f %.2f %.2f", a, b, r, v[0], v[1], v[2]);
        else
            log_line("cb_position(%d, %d, 0x%08x) -> %d", a, b, out, r);
    }
    return r;
}

static int __stdcall w_decode(int a1, int a2, int a3, int a4, int a5, int dst, int bits, int a8, int channels)
{
    int r = g_cb_decode ? g_cb_decode(a1, a2, a3, a4, a5, dst, bits, a8, channels) : 0;
    if (count(C_DECODE))
        log_line("cb_decode(src 0x%08x, %d, %d, %d, %d, dst 0x%08x, bits %d, %d, channels %d) -> %d",
                 a1, a2, a3, a4, a5, dst, bits, a8, channels, r);
    return r;
}

/* ------------------------------------------------------------------------- */
/* exports                                                                   */
/* ------------------------------------------------------------------------- */
#define TID GetCurrentThreadId()

int __stdcall L_dllversion(void)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(dllversion);
    r = R.dllversion();
    log_line("dllversion() -> 0x%08x  from %s  tid %lu", r, at, TID);
    flush();
    return r;
}

int __stdcall L_setfunctions(int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(setfunctions);
    log_line("setfunctions(mixer 0x%08x, voice_ended 0x%08x, msg3 0x%08x, msg4 0x%08x, tick 0x%08x, "
             "msg6 0x%08x, position 0x%08x, decode 0x%08x)  from %s  tid %lu", p1, p2, p3, p4, p5, p6, p7, p8, at, TID);
    g_cb_mixer = (fn2)(uintptr_t)p1;
    g_cb_ended = (fn1)(uintptr_t)p2;
    g_cb_msg3 = (fnmsg)(uintptr_t)p3;
    g_cb_msg4 = (fnmsg)(uintptr_t)p4;
    g_cb_tick = (fn0)(uintptr_t)p5;
    g_cb_msg6 = (fnmsg)(uintptr_t)p6;
    g_cb_position = (fn3)(uintptr_t)p7;
    g_cb_decode = (fn9)(uintptr_t)p8;
    /* a null pointer stays null: EA's DLL checks for it before calling */
    r = R.setfunctions(p1 ? (int)(uintptr_t)w_mixer : 0, p2 ? (int)(uintptr_t)w_ended : 0,
                       p3 ? (int)(uintptr_t)w_msg3 : 0, p4 ? (int)(uintptr_t)w_msg4 : 0,
                       p5 ? (int)(uintptr_t)w_tick : 0, p6 ? (int)(uintptr_t)w_msg6 : 0,
                       p7 ? (int)(uintptr_t)w_position : 0, p8 ? (int)(uintptr_t)w_decode : 0);
    log_line("setfunctions -> %d", r);
    flush();
    return r;
}

/* caps bits as EA's DLL sets them (from its format probe; 0x800 = 22050 Hz) */
int __stdcall L_caps(int a)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(caps);
    log_line("caps(0x%08x)  from %s  tid %lu", a, at, TID);
    r = R.caps(a);
    log_line("caps -> 0x%08x", r);
    flush();
    return r;
}

int __stdcall L_start(int a, int b)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(start);
    log_line("start(0x%08x, 0x%08x)  from %s  tid %lu", a, b, at, TID);
    r = R.start(a, b);
    log_line("start -> %d (0x%08x)", r, r);
    flush();
    return r;
}

int __stdcall L_stop(void)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(stop);
    r = R.stop();
    log_line("stop() -> %d  from %s  tid %lu", r, at, TID);
    flush();
    return r;
}

int __stdcall L_recordstart(int a, int b, int c, int d)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(recordstart);
    r = R.recordstart(a, b, c, d);
    log_line("recordstart(0x%08x, 0x%08x, 0x%08x, 0x%08x) -> %d  from %s  tid %lu", a, b, c, d, r, at, TID);
    return r;
}

int __stdcall L_recordpacket(int a)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(recordpacket);
    r = R.recordpacket(a);
    log_line("recordpacket(0x%08x) -> %d  from %s  tid %lu", a, r, at, TID);
    return r;
}

int __stdcall L_recordstop(void)
{
    char at[96];
    int r;
    CALLER(at);
    NEED(recordstop);
    r = R.recordstop();
    log_line("recordstop() -> %d  from %s  tid %lu", r, at, TID);
    return r;
}

int __stdcall L_serve(void)
{
    double t = now_ms();
    NEED(serve);
    count(C_SERVE);
    if (g_serve_last > 0) {
        double d = t - g_serve_last;
        if (d < g_serve_min) g_serve_min = d;
        if (d > g_serve_max) g_serve_max = d;
        g_serve_sum += d;
        g_serve_n++;
    }
    g_serve_last = t;
    if (g_count[C_SERVE] == 1) {
        char at[96];
        CALLER(at);
        log_line("serve: first call from %s  tid %lu", at, TID);
    }
    return R.serve();
}

int __stdcall L_vol(int voice, int v)
{
    char at[96];
    int r;
    NEED(vol);
    r = R.vol(voice, v);
    if (count(C_VOL)) { CALLER(at); log_line("vol(voice %d, %d) -> %d  from %s", voice, v, r, at); }
    return r;
}

int __stdcall L_pos3d(int voice, int a, int b)
{
    char at[96];
    int r;
    NEED(pos3d);
    r = R.pos3d(voice, a, b);
    if (count(C_POS3D)) { CALLER(at); log_line("pos3d(voice %d, %d, %d) -> %d  from %s", voice, a, b, r, at); }
    return r;
}

int __stdcall L_rate(int voice, int hz)
{
    char at[96];
    int r;
    NEED(rate);
    r = R.rate(voice, hz);
    if (count(C_RATE)) { CALLER(at); log_line("rate(voice %d, %d) -> %d  from %s", voice, hz, r, at); }
    return r;
}

/* a1, a2 bytes, a3 sample rate (word), a4 length in frames, a5..a6 range, a7 source (passed to cb_decode) */
int __stdcall L_create3dbuf(int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    char at[96];
    int r, full;
    NEED(create3dbuf);
    full = count(C_CREATE3D);
    if (full) {
        CALLER(at);
        log_line("create3dbuf(%d, %d, rate %d, len %d, %d, %d, src 0x%08x)  from %s  tid %lu",
                 a1 & 0xff, a2 & 0xff, a3 & 0xffff, a4, a5, a6, a7, at, TID);
    }
    r = R.create3dbuf(a1, a2, a3, a4, a5, a6, a7);
    if (full) log_line("create3dbuf -> %d", r);
    return r;
}

int __stdcall L_removebuf(int src)
{
    char at[96];
    int r;
    NEED(removebuf);
    r = R.removebuf(src);
    if (count(C_REMOVE)) { CALLER(at); log_line("removebuf(%d) -> %d  from %s", src, r, at); }
    return r;
}

/* play3d(source, voice, loop, rate, vol, posA, posB) */
int __stdcall L_play3d(int src, int voice, int loop, int hz, int v, int pa, int pb)
{
    char at[96];
    int r, full;
    NEED(play3d);
    full = count(C_PLAY3D);
    if (full) {
        CALLER(at);
        log_line("play3d(src %d, voice %d, loop %d, rate %d, vol %d, pos %d %d)  from %s  tid %lu",
                 src, voice, loop, hz, v, pa, pb, at, TID);
    }
    r = R.play3d(src, voice, loop, hz, v, pa, pb);
    if (full) log_line("play3d -> %d", r);
    return r;
}

int __stdcall L_stopbuf(int voice)
{
    char at[96];
    int r;
    NEED(stopbuf);
    r = R.stopbuf(voice);
    if (count(C_STOPBUF)) { CALLER(at); log_line("stopbuf(voice %d) -> %d  from %s  tid %lu", voice, r, at, TID); }
    return r;
}

/* ------------------------------------------------------------------------- */
/* per-second summary                                                        */
/* ------------------------------------------------------------------------- */
static DWORD WINAPI ticker(LPVOID arg)
{
    (void)arg;
    while (!g_stop) {
        int i, n, any = 0;
        char line[1024];
        Sleep(1000);
        n = _snprintf(line, sizeof line, "TICK");
        for (i = 0; i < C_COUNT; i++) {
            LONG c = InterlockedExchange(&g_sec[i], 0);
            if (c && n < (int)sizeof line - 48) {
                n += _snprintf(line + n, sizeof line - n, " %s=%ld", g_cname[i], (long)c);
                any = 1;
            }
        }
        {
            LONG f = InterlockedExchange(&g_mix_frames_sec, 0);
            if (f && n < (int)sizeof line - 80) {
                n += _snprintf(line + n, sizeof line - n, "  | mixer frames %ld (per call %ld..%ld)",
                               (long)f, (long)g_mix_frames_min, (long)g_mix_frames_max);
                g_mix_frames_min = 0x7fffffff; g_mix_frames_max = 0;
            }
        }
        if (g_serve_n && n < (int)sizeof line - 80) {
            n += _snprintf(line + n, sizeof line - n, "  | serve gap ms %.1f %.1f %.1f",
                           g_serve_min, g_serve_sum / g_serve_n, g_serve_max);
            g_serve_min = 1e9; g_serve_max = 0; g_serve_sum = 0; g_serve_n = 0;
        }
        if (any) log_line("%s", line);
        flush();
    }
    return 0;
}

static void start(HINSTANCE self)
{
    char path[MAX_PATH], name[64], me[MAX_PATH], *s;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    InitializeCriticalSection(&g_cs);
    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);       /* game folder */
    s = strrchr(g_dir, '\\');
    if (s) s[1] = 0;
    _snprintf(g_ini, sizeof g_ini, "%snba99fix.ini", g_dir);
    nba99_log_path(g_dir, "eacsnd_log.txt", path, sizeof path);
    g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_first = (int)GetPrivateProfileIntA("eacsndlog", "First", 400, g_ini);
    /* our release replaces eacsnd.dll, so EA's own copy is kept as eacsnd_ea.dll; without
       that file (and without Real=) the calls go to eacsnd.dll, whichever DLL that is */
    GetPrivateProfileStringA("eacsndlog", "Real", "", name, sizeof name, g_ini);
    if (!name[0]) {
        _snprintf(g_real_path, sizeof g_real_path, "%seacsnd_ea.dll", g_dir);
        strcpy(name, GetFileAttributesA(g_real_path) != INVALID_FILE_ATTRIBUTES ? "eacsnd_ea.dll" : "eacsnd.dll");
    }
    _snprintf(g_real_path, sizeof g_real_path, "%s%s", g_dir, name);
    GetModuleFileNameA(self, me, sizeof me);
    log_line("eacsnd_log %s (sound DLL call log), game folder %s", NBA99_VERSION_LINE, g_dir);
    log_line("this proxy: %s; real DLL: %s (loaded on the first call); First=%d", me, g_real_path, g_first);
    flush();
    g_ticker = CreateThread(NULL, 0, ticker, NULL, 0, NULL);
}

static void stop(void)
{
    int i;
    char line[1024];
    int n = _snprintf(line, sizeof line, "call counts at exit:");
    g_stop = 1;
    for (i = 0; i < C_COUNT; i++)
        if (g_count[i] && n < (int)sizeof line - 48)
            n += _snprintf(line + n, sizeof line - n, " %s=%ld", g_cname[i], (long)g_count[i]);
    log_line("%s", line);
    EnterCriticalSection(&g_cs);
    flush_locked();
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    g_log = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_cs);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        start(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        stop();
    }
    return TRUE;
}

NBA99_C_END
