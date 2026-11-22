/*
 * Judder log (0.7.10, Debug; [glide] JudderLog=1, default in Debug). Included by gl_d3d.cpp.
 *
 * Judder was still seen that is not the game clock or the frame rate (timing.log is clean in
 * live play for most seconds), possibly the player animations or the camera. This log shows,
 * for every frame of a match:
 *   - where the time between two buffer swaps went: the game's own work (from the end of our
 *     last present to its next grBufferSwap; includes our draw calls), our batch flush, the
 *     step lock / pacing wait, and the present itself (vsync wait, CRT pass);
 *   - the game's main-thread file calls in that frame (nba99core times them, stats io_main_us);
 *   - how many simulation steps the frame shows;
 *   - whether the camera moved (eye 0x60D7D8 and rotation 0x60D7E4, 12 floats, read at the
 *     swap), whether each player moved (object table 0x5E60FC, players 0 to 9, world position
 *     at +8/+0xC/+0x10, fixed point), and whether each player's drawn shape changed: the
 *     triangles drawn with his uniform texture by the player mesh draw (rend3dfx caller
 *     0x55C270), counted and boxed on the 640x480 Glide screen. A shape that changes while the
 *     player moves every frame means his animation advanced; if the animation only advances
 *     every 2nd or 4th frame, poses stand still while he moves (a 30 or 15 Hz animation).
 * Written to the session's judder.log (logs\<session>\): one summary line per second while a
 * match runs (30 or more steps in the second), and one line per uneven frame (longer than
 * 20.5 ms, or showing 0 or 2+ steps) with its breakdown. Times are glide2x's clock (as in
 * glide2x.log). Reading game memory only happens with the verified v1.1 exe (stats game_v11).
 */
static const char *ps_mode_name(int m);   /* gl_d3d.cpp (0.7.18) */
#define JD_POSE_MAX 16
#define JD_LATE_PER_SEC 30

typedef struct { uint32_t tex; int tris; float x0, y0, x1, y1; } jd_shape;
typedef struct { uint32_t tex; long frames, changed, frames_cs, changed_cs; } jd_pose;   /* _cs: frames with the camera still */

static struct {
    int on;
    HANDLE log;
    double last_out, t_in, t_flush, t_lock;
    long prev_steps, prev_io_us, prev_io_n;
    float cam[12];
    int cam_ok;
    int pos[10][3], pos_ok[10];
    jd_shape cur[JD_POSE_MAX], prev[JD_POSE_MAX];
    int ncur, nprev;
    /* this second */
    double sec_start;
    long frames, steps, spf[4], late_lines;
    double iv_min, iv_max, iv_sum, work_sum, work_max, flush_max, wait_sum, wait_max, pres_sum, pres_max, io_ms, io_max;
    long io_n, cam_moves, pmoves[10], pseen[10], waited, skips, sc_shown, sc_held, sc_skipped, sc_queued_max, sc_mode_seen;
    int sc_mode;
    jd_pose pose[JD_POSE_MAX];
    int npose;
} JD;

static void jd_write(const char *fmt, ...)
{
    char buf[700];
    int n;
    va_list ap;
    DWORD w;
    if (JD.log == INVALID_HANDLE_VALUE || !JD.log) return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(JD.log, buf, (DWORD)n, &w, NULL);
}

static void jd_reset_second(double now)
{
    memset(&JD.frames, 0, (char *)&JD.npose + sizeof JD.npose - (char *)&JD.frames);
    JD.sec_start = now;
    JD.iv_min = 1e9;
}

