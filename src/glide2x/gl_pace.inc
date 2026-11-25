/*
 * Pace log (0.3.0, Debug; [glide] PaceLog=1, default in the Debug build). Included by gl_d3d.cpp.
 *
 * With the frame pacing locks on (25 Nov 2026) every match frame shows exactly one step, but the
 * time between presents ranges from about 7 to 26 ms (one and three game ticks) and the present
 * lands anywhere in the refresh interval (0.0 to 1.0) instead of near 0.5. This log shows, for
 * every frame of a match, where the time between two presents went, to find which part moves:
 *   - work:  end of our last present to the game's next grBufferSwap (the game's step and drawing,
 *            including our draw calls);
 *   - flush: our batch flush at the swap;
 *   - wait:  the step lock's wait, and how it ended (w = waited for the input frame, r = the frame
 *            was already there, l = drawing ran late: no wait, t = 50 ms limit);
 *   - age:   how long before the lock let go the newest input frame was made (start of the game
 *            timer callback that made it, from nba99core); itk: game ticks between the last two
 *            input frames (2 is normal);
 *   - comp:  composing the back buffer (CRT pass, overlay, display lock);
 *   - pres:  the Present call;
 *   - ph:    where the present fell in the refresh interval (0 = at a refresh, display lock).
 * Written to logs\<session>\pace.log: one summary line per second while the step lock runs, and
 * one line per uneven frame (time between presents outside 13 to 20.5 ms, present within 0.1 of a
 * refresh, or not exactly one new step), at most 40 a second. Times are glide2x's clock.
 */
static struct {
    int on;
    HANDLE log;
    double t_in, t_fl, t_lk, t_p0, t_p1, last_p1, sec_start;
    double age;
    int how;                    /* step lock: 'w', 'r', 'l', 't' */
    long prev_steps, prev_inf, prev_itick, itk;
    /* this second */
    long n, lines, ft_hist[4], ph_hist[10], how_n[4], itk_n[4], steps_bad;
    double ft_min, ft_max, ft_sum, work_max, comp_max, pres_max, age_min, age_max, age_sum;
} pc;

static double dl_last_ph = -1;   /* display_lock: phase of the last present (-1 = not measured) */

static void pc_write(const char *fmt, ...)
{
    char b[600];
    int n;
    DWORD w;
    va_list ap;
    va_start(ap, fmt);
    n = _vsnprintf(b, sizeof b - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof b - 3) n = (int)sizeof b - 3;
    b[n++] = '\r'; b[n++] = '\n';
    WriteFile(pc.log, b, (DWORD)n, &w, NULL);
}

static void pc_open(void)
{
    char path[MAX_PATH], dir[MAX_PATH], *s;
    static int tried;
    if (tried++) return;
    if (!NBA99_DEBUG || !G.cfg_pace_log) return;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    s = strrchr(dir, '\\');
    if (s) s[1] = 0;
    nba99_log_path(dir, "pace.log", path, sizeof path);
    pc.log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pc.log == INVALID_HANDLE_VALUE) { pc.log = NULL; return; }
    pc.on = 1;
    pc_write("NBA Live 99 pace log, %s. Match frames only (step lock running); times in ms.", NBA99_VERSION_LINE);
    pc_write("frame lines: time, ft (since the last present), work (game), flush, wait+how (w waited, r ready, l late, t limit),");
    pc_write("  age (newest input frame when the lock let go), itk (ticks between the last two input frames), comp, pres, ph, steps");
    pc_write("second lines: frames, ft min/avg/max and how many <13 / 13-20.5 / >20.5 / >50, ph in tenths (0.0-0.1 ... 0.9-1.0),");
    pc_write("  lock ends w/r/l/t, itk 1/2/3/4+, age min/avg/max, max work/comp/pres, frames not showing one step");
    glog("pace log: %s", path);
}

static void pc_reset_second(double now)
{
    memset(pc.ft_hist, 0, sizeof pc.ft_hist); memset(pc.ph_hist, 0, sizeof pc.ph_hist);
    memset(pc.how_n, 0, sizeof pc.how_n); memset(pc.itk_n, 0, sizeof pc.itk_n);
    pc.n = pc.lines = pc.steps_bad = 0;
    pc.ft_min = 1e9; pc.ft_max = pc.ft_sum = pc.work_max = pc.comp_max = pc.pres_max = 0;
    pc.age_min = 1e9; pc.age_max = pc.age_sum = 0;
    pc.sec_start = now;
}

