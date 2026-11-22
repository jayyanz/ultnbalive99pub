/*
 * Timing statistics shared between nba99core.dll (game clock and simulation
 * side) and glide2x.dll (frames). nba99core owns the struct and exports
 * nba99core_stats(); glide2x looks it up at run time and works without it.
 *
 * Totals only ever count up. The "sec_" and "r_" blocks are the last full
 * second, published by the side that measures them; *_seq goes up by one on
 * every publish.
 */
#ifndef NBA99_STATS_H
#define NBA99_STATS_H
#include <stdint.h>

#define NBA99_STATS_VERSION 11

typedef struct nba99_stats {
    uint32_t version;               /* NBA99_STATS_VERSION */
    uint32_t size;                  /* sizeof(nba99_stats) */
    volatile long sim_steps;        /* offset 8: game simulation steps (60 per game second), total */
    volatile long ticks;            /* game timer callbacks (120 Hz clock), total */
    volatile long presents;         /* frames shown, total (glide2x) */

    /* game side, last full second (nba99core) */
    volatile long sec_seq;
    float sec_len_s;                /* length of the measured window */
    float sec_ticks, sec_steps;     /* per second */
    float sec_gap_min, sec_gap_avg, sec_gap_max;   /* ms between timer callbacks */

    /* frame side, last full second (glide2x) */
    volatile long r_seq;
    float r_fps;
    float r_ft_min, r_ft_avg, r_ft_max;            /* ms between presents */
    long r_spf[4];                  /* frames that showed 0, 1, 2, 3+ new simulation steps */
    long r_long;                    /* frames longer than 50 ms */

    /* version 2: step lock (glide2x waits for the game clock before presenting) */
    volatile long ticks_done;       /* game timer callbacks finished (input frames are made inside them) */
    void *tick_event;               /* auto-reset event, set after every finished callback */
    volatile long precise_clock;    /* 1 when the game's GetTickCount is our high-resolution clock */

    /* version 3: display lock (glide2x steers the game clock so frames land mid-refresh) */
    volatile long disp_lock;        /* 1 while the display lock is steering */
    float disp_hz;                  /* display refresh rate (DWM) */
    float clock_rate;               /* game clock speed, 1.0 = real time (set by nba99core_set_clock_rate) */
    float r_ph_min, r_ph_avg, r_ph_max;   /* last second: present time within the refresh interval, 0..1 */

    /* version 4: live settings from the options menu (nba99core writes, glide2x reads every frame) */
    volatile long crt_mode;         /* CRT filter: -1 = not set (glide2x uses its ini value), 0 off, 1 auto, 2 sharp, 3 soft */

    /* version 5: gamepad menu navigation (nba99core writes from the menu poll, glide2x draws the box) */
    volatile long nav_box[4];       /* selected menu object in 640x480 game pixels: x, y, w, h (w 0 = no box) */
    volatile long nav_time;         /* GetTickCount() of the last update; the box is hidden once this is 250 ms old */

    /* version 6: widescreen matches (Hor+) */
    float ws_aspect;                /* glide2x: aspect of the picture it draws matches into (0 = widescreen off) */
    volatile long draw_2d;          /* nba99core: 1 from the match HUD call until glide2x's next buffer swap */
    float ws_fov;                   /* nba99core: horizontal half-angle written for the match camera (turns), 0 = original */

    /* version 7: step lock on input frames (0.7.6) */
    volatile long input_frames;     /* nba99core: input frames the game has made (one per simulation step), total */

    /* version 8: distance detail (0.7.8) */
    volatile long render_h;         /* glide2x: height in pixels the match 3D is drawn at */

    /* version 9: judder log (0.7.10, Debug) */
    volatile long game_v11;         /* nba99core: 1 = the verified v1.1 exe (glide2x may read its camera and player data) */
    volatile long io_main_us;       /* nba99core: time in the main thread's file calls, total, microseconds */
    volatile long io_main_n;        /* nba99core: number of those calls, total */

    /* version 10: widescreen from the first match frame (0.7.11) */
    volatile long match_3d;         /* nba99core: 1 from the match camera setup or world render until glide2x's next buffer swap */

    /* version 11: pause screens full width (0.7.14) */
    volatile long match_state;      /* nba99core: the front-end screen state (0x5C8284) the last match world render ran in, 0 = none */
} nba99_stats;

/* exported by nba99core: run the game clock at 'rate' x real time from now on */
typedef void (*nba99core_set_clock_rate_fn)(double rate);

typedef nba99_stats *(*nba99core_stats_fn)(void);

#endif
