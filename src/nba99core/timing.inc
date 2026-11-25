/*
 * Timing statistics (included by nba99core.cpp). See common/nba99_stats.h.
 *
 * - Game clock: every timer callback the game sets with timeSetEvent runs
 *   through our wrapper; the first one is counted as the game clock and the gap
 *   since the last call measured with QueryPerformanceCounter. (EA's timer
 *   server, 0x548020: a one-shot timeSetEvent re-armed from GetTickCount, as in
 *   NBA Live 98.)
 * - Simulation steps (0.3.0): normalgameloop (0x4382C0) calls singleailoop
 *   (0x437C80) once per simulation step. Its first 5 bytes (push ebx, ecx, esi,
 *   edi, ebp) become a jump to a thunk that counts the step, does the same five
 *   pushes and jumps back to 0x437C85. Counting only: no game state is written.
 * - Input frames (0.3.0): the game's input task ctrlinterrupt (0x40D050, timer
 *   task) counts ticks in 0x5FE4D8 and samples the controllers when it reaches
 *   2 (4 in an online game), resetting it to 0: the same scheme as NBA Live 98's
 *   0x42E4A0. It is registered at 0x40CECB (init_game_input) and removed at
 *   0x40CF00 (release_game_input) with "mov eax, 0x40D050"; both immediates
 *   point at our wrapper, which runs the task and counts a frame when 0x5FE4D8
 *   is 0 afterwards. Unlike 98, the task does not run inside the timer
 *   callback: EA's callback (0x547E50) only sets events (0x567470: SetEvent)
 *   and the tasks run on the game's task thread. So the wrapper also wakes
 *   glide2x's step lock itself (input_made): waking it only after the timer
 *   callback missed the input frame whenever the task thread ran later, and
 *   the frame waited a whole tick (pace log, 25 Nov 2026). Both counters are checked byte for byte first and left out
 *   if the exe differs.
 * - Precise clock ([nba99] PreciseClock=1, default since 0.3.0): the exe's
 *   GetTickCount import returns a millisecond clock from QueryPerformanceCounter
 *   and timer resolution is set to 1 ms. EA's timer server re-arms its one-shot
 *   timer from GetTickCount against a 16.16 deadline (0x6E4010), so with the
 *   coarse Windows clock its ticks came 1 to 21 ms apart (0.3.0 measurements);
 *   with this clock they are even, and glide2x's display lock can steer the
 *   clock speed (nba99core_set_clock_rate). As in NBA Live 98.
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


/* ---- simulation steps and input frames (0.3.0) ---- */
#define STEP_VA 0x00437C80u          /* singleailoop */
#define INPUT_TASK_VA 0x0040D050u     /* ctrlinterrupt */
#define INPUT_REG_VA 0x0040CECBu      /* init_game_input: mov eax, 0x40D050 */
#define INPUT_UNREG_VA 0x0040CF00u    /* release_game_input: mov eax, 0x40D050 */
#define INPUT_COUNT_VA 0x005FE4D8u    /* ticks since the last sample */
void step_thunk(void) __asm__("nba99_step_thunk");
__asm__(".text\n"
        "nba99_step_thunk:\n"
        "  lock incl nba99_g_stats+8\n"
        "  pushl %ebx\n"
        "  pushl %ecx\n"
        "  pushl %esi\n"
        "  pushl %edi\n"
        "  pushl %ebp\n"
        "  jmp *nba99_step_back\n");
uint32_t g_step_back __asm__("nba99_step_back") = STEP_VA + 5;

volatile long *g_input_frames_p __asm__("nba99_input_frames_p") = &g_stats.input_frames;

/* called by the input task wrapper (on the game's task thread) right after it counted an input
   frame: wake the step lock now, not at the next timer callback (see timing_after_timer) */
extern "C" void __cdecl input_made(void) __asm__("nba99_input_made");
extern "C" void __cdecl input_made(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    g_stats.input_qpc_lo = (unsigned long)q.LowPart;
    g_stats.input_tick = g_stats.ticks;
    if (g_stats.tick_event) SetEvent((HANDLE)g_stats.tick_event);
}
uint32_t g_input_task __asm__("nba99_input_task") = INPUT_TASK_VA;
__asm__(".text\n"
        "nba99_input_task_wrap:\n"
        "  call *nba99_input_task\n"
        "  pushfl\n"
        "  cmpl $0, 0x005FE4D8\n"
        "  jne 1f\n"
        "  pushl %eax\n"
        "  movl nba99_input_frames_p, %eax\n"
        "  lock incl (%eax)\n"
        "  popl %eax\n"
        "  pushal\n"
        "  call nba99_input_made\n"
        "  popal\n"
        "1:\n"
        "  popfl\n"
        "  ret\n");
void input_task_wrap(void) __asm__("nba99_input_task_wrap");

static int patch_code(uint32_t va, const uint8_t *expect, const uint8_t *with, int n)
{
    DWORD old;
    if (memcmp((const void *)(uintptr_t)va, expect, n)) return 0;
    if (!VirtualProtect((void *)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)va, with, n);
    VirtualProtect((void *)(uintptr_t)va, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)va, n);
    return 1;
}

static void counters_setup(void)
{
    static const uint8_t step_expect[5] = { 0x53, 0x51, 0x56, 0x57, 0x55 };   /* push ebx, ecx, esi, edi, ebp */
    static const uint8_t task_expect[5] = { 0xB8, 0x50, 0xD0, 0x40, 0x00 };   /* mov eax, 0x40D050 */
    uint8_t j[5], m[5];
    int32_t rel = (int32_t)((uintptr_t)step_thunk - (STEP_VA + 5));
    uint32_t w = (uint32_t)(uintptr_t)input_task_wrap;
    j[0] = 0xE9; memcpy(j + 1, &rel, 4);
    if (patch_code(STEP_VA, step_expect, j, 5)) LOG("timing: counting simulation steps (singleailoop 0x%08X)", STEP_VA);
    else LOG("timing: singleailoop code not as expected; simulation steps not counted");
    m[0] = 0xB8; memcpy(m + 1, &w, 4);
    if (!memcmp((const void *)(uintptr_t)INPUT_REG_VA, task_expect, 5) && !memcmp((const void *)(uintptr_t)INPUT_UNREG_VA, task_expect, 5) &&
        patch_code(INPUT_REG_VA, task_expect, m, 5) && patch_code(INPUT_UNREG_VA, task_expect, m, 5))
        LOG("timing: counting input frames (ctrlinterrupt 0x%08X wrapped)", INPUT_TASK_VA);
    else
        LOG("timing: input task registration not as expected; input frames not counted");
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
        tlog("steps: simulation steps per second (singleailoop); inp: input frames per second (ctrlinterrupt samples).");
        tlog("frames: frames shown, frame time (min avg max), frames that showed 0/1/2/3+ new steps, frames over 50 ms.");
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
                _snprintf(fr, sizeof fr, "frames %5.1f/s  frame ms %5.1f %5.1f %5.1f  steps/frame %ld/%ld/%ld/%ld  >50ms %ld",
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
    g_stats.tick_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    counters_setup();
    if (ini_int("PreciseClock", 1)) {
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
