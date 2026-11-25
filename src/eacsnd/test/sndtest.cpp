/*
 * sndtest (dist/Tools/eacsnd_test/sndtest.exe): checks our eacsnd.dll on its own.
 * Put it in the game folder next to eacsnd.dll and SDL3.dll and run it (console).
 * It plays the game's part (callbacks, a 100 Hz serve thread under a lock) and:
 *   1. checks the version, caps and start;
 *   2. plays a noise burst from the front, right, behind and left (3 s each), then
 *      circling clockwise twice: listen that each comes from where the text says;
 *   3. plays a short sound once and checks the game is told when it ends;
 *   4. counts the 100 Hz ticks against the sound played (should be 100 per second).
 * Settings come from nba99fix.ini [sound] as in the game (Mode3D, Device, ...).
 * Exit code 0 = all checks passed.
 */
#include "nba99_cxx.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

NBA99_C_BEGIN
typedef int (__stdcall *f0)(void);
typedef int (__stdcall *f1)(int);
typedef int (__stdcall *f2)(int, int);
typedef int (__stdcall *f3)(int, int, int);
typedef int (__stdcall *f7)(int, int, int, int, int, int, int);
typedef int (__stdcall *f8)(int, int, int, int, int, int, int, int);

static f0 dllversion, stop_, serve;
static f8 setfunctions;
static f1 caps, stopbuf;
static f2 start, vol;
static f3 pos3d;
static f7 create3dbuf, play3d;

static CRITICAL_SECTION g_cs;
static volatile LONG g_ticks, g_ended, g_quit;
static volatile LONG g_frames;
static int g_fails;

static int __stdcall cb_mixer(void *dst, int frames) { memset(dst, 0, (size_t)frames * 4); g_frames += frames; return 0; }
static int __stdcall cb_ended(int v) { (void)v; InterlockedIncrement(&g_ended); return 0; }
static int __cdecl cb_msg(const char *f, ...) { printf("  message from the DLL: %s\n", f); return 0; }
static int __stdcall cb_tick(void) { InterlockedIncrement(&g_ticks); return 0; }
/* our own position convention for this test: a = azimuth in degrees (+ = right), b unused */
static int __stdcall cb_position(int a, int b, float *xyz)
{
    double r = a * 3.14159265358979 / 180.0;
    (void)b;
    xyz[0] = (float)(sin(r) * 65536.0);
    xyz[1] = 0.0f;
    xyz[2] = (float)(cos(r) * 65536.0);
    return 0;
}
/* the "decoder": noise bursts (easy to place by ear), 0.15 s on, 0.1 s off */
static int __stdcall cb_decode(int src, int a2, int a1, int s, int e, void *dst, int bits, int z, int ch)
{
    int16_t *o = (int16_t *)dst;
    int n = e - s + 1, i;
    uint32_t r = 12345;
    (void)src; (void)a2; (void)a1; (void)bits; (void)z; (void)ch;
    for (i = 0; i < n; i++) {
        r = r * 1664525u + 1013904223u;
        o[i] = (i % 5512) < 3300 ? (int16_t)((int)(r >> 16) - 32768) / 3 : 0;
    }
    return 0;
}

static DWORD WINAPI server(LPVOID p)
{
    (void)p;
    while (!g_quit) {
        EnterCriticalSection(&g_cs);
        serve();
        LeaveCriticalSection(&g_cs);
        Sleep(10);
    }
    return 0;
}

#define CHECK(c, ...) do { if (c) printf("  ok: "); else { printf("  FAILED: "); g_fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static void place(int deg, const char *where, int ms)
{
    printf("  %s\n", where);
    EnterCriticalSection(&g_cs);
    pos3d(0, deg, 0);
    LeaveCriticalSection(&g_cs);
    Sleep(ms);
}