static void jd_start(void)
{
    char path[MAX_PATH], dir[MAX_PATH], *s;
    static const char *const hdr[] = {
        "Judder log: one line a second while a match runs, and one line for every uneven frame.",
        "Times in ms on glide2x's clock (as in glide2x.log). interval = buffer swap to buffer swap.",
        "  work = the game's own frame (end of our last present to its next swap, includes our draw calls)",
        "  flush = our last batch, wait = step lock or pacing, present = present call (vsync wait, CRT pass)",
        "  file = the game's main-thread file calls inside the frame (open, read, seek, ...)",
        "camera: frames in which the camera moved (eye or direction). players: median frames in which a player moved.",
        "poses: median frames in which a player's drawn shape changed (his animation advanced, or the view of him changed).",
        "poses, camera still: the same, counted only in frames where the camera did not move (the shape then changes only",
        "  with the animation). If players move in nearly every frame but these poses change in about half or a quarter of",
        "  the camera-still frames, the animations run at 30 or 15 Hz.",
        "Numbers in brackets: how many players or player textures the median is over.",
    };
    int i;
    JD.on = 1;
    JD.log = INVALID_HANDLE_VALUE;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    s = strrchr(dir, '\\');
    if (s) s[1] = 0;
    nba99_log_path(dir, "judder.log", path, sizeof path);
    JD.log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    jd_write("NBA Live 99 judder log, glide2x %s.", NBA99_VERSION_LINE);
    for (i = 0; i < (int)(sizeof hdr / sizeof hdr[0]); i++) jd_write("%s", hdr[i]);
    jd_reset_second(gnow_ms());
}

/* called for every triangle (Debug): record the player meshes' shapes by uniform texture */
void jd_tri(const GrVertex *a, const GrVertex *b, const GrVertex *c, uint32_t caller)
{
    const GrVertex *v[3];
    jd_shape *sh = NULL;
    uint32_t tex;
    int i;
    if (!JD.on || caller != 0x0055C270 || !G.cb.tex0[0] || !G.bound[0]) return;
    tex = G.bound[0]->addr;
    for (i = 0; i < JD.ncur; i++) if (JD.cur[i].tex == tex) { sh = &JD.cur[i]; break; }
    if (!sh) {
        if (JD.ncur >= JD_POSE_MAX) return;
        sh = &JD.cur[JD.ncur++];
        sh->tex = tex; sh->tris = 0;
        sh->x0 = sh->y0 = 1e30f; sh->x1 = sh->y1 = -1e30f;
    }
    v[0] = a; v[1] = b; v[2] = c;
    for (i = 0; i < 3; i++) {
        float x = v[i]->x, y = v[i]->y;
        if (x >= 524288.0f) x -= 786432.0f;   /* the game's vertex snapping offset */
        if (y >= 524288.0f) y -= 786432.0f;
        if (x < sh->x0) sh->x0 = x;
        if (x > sh->x1) sh->x1 = x;
        if (y < sh->y0) sh->y0 = y;
        if (y > sh->y1) sh->y1 = y;
    }
    sh->tris++;
}

static void jd_swap_in(void)
{
    static int started;
    if (!started && G.cfg_judder) { started = 1; jd_start(); }
    if (JD.on) JD.t_in = gnow_ms();
}
static void jd_flushed(void) { if (JD.on) JD.t_flush = gnow_ms(); }
static void jd_locked(void) { if (JD.on) JD.t_lock = gnow_ms(); }

static int jd_median(long *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) for (j = i; j > 0 && a[j] < a[j - 1]; j--) { long t = a[j]; a[j] = a[j - 1]; a[j - 1] = t; }
    return n ? (int)a[n / 2] : 0;
}

