/*
 * Timing statistics (included by nba99core.cpp). See common/nba99_stats.h.
 *
 * 0.1 keeps only the parts of the NBA Live 98 timing code that need no game
 * addresses; frame pacing (stage 5) adds the rest once the 99 simulation step
 * and input task have been found.
 * - Game clock: every timer callback the game sets with timeSetEvent runs
 *   through our wrapper; the first one is counted as the game clock and the gap
 *   since the last call measured with QueryPerformanceCounter. (In 98 that was
 *   the 120 Hz clock; for 99 the timing log will tell.)
 * - Simulation steps are not counted yet (sim_steps stays 0), and game_v11 stays
 *   0 so glide2x never reads NBA Live 98 addresses in this game.
 * - Precise clock ([nba99] PreciseClock, default 0 in 0.1): the exe's
 *   GetTickCount import returns a millisecond clock from QueryPerformanceCounter
 *   and timer resolution is set to 1 ms. Off until stage 5 has measured the
 *   99 clock.
 * - A sampler thread runs the machine report, publishes per-second figures for
 *   the overlay and, with [nba99] TimingLog=1 (default in the Debug build),
 *   writes one line per second to logs\timing.log with glide2x's frame figures.
 */
#include "nba99_stats.h"
#include <stddef.h>

nba99_stats g_stats __asm__("nba99_g_stats") = { NBA99_STATS_VERSION, sizeof(nba99_stats) };
static_assert(offsetof(nba99_stats, sim_steps) == 8, "the step thunk uses offset 8");

__declspec(dllexport) nba99_stats *nba99core_stats(void) { return &g_stats; }

static LPTIMECALLBACK g_clock_cb;     /* the timer callback we measure */
static LARGE_INTEGER g_tick_last;
static double g_gap_min = 1e9, g_gap_max, g_gap_sum;
static long g_gap_n;
static HANDLE g_tlog = INVALID_HANDLE_VALUE;

/* precise GetTickCount for the game. The clock can run slightly faster or
   slower than real time (display lock, set by glide2x): it is kept as an
   anchor (QPC, ms, rate) in one of two slots; a rate change fills the other
   slot from the current reading and flips the index, so readers on other
   threads never see a half-written anchor and the clock never jumps. */
static DWORD g_tc_base;
typedef struct { LONGLONG q0; double ms0, rate; } clk_anchor;
static clk_anchor g_clk[2];
static volatile LONG g_clk_i;

static double clk_ms(const clk_anchor *a, LONGLONG q)
{
    return a->ms0 + (double)(q - a->q0) * 1000.0 / (double)g_qpf.QuadPart * a->rate;
}

static DWORD WINAPI h_GetTickCount(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return g_tc_base + (DWORD)(LONGLONG)clk_ms(&g_clk[g_clk_i], now.QuadPart);
}

__declspec(dllexport) void nba99core_set_clock_rate(double rate)
{
    LONG i = g_clk_i;
    LARGE_INTEGER now;
    if (!g_stats.precise_clock) return;
    if (rate < 0.97) rate = 0.97;
    if (rate > 1.03) rate = 1.03;
    QueryPerformanceCounter(&now);
    g_clk[i ^ 1].ms0 = clk_ms(&g_clk[i], now.QuadPart);
    g_clk[i ^ 1].q0 = now.QuadPart;
    g_clk[i ^ 1].rate = rate;
    InterlockedExchange(&g_clk_i, i ^ 1);
    g_stats.clock_rate = (float)rate;
}


/* called from timer_thunk on the timer thread, after the game's callback */
static void timing_after_timer(LPTIMECALLBACK cb)
{
    if (cb != g_clock_cb) return;
    InterlockedIncrement(&g_stats.ticks_done);
    if (g_stats.tick_event) SetEvent((HANDLE)g_stats.tick_event);
}