int main(void)
{
    HMODULE m;
    int r, id, id2, i;
    LONG t0, f0_;
    DWORD ms0;
    InitializeCriticalSection(&g_cs);
    m = LoadLibraryA("eacsnd.dll");
    if (!m) { printf("eacsnd.dll not found next to sndtest.exe (error %lu)\n", GetLastError()); return 2; }
#define GET(v, n) *(FARPROC *)&v = GetProcAddress(m, n); if (!v) { printf("eacsnd.dll has no %s (EA's DLL? this test is for ours)\n", n); return 2; }
    GET(dllversion, "_iSNDdllversion@0") GET(setfunctions, "_iSNDdirectsetfunctions@32") GET(caps, "_iSNDdirectcaps@4")
    GET(start, "_iSNDdirectstart@8") GET(stop_, "_iSNDdirectstop@0") GET(serve, "_iSNDdirectserve@0")
    GET(vol, "_iSNDdirectvol@8") GET(pos3d, "_iSNDdirectpos3d@12") GET(create3dbuf, "_iSNDdirectcreate3dbuf@28")
    GET(play3d, "_iSNDdirectplay3d@28") GET(stopbuf, "_iSNDdirectstopbuf@4")
    printf("NBA Live 99 sound test (our eacsnd.dll)\n\n1. Setup\n");
    CHECK(dllversion() == 0x00100003, "version 0x%08x", dllversion());
    setfunctions((int)(uintptr_t)cb_mixer, (int)(uintptr_t)cb_ended, (int)(uintptr_t)cb_msg, (int)(uintptr_t)cb_msg,
                 (int)(uintptr_t)cb_tick, (int)(uintptr_t)cb_msg, (int)(uintptr_t)cb_position, (int)(uintptr_t)cb_decode);
    r = caps(0);
    CHECK(r > 0 && (r & 0x20800) == 0x20800, "caps 0x%x (22050 Hz and 3D)", r);
    r = start(-1, 0);
    CHECK(r == 0, "start -> %d (see logs\\...\\eacsnd.log for the device)", r);
    if (r) return 1;
    CreateThread(NULL, 0, server, NULL, 0, NULL);

    printf("\n2. Direction: listen that the sound comes from where it says\n");
    id = create3dbuf(0, 0, 22050, 0, 0, 22049, 0x1234);
    CHECK(id > 0, "3D source loaded, id %d", id);
    EnterCriticalSection(&g_cs);
    r = play3d(id, 0, 1, 22050, 110, 0, 0);
    LeaveCriticalSection(&g_cs);
    CHECK(r == 0, "play3d (looping) -> %d", r);
    place(0, "front", 3000);
    place(90, "right", 3000);
    place(180, "behind (on two speakers: centre or slightly to one side)", 3000);
    place(-90, "left", 3000);
    place(0, "front again, then circling clockwise twice (front, right, behind, left)", 1500);
    for (i = 0; i <= 720; i += 4) { EnterCriticalSection(&g_cs); pos3d(0, i, 0); LeaveCriticalSection(&g_cs); Sleep(50); }
    EnterCriticalSection(&g_cs);
    g_ended = 0;
    stopbuf(0);
    LeaveCriticalSection(&g_cs);
    CHECK(g_ended == 1, "stopbuf told the game (voice_ended %ld)", g_ended);

    printf("\n3. A short sound played once\n");
    id2 = create3dbuf(0, 0, 11025, 0, 0, 2755, 0x5678);
    g_ended = 0;
    EnterCriticalSection(&g_cs);
    play3d(id2, 3, 0, 11025, 127, 0, 0);
    LeaveCriticalSection(&g_cs);
    Sleep(800);
    CHECK(g_ended == 1, "the game was told when it ended (voice_ended %ld)", g_ended);

    printf("\n4. Timing\n");
    t0 = g_ticks; f0_ = g_frames; ms0 = GetTickCount();
    Sleep(3000);
    {
        double secs = (g_frames - f0_) / 22050.0, rate = (g_ticks - t0) / secs;
        CHECK(rate > 98 && rate < 102, "%.1f ticks per second of sound (100 expected), %.2f s of sound in %.2f s",
              rate, secs, (GetTickCount() - ms0) / 1000.0);
    }
    g_quit = 1;
    Sleep(50);
    stop_();
    printf("\n%s\n", g_fails ? "Some checks FAILED (details above)" : "All checks passed.");
    printf("Press Enter to close.");
    getchar();
    return g_fails ? 1 : 0;
}
NBA99_C_END