static void jd_second_line(double now, nba99_stats *st)
{
    long pm[10], po[JD_POSE_MAX], pc[JD_POSE_MAX], still = JD.frames - JD.cam_moves;
    int i, np = 0, npo = 0, npc = 0;
    if (JD.frames && JD.steps >= 30) {
        for (i = 0; i < 10; i++) if (JD.pseen[i] >= JD.frames / 2) pm[np++] = JD.pmoves[i];
        for (i = 0; i < JD.npose; i++) if (JD.pose[i].frames >= JD.frames / 2) po[npo++] = JD.pose[i].changed;
        for (i = 0; i < JD.npose; i++) if (still >= 4 && JD.pose[i].frames_cs >= still / 2) pc[npc++] = JD.pose[i].changed_cs;
        jd_write("%9.1f s  frames %3ld steps %3ld spf 0:%ld 1:%ld 2:%ld 3+:%ld | interval %5.1f %5.1f %5.1f | work %4.1f max %4.1f"
                 " | flush max %4.1f | wait %4.1f max %4.1f | present %4.1f max %4.1f | file %ld calls %5.1f ms, max %4.1f in a frame"
                 " | camera %ld/%ld | players %d/%ld (%d) | poses %d/%ld (%d) | poses, camera still %d/%ld (%d)"
                 " | queue full %ld/%ld, skipped %ld | screen: shown %ld, held 2+ %ld, never shown %ld, queued max %ld, %s",
                 now / 1000.0, JD.frames, JD.steps, JD.spf[0], JD.spf[1], JD.spf[2], JD.spf[3], JD.iv_min,
                 JD.iv_sum / JD.frames, JD.iv_max, JD.work_sum / JD.frames, JD.work_max, JD.flush_max, JD.wait_sum / JD.frames,
                 JD.wait_max, JD.pres_sum / JD.frames, JD.pres_max, JD.io_n, JD.io_ms, JD.io_max, JD.cam_moves, JD.frames,
                 jd_median(pm, np), JD.frames, np, jd_median(po, npo), JD.frames, npo, jd_median(pc, npc), still, npc,
                 JD.waited, JD.frames, JD.skips, JD.sc_shown, JD.sc_held, JD.sc_skipped, JD.sc_queued_max,
                 JD.sc_mode_seen ? ps_mode_name(JD.sc_mode) : "no statistics");
    }
    (void)st;
    jd_reset_second(now);
}

/* 0.7.18: what reached the screen since the last present (gl_d3d.cpp present_stats).
   dp presents went out over dr refreshes: dp = 1, dr = 1 is a frame shown for one refresh;
   dr >= 2 per frame means the picture stayed up 2+ refreshes (a repeat); dp > dr means
   frames that never reached the screen. queued = presents not yet on the screen. */
static void jd_screen(unsigned dp, unsigned dr, unsigned queued, int mode)
{
    if (!JD.on) return;
    if (dp) {
        JD.sc_shown += dp;
        if (dp == 1 && dr >= 2) JD.sc_held++;
        if (dp > dr) JD.sc_skipped += dp - dr;
        if (dp > 1 && dr > dp) JD.sc_held++;
    }
    if ((long)queued > JD.sc_queued_max) JD.sc_queued_max = queued;
    JD.sc_mode = mode;
    JD.sc_mode_seen = 1;
}

/* 0.7.19: a frame was not presented to clear the display queue (gl_d3d.cpp queue_watch) */
static void jd_note_skip(void)
{
    if (!JD.on) return;
    JD.skips++;
    jd_write("%9.1f s    display queue: one frame not shown (Present had blocked for 30 frames)", gnow_ms() / 1000.0);
}