/* called from timer_thunk on the timer thread, before the game's callback */
static void timing_on_timer(LPTIMECALLBACK cb)
{
    LARGE_INTEGER now;
    if (!g_clock_cb) g_clock_cb = cb;
    if (cb != g_clock_cb) return;
    QueryPerformanceCounter(&now);
    InterlockedIncrement(&g_stats.ticks);
    if (g_tick_last.QuadPart) {
        double gap = (double)(now.QuadPart - g_tick_last.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        if (gap < g_gap_min) g_gap_min = gap;
        if (gap > g_gap_max) g_gap_max = gap;
        g_gap_sum += gap;
        g_gap_n++;
    }
    g_tick_last = now;
}

static void tlog(const char *fmt, ...)
{
    char buf[512];
    int n;
    va_list ap;
    DWORD w;
    if (g_tlog == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_tlog, buf, (DWORD)n, &w, NULL);
}

static DWORD WINAPI timing_thread(LPVOID arg)
{
    LARGE_INTEGER t0, t1;
    long ticks0 = g_stats.ticks, steps0 = g_stats.sim_steps, inf0 = g_stats.input_frames, r_seq_logged = 0, quiet = 0;
    (void)arg;
    sys_report();   /* machine report: here, not in DllMain (it loads dxgi.dll) */
    if (g_tlog != INVALID_HANDLE_VALUE) {
        tlog("NBA Live 99 timing log, %s. One line per second.", NBA99_VERSION_LINE);
        tlog("machine: %s", g_machine);
        tlog("ticks: calls of the game's first timer callback and the gap between them (min avg max).");
        tlog("steps and inp: not counted yet in NBA Live 99 (always 0).");
        tlog("frames: frames shown, frame time (min avg max), frames over 50 ms (steps per frame not known yet).");
    }
    QueryPerformanceCounter(&t0);
    for (;;) {
        double len, gmin, gmax, gsum;
        long gn, ticks, steps, inf;
        Sleep(1000);
        QueryPerformanceCounter(&t1);
        len = (double)(t1.QuadPart - t0.QuadPart) / (double)g_qpf.QuadPart;
        t0 = t1;
        /* the timer thread is the only writer; a sample torn by one tick does not matter here */
        gmin = g_gap_min; gmax = g_gap_max; gsum = g_gap_sum; gn = g_gap_n;
        g_gap_min = 1e9; g_gap_max = 0; g_gap_sum = 0; g_gap_n = 0;
        ticks = g_stats.ticks; steps = g_stats.sim_steps;
        g_stats.sec_len_s = (float)len;
        g_stats.sec_ticks = (float)((ticks - ticks0) / len);
        g_stats.sec_steps = (float)((steps - steps0) / len);
        g_stats.sec_gap_min = gn ? (float)gmin : 0;
        g_stats.sec_gap_avg = gn ? (float)(gsum / gn) : 0;
        g_stats.sec_gap_max = gn ? (float)gmax : 0;
        InterlockedIncrement(&g_stats.sec_seq);
        ticks0 = ticks; steps0 = steps;
        inf = g_stats.input_frames;
        if (g_tlog != INVALID_HANDLE_VALUE) {
            char fr[400] = "frames: no data";
            /* glide2x publishes on its own one-second windows, which drift against ours: a
               second without a new publish is only a stall if the one before had none either */
            if (g_stats.r_seq != r_seq_logged || (g_stats.r_seq && !quiet++)) {
                if (g_stats.r_seq != r_seq_logged) quiet = 0;
                r_seq_logged = g_stats.r_seq;
                _snprintf(fr, sizeof fr, "frames %5.1f/s  frame ms %5.1f %5.1f %5.1f  steps/frame 0:%-3ld 1:%-3ld 2:%-3ld 3+:%-3ld  >50ms %ld",
                          g_stats.r_fps, g_stats.r_ft_min, g_stats.r_ft_avg, g_stats.r_ft_max, g_stats.r_spf[0],
                          g_stats.r_spf[1], g_stats.r_spf[2], g_stats.r_spf[3], g_stats.r_long);
            } else if (g_stats.r_seq) {
                _snprintf(fr, sizeof fr, "frames: none this second (stall or loading)");
            }
            if (g_stats.disp_lock) {
                size_t l = strlen(fr);
                _snprintf(fr + l, sizeof fr - l, "  |  display %.3f Hz, present at %.2f %.2f %.2f of refresh, clock x%.5f",
                          g_stats.disp_hz, g_stats.r_ph_min, g_stats.r_ph_avg, g_stats.r_ph_max, g_stats.clock_rate);
            }
            tlog("%8.1f s  ticks %5.1f/s  gap ms %5.1f %5.1f %5.1f  steps %5.1f/s  inp %5.1f/s  |  %s", now_ms() / 1000.0,
                 g_stats.sec_ticks, g_stats.sec_gap_min, g_stats.sec_gap_avg, g_stats.sec_gap_max, g_stats.sec_steps,
                 (inf - inf0) / len, fr);
        }
        inf0 = inf;
    }
    return 0;
}

static void timing_setup(void)
{
    HANDLE h;
    g_stats.crt_mode = -1;   /* set by the options menu when the CRT filter is changed */
    g_stats.tick_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (ini_int("PreciseClock", 0)) {
        LARGE_INTEGER q;
        g_tc_base = GetTickCount();
        QueryPerformanceCounter(&q);
        g_clk[0].q0 = q.QuadPart;
        g_clk[0].ms0 = 0;
        g_clk[0].rate = 1.0;
        g_stats.clock_rate = 1.0f;
        if (iat_hook(GetModuleHandleA(NULL), "KERNEL32.dll", "GetTickCount", (void *)h_GetTickCount)) {
            g_stats.precise_clock = 1;
            timeBeginPeriod(1);
            LOG("timing: precise clock on (game GetTickCount from QueryPerformanceCounter, timer resolution 1 ms)");
        } else {
            LOG("timing: could not hook GetTickCount; game clock left as is");
        }
    }
    if (ini_int("TimingLog", NBA99_DEBUG)) {
        char path[MAX_PATH];
        nba99_log_path(g_exe_dir, "timing.log", path, sizeof path);
        g_tlog = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    h = o_CreateThread(NULL, 0, timing_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