/* called by step_lock() after it let go (only while it runs) */
static void pc_lock_done(int how)
{
    nba99_stats *st = core_stats();
    LARGE_INTEGER q;
    if (!pc.on || !st) return;
    pc.how = how;
    QueryPerformanceCounter(&q);
    pc.age = (double)(unsigned long)((unsigned long)q.LowPart - st->input_qpc_lo) * 1000.0 / (double)G.qpf_hz;
    if (st->input_tick != pc.prev_itick) {
        pc.itk = pc.prev_itick ? st->input_tick - pc.prev_itick : 2;
        pc.prev_itick = st->input_tick;
    }
}

/* called at the end of d3d_swap */
static void pc_frame(void)
{
    nba99_stats *st = core_stats();
    double ft, work, now = pc.t_p1;
    long steps, k;
    int bad, hi;
    if (!pc.on || !st) return;
    if (!G.step_lock_on) {   /* menus, pauses: close the second, keep nothing */
        pc.last_p1 = 0;
        pc.prev_steps = st->sim_steps;
        pc_reset_second(now);
        return;
    }
    if (!pc.last_p1) {       /* first match frame after a break */
        pc.last_p1 = now;
        pc.prev_steps = st->sim_steps;
        pc_reset_second(now);
        return;
    }
    ft = now - pc.last_p1;
    work = pc.t_in - pc.last_p1;
    steps = st->sim_steps - pc.prev_steps;
    pc.prev_steps = st->sim_steps;
    pc.last_p1 = now;

    pc.n++;
    pc.ft_sum += ft;
    if (ft < pc.ft_min) pc.ft_min = ft;
    if (ft > pc.ft_max) pc.ft_max = ft;
    pc.ft_hist[ft < 13.0 ? 0 : ft <= 20.5 ? 1 : 2]++;
    if (ft > 50.0) pc.ft_hist[3]++;
    if (dl_last_ph >= 0) { k = (long)(dl_last_ph * 10.0); if (k > 9) k = 9; if (k < 0) k = 0; pc.ph_hist[k]++; }
    pc.how_n[pc.how == 'w' ? 0 : pc.how == 'r' ? 1 : pc.how == 'l' ? 2 : 3]++;
    pc.itk_n[pc.itk <= 1 ? 0 : pc.itk == 2 ? 1 : pc.itk == 3 ? 2 : 3]++;
    if (pc.age < pc.age_min) pc.age_min = pc.age;
    if (pc.age > pc.age_max) pc.age_max = pc.age;
    pc.age_sum += pc.age;
    if (work > pc.work_max) pc.work_max = work;
    if (pc.t_p0 - pc.t_lk > pc.comp_max) pc.comp_max = pc.t_p0 - pc.t_lk;
    if (pc.t_p1 - pc.t_p0 > pc.pres_max) pc.pres_max = pc.t_p1 - pc.t_p0;
    if (steps != 1) pc.steps_bad++;

    hi = dl_last_ph >= 0 && (dl_last_ph < 0.1 || dl_last_ph > 0.9);
    bad = ft < 13.0 || ft > 20.5 || hi || steps != 1;
    if (bad && pc.lines < 40) {
        pc.lines++;
        pc_write("%10.1f  ft %5.1f  work %5.1f  flush %4.1f  wait %5.1f%c  age %5.1f  itk %ld  comp %4.1f  pres %4.1f  ph %4.2f  steps %ld",
                 now, ft, work, pc.t_fl - pc.t_in, pc.t_lk - pc.t_fl, pc.how, pc.age, pc.itk, pc.t_p0 - pc.t_lk,
                 pc.t_p1 - pc.t_p0, dl_last_ph, steps);
    }
    if (now - pc.sec_start >= 1000.0 && pc.n) {
        pc_write("%10.1f  second: %ld frames, ft %.1f %.1f %.1f (%ld/%ld/%ld/%ld), ph %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld, "
                 "lock %ld/%ld/%ld/%ld, itk %ld/%ld/%ld/%ld, age %.1f %.1f %.1f, max work %.1f comp %.1f pres %.1f, not one step %ld%s",
                 now, pc.n, pc.ft_min, pc.ft_sum / pc.n, pc.ft_max, pc.ft_hist[0], pc.ft_hist[1], pc.ft_hist[2], pc.ft_hist[3],
                 pc.ph_hist[0], pc.ph_hist[1], pc.ph_hist[2], pc.ph_hist[3], pc.ph_hist[4], pc.ph_hist[5], pc.ph_hist[6],
                 pc.ph_hist[7], pc.ph_hist[8], pc.ph_hist[9], pc.how_n[0], pc.how_n[1], pc.how_n[2], pc.how_n[3],
                 pc.itk_n[0], pc.itk_n[1], pc.itk_n[2], pc.itk_n[3], pc.age_min, pc.age_sum / pc.n, pc.age_max,
                 pc.work_max, pc.comp_max, pc.pres_max, pc.steps_bad, pc.lines >= 40 ? " (frame lines cut at 40)" : "");
        pc_reset_second(now);
    }
}
