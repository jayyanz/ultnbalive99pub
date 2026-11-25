/*
 * Timing statistics shared between nba99core.dll (game clock side) and
 * glide2x.dll (frames). nba99core owns the struct and exports
 * nba99core_stats(); glide2x looks it up at run time and works without it.
 *
 * Totals only ever count up. The "sec_" and "r_" blocks are the last full
 * second, published by the side that measures them; *_seq goes up by one on
 * every publish.
 *
 * Version 1 for NBA Live 99 (22 Nov 2026): NBA Live 98's version 11 struct with
 * the fields for 98-only features taken out (options menu, gamepad menu box,
 * widescreen, distance detail, judder log, pause screens). The frame pacing
 * fields stay until the release 3 check decides whether 99 needs the 98 locks.
 * Version 2 (0.3.0): simulation steps and input frames are counted in 99
 * (nba99core timing.inc); r_spf (steps shown per frame) added at the end.
 * Version 3 (0.3.0): input_qpc_lo and input_tick, when the newest input frame was made (glide2x pace log);
 * tick_event is now also set the moment an input frame is made.
 */
#ifndef NBA99_STATS_H
#define NBA99_STATS_H
#include <stdint.h>

#define NBA99_STATS_VERSION 3

typedef struct nba99_stats {
    uint32_t version;               /* NBA99_STATS_VERSION */
    uint32_t size;                  /* sizeof(nba99_stats) */
    volatile long sim_steps;        /* offset 8: game simulation steps (singleailoop calls), total */
    volatile long ticks;            /* calls of the game's first timer callback, total */
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
    long r_long;                    /* frames longer than 50 ms */

    /* frame pacing from NBA Live 98 (StepLock, DisplayLock, QueueSkip, PreciseClock): on since 0.3.0 */
    volatile long ticks_done;       /* game timer callbacks finished */
    void *tick_event;               /* auto-reset event, set after every finished callback and when an input frame is made */
    volatile long precise_clock;    /* 1 when the game's GetTickCount is our high-resolution clock */
    volatile long disp_lock;        /* 1 while the display lock is steering */
    float disp_hz;                  /* display refresh rate (DWM) */
    float clock_rate;               /* game clock speed, 1.0 = real time (set by nba99core_set_clock_rate) */
    float r_ph_min, r_ph_avg, r_ph_max;   /* last second: present time within the refresh interval, 0..1 */
    volatile long input_frames;     /* input frames the game has made (ctrlinterrupt samples), total */
    long r_spf[4];                  /* last second (glide2x): frames that showed 0, 1, 2, 3+ new simulation steps */
    volatile unsigned long input_qpc_lo;   /* low 32 bits of QueryPerformanceCounter when the newest input
                                              frame was made */
    volatile long input_tick;       /* 'ticks' at that callback */
} nba99_stats;

/* exported by nba99core: run the game clock at 'rate' x real time from now on */
typedef void (*nba99core_set_clock_rate_fn)(double rate);

typedef nba99_stats *(*nba99core_stats_fn)(void);

#endif