/* after the present: one frame done */
static void jd_swap_out(void)
{
    nba99_stats *st;
    double now, iv, work, flush, wait, pres, io = 0;
    long steps = 0, ion = 0;
    int i, j, cam_moved = -1, moved = 0, posed = 0;
    if (!JD.on) return;
    now = gnow_ms();
    st = core_stats();
    if (JD.last_out <= 0) { JD.last_out = now; JD.ncur = 0; return; }
    iv = now - JD.last_out;
    work = JD.t_in - JD.last_out;
    flush = JD.t_flush - JD.t_in;
    wait = JD.t_lock - JD.t_flush;
    pres = now - JD.t_lock;
    if (st) {
        steps = st->sim_steps - JD.prev_steps;
        JD.prev_steps = st->sim_steps;
        io = (st->io_main_us - JD.prev_io_us) / 1000.0;
        ion = st->io_main_n - JD.prev_io_n;
        JD.prev_io_us = st->io_main_us;
        JD.prev_io_n = st->io_main_n;
    }
    if (st && st->game_v11) {
        float cam[12];
        memcpy(cam, (const void *)(uintptr_t)0x60D7D8, sizeof cam);
        cam_moved = JD.cam_ok && memcmp(cam, JD.cam, sizeof cam) != 0;
        memcpy(JD.cam, cam, sizeof cam);
        JD.cam_ok = 1;
        for (i = 0; i < 10; i++) {
            const unsigned char *p = *(const unsigned char *const *)(uintptr_t)(0x5E60FC + 4 * i);
            int q[3];
            if (!p || IsBadReadPtr(p, 0x14)) { JD.pos_ok[i] = 0; continue; }
            memcpy(q, p + 8, sizeof q);
            if (JD.pos_ok[i]) {
                JD.pseen[i]++;
                if (memcmp(q, JD.pos[i], sizeof q)) { JD.pmoves[i]++; moved++; }
            }
            memcpy(JD.pos[i], q, sizeof q);
            JD.pos_ok[i] = 1;
        }
    }
    /* poses: this frame's player shapes against the last frame's */
    for (i = 0; i < JD.ncur; i++) {
        jd_shape *c = &JD.cur[i];
        jd_pose *ps = NULL;
        for (j = 0; j < JD.npose; j++) if (JD.pose[j].tex == c->tex) ps = &JD.pose[j];
        if (!ps && JD.npose < JD_POSE_MAX) { ps = &JD.pose[JD.npose++]; memset(ps, 0, sizeof *ps); ps->tex = c->tex; }
        for (j = 0; j < JD.nprev; j++) {
            jd_shape *p = &JD.prev[j];
            if (p->tex != c->tex) continue;
            if (ps) { ps->frames++; if (cam_moved == 0) ps->frames_cs++; }
            if (p->tris != c->tris || fabsf((c->x1 - c->x0) - (p->x1 - p->x0)) > 0.3f || fabsf((c->y1 - c->y0) - (p->y1 - p->y0)) > 0.3f) {
                if (ps) { ps->changed++; if (cam_moved == 0) ps->changed_cs++; }
                posed++;
            }
        }
    }
    memcpy(JD.prev, JD.cur, sizeof JD.cur);
    JD.nprev = JD.ncur;
    JD.ncur = 0;

    JD.frames++;
    JD.steps += steps;
    JD.spf[steps >= 3 ? 3 : steps < 0 ? 0 : steps]++;
    if (iv < JD.iv_min) JD.iv_min = iv;
    if (iv > JD.iv_max) JD.iv_max = iv;
    JD.iv_sum += iv;
    JD.work_sum += work; if (work > JD.work_max) JD.work_max = work;
    if (flush > JD.flush_max) JD.flush_max = flush;
    JD.wait_sum += wait; if (wait > JD.wait_max) JD.wait_max = wait;
    JD.pres_sum += pres; if (pres > JD.pres_max) JD.pres_max = pres;
    if (pres >= 2.0) JD.waited++;  /* Present waited: a frame was in the display queue (0.7.17) */
    JD.io_ms += io; JD.io_n += ion; if (io > JD.io_max) JD.io_max = io;
    if (cam_moved > 0) JD.cam_moves++;
    /* an uneven frame in a match: its breakdown */
    if ((iv > 20.5 || steps != 1) && st && st->sec_steps >= 30 && JD.late_lines < JD_LATE_PER_SEC) {
        JD.late_lines++;
        jd_write("%9.1f s    frame %5.1f ms = work %5.1f (file %ld calls %4.1f) + flush %4.1f + wait %4.1f + present %5.1f | steps %ld"
                 " | camera %s, players moved %d, poses changed %d of %d",
                 now / 1000.0, iv, work, ion, io, flush, wait, pres, steps, cam_moved < 0 ? "?" : cam_moved ? "moved" : "still",
                 moved, posed, JD.nprev);
    }
    JD.last_out = now;
    if (now - JD.sec_start >= 1000.0) jd_second_line(now, st);
}
