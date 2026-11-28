/*
 * Rumble (included by nba99core.cpp after pad_input.inc and pad_ini.inc; 0.4.0).
 *
 * The PC game has no vibration. We add our own, with the PS1 version as the
 * reference for which plays vibrate and how they feel:
 * short steps of 0.1 s, each with a strength for the big (low) motor and the
 * small (high, buzz) motor, events mixing, and only the human controlling the
 * player concerned feels it. Code and pattern table are ours; nothing from the
 * PS1 disc.
 *
 * Events come from the exe's own named game events (Watcom names;
 * 28 Nov 2026: hook the named events). Each is reached through one or more
 * call sites in EA's code; the site's call is pointed at a small thunk of ours
 * that reads the arguments, queues the pattern and jumps on to EA's function
 * unchanged. Nothing in the game is written.
 *   event_startdunk(player object)            dunk started: noted only; the dunker
 *                                             feels Dunk when that basket counts
 *                                             (28 Nov 2026: at the slam, not
 *                                             at the start of the animation)
 *   event_block(stats, player, user)          blocked shot: the blocker
 *   event_steal(stats, player, user)          steal: the stealer
 *   event_playerhardhit(player, other)        hard contact: both (hit player harder)
 *   event_playerhit(player, other)            contact: both
 *   event_playerfall(player, other)           knocked down: the player who falls,
 *                                             and the player who caused it
 *   event_injury(player)                      injury: the injured player
 *   event_hangrim(player)                     hanging on the rim: that player. On
 *                                             PC EA's function is empty (0x44B590,
 *                                             a ret) but anewdunk still calls it
 *                                             (0x428E61), where the PS1 calls its own
 *   event_playerhitsground(player)            hits the floor: that player
 *   event_tipoff(player)                      jump ball: the jumper
 *   event_basket(stats, player, ...)          basket made: the shooter (Dunk if it
 *                                             came from a dunk started under 3 s ago)
 * A player (0 to 9 on court) is controlled by user (int16) at +4 of its object
 * (array of objects at 0x6417B8; -1 = CPU). Offline, user n reads controller
 * slot n (read_user_input), so the user's slot is the pad that vibrates. Users
 * 4 to 7 (the other PC online) never vibrate a pad here.
 *
 * Hanging on the rim is held: its first step lasts as long as the game keeps
 * reporting the hang (every step), then the rest of the pattern plays as a tail.
 *
 * Settings: [input] Rumble = off / low (60%) / medium (100%, default) / high
 * (150%; strong steps then reach the motors' full strength), or a percent up to 200;
 * [rumble] <event> = percent of our table's strength (0 = off), default 100.
 * Debug: Ctrl+F9 plays the dunk pattern on every pad; every event is logged.
 *
 * PS1 comparison (28 Nov 2026, reference only; matched by EA's shared crowd
 * event strings and code order): the PS1 vibrates on hang on rim, the start of
 * a dunk (event_shotpicked, dunk type), injury (its biggest pattern), hard hit
 * and knocked down (big hit on the player and a hit on the other), contact
 * (thuds, both), hits the floor, and block (the blocker). 0.4.0 takes injury,
 * the other player in a knock-down and hang on rim; the dunk is felt at the
 * slam instead. Steal, jump ball and basket are ours.
 */

#define RUMBLE_STEPS 5
#define RUMBLE_QUEUE 8
enum { RV_DUNK, RV_BLOCK, RV_STEAL, RV_HARDHIT, RV_HARDHIT_BY, RV_HIT, RV_HIT_BY, RV_FALL, RV_GROUND, RV_TIPOFF,
       RV_BASKET, RV_INJURY, RV_FALL_BY, RV_HANGRIM, RV_COUNT };

/* our patterns: per 0.1 s step, big motor then buzz motor (0 to 255); written to the feel of the PS1 shapes */
static const struct { const char *key; uint8_t step[RUMBLE_STEPS][2]; } g_rv[RV_COUNT] = {
    { "Dunk",          { { 255, 170 }, { 235, 120 }, { 160, 0 }, { 90, 0 }, { 40, 0 } } },
    { "Block",         { { 225, 150 }, { 140, 0 }, { 60, 0 } } },
    { "Steal",         { { 170, 110 }, { 50, 0 } } },
    { "HardContact",   { { 230, 130 }, { 170, 60 }, { 100, 0 }, { 40, 0 } } },
    { "HardContactBy", { { 150, 80 }, { 60, 0 } } },
    { "Contact",       { { 150, 90 }, { 70, 0 } } },
    { "ContactBy",     { { 90, 50 } } },
    { "KnockedDown",   { { 190, 0 }, { 110, 0 }, { 40, 0 } } },
    { "HitsFloor",     { { 200, 0 }, { 80, 0 } } },
    { "JumpBall",      { { 120, 70 } } },
    { "Basket",        { { 90, 60 }, { 30, 0 } } },
    { "Injury",        { { 255, 220 }, { 255, 180 }, { 180, 0 }, { 100, 0 }, { 40, 0 } } },
    { "KnockedDownBy", { { 200, 130 }, { 140, 40 }, { 70, 0 }, { 25, 0 } } },
    /* held while the player hangs (step 1), then the rest as the tail (28 Nov 2026: longer and stronger) */
    { "HangOnRim",     { { 255, 255 }, { 150, 60 }, { 60, 0 } } },
};
static int g_rv_pct[RV_COUNT];
static int g_rumble_pct;                     /* master strength, 0 = off */
static struct {
    uint16_t lo[RUMBLE_QUEUE], hi[RUMBLE_QUEUE];
    int pos, busy, sent;
    uint16_t hold_lo, hold_hi;               /* a held event (hanging on the rim) */
    DWORD hold_until;
    int holding;
} g_rq[PAD_SLOTS];
static CRITICAL_SECTION g_rumble_cs;
static DWORD g_rumble_last;
static bool (__cdecl *p_RumbleGamepad)(SDL_Gamepad *, uint16_t, uint16_t, uint32_t);
static DWORD g_dunk_start[10];               /* when each on-court player started a dunk (0 = none) */
#define DUNK_TO_BASKET_MS 3000               /* a basket this soon after the start counts as the dunk */
/* Some events repeat: anewdunk calls event_hangrim on every simulation step while the player hangs (about 60 a
   second), and stumblea2 can report the same contact on two or three steps in a row. A repeat for the same
   player and event is dropped: within REPEAT_MS, or for hanging on the rim, until the calls stop for
   HANG_GAP_MS (so one hang is one pattern). */
#define REPEAT_MS 150
#define HANG_GAP_MS 500
static DWORD g_rv_last[10][RV_COUNT];

static int rumble_repeat(int p, int ev)
{
    DWORD now = GetTickCount() | 1, last;
    if (p < 0 || p > 9) return 0;
    last = g_rv_last[p][ev];
    g_rv_last[p][ev] = (ev == RV_HANGRIM || !last || now - last >= REPEAT_MS) ? now : last;
    return last && now - last < (DWORD)(ev == RV_HANGRIM ? HANG_GAP_MS : REPEAT_MS);
}

/* mix a pattern into a slot's queue from its current step (strengths add, capped) */
static void rumble_mix(int slot, int ev, int from)
{
    int i, k;
    if (slot < 0 || slot >= PAD_SLOTS || !g_rumble_pct || !g_rv_pct[ev]) return;
    EnterCriticalSection(&g_rumble_cs);
    for (i = from; i < RUMBLE_STEPS; i++) {
        long lo = (long)g_rv[ev].step[i][0] * 257 * g_rumble_pct / 100 * g_rv_pct[ev] / 100;
        long hi = (long)g_rv[ev].step[i][1] * 257 * g_rumble_pct / 100 * g_rv_pct[ev] / 100;
        if (!lo && !hi) continue;
        k = (g_rq[slot].pos + i - from) % RUMBLE_QUEUE;
        lo += g_rq[slot].lo[k];
        hi += g_rq[slot].hi[k];
        g_rq[slot].lo[k] = (uint16_t)(lo > 65535 ? 65535 : lo);
        g_rq[slot].hi[k] = (uint16_t)(hi > 65535 ? 65535 : hi);
        g_rq[slot].busy = 1;
    }
    LeaveCriticalSection(&g_rumble_cs);
}

static void rumble_play(int slot, int ev) { rumble_mix(slot, ev, 0); }

/* hold an event's first step for as long as the game keeps reporting it (each report extends it by 150 ms);
   when the reports stop, the rest of the pattern plays as a tail (rumble_tick) */
static void rumble_hold(int slot, int ev)
{
    if (slot < 0 || slot >= PAD_SLOTS || !g_rumble_pct || !g_rv_pct[ev]) return;
    EnterCriticalSection(&g_rumble_cs);
    {
        long lo = (long)g_rv[ev].step[0][0] * 257 * g_rumble_pct / 100 * g_rv_pct[ev] / 100;
        long hi = (long)g_rv[ev].step[0][1] * 257 * g_rumble_pct / 100 * g_rv_pct[ev] / 100;
        g_rq[slot].hold_lo = (uint16_t)(lo > 65535 ? 65535 : lo);
        g_rq[slot].hold_hi = (uint16_t)(hi > 65535 ? 65535 : hi);
        g_rq[slot].hold_until = GetTickCount() + 150;
        g_rq[slot].holding = ev + 1;
        g_rq[slot].busy = 1;
    }
    LeaveCriticalSection(&g_rumble_cs);
}

/* the pad slot of the human controlling on-court player p (0 to 9), or -1 */
static int rumble_slot_of_player(int p)
{
    uint8_t *obj;
    int16_t user;
    if (p < 0 || p > 9) return -1;
    obj = *(uint8_t *volatile *)(uintptr_t)(0x6417B8 + 4 * p);
    if (!obj) return -1;
    user = *(volatile int16_t *)(obj + 4);
    return user >= 0 && user < PAD_SLOTS ? user : -1;
}

/* called from the thunks below on the game's thread, before EA's event function */
extern "C" void __cdecl rumble_event(int which, uint32_t eax, uint32_t edx, uint32_t ebx) __asm__("nba99_rumble_event");
extern "C" void __cdecl rumble_event(int which, uint32_t eax, uint32_t edx, uint32_t ebx)
{
    int a = -1, b = -1, ea = -1, eb = -1, pa, pb;
    switch (which) {
    case 0:   /* event_startdunk (object): the slam is felt at the basket, so only note the start */
        a = (int)*(volatile int16_t *)(uintptr_t)eax;
        if (a >= 0 && a <= 9) g_dunk_start[a] = GetTickCount() | 1;
#if NBA99_DEBUG
        LOG("rumble: dunk started by player %d (felt when the basket counts)", a);
#endif
        return;
    case 1: a = (int16_t)edx; ea = RV_BLOCK; break;
    case 2: a = (int16_t)edx; ea = RV_STEAL; break;
    case 3: a = (int16_t)eax; ea = RV_HARDHIT; b = (int16_t)edx; eb = RV_HARDHIT_BY; break;
    case 4: a = (int16_t)eax; ea = RV_HIT; b = (int16_t)edx; eb = RV_HIT_BY; break;
    case 5: a = (int16_t)eax; ea = RV_FALL; b = (int16_t)edx; eb = RV_FALL_BY; break;
    case 6: a = (int16_t)eax; ea = RV_GROUND; break;
    case 7: a = (int16_t)eax; ea = RV_TIPOFF; break;
    case 8:   /* event_basket: a dunk's basket gets the Dunk pattern */
        a = (int16_t)edx;
        ea = RV_BASKET;
        if (a >= 0 && a <= 9 && g_dunk_start[a] && GetTickCount() - g_dunk_start[a] < DUNK_TO_BASKET_MS) ea = RV_DUNK;
        if (a >= 0 && a <= 9) g_dunk_start[a] = 0;
        break;
    case 9: a = (int16_t)eax; ea = RV_INJURY; break;
    case 10: a = (int16_t)eax; ea = RV_HANGRIM; break;
    default: return;
    }
    if (ea != RV_HANGRIM && rumble_repeat(a, ea)) { a = -1; }
    if (eb >= 0 && rumble_repeat(b, eb)) { b = -1; eb = -1; }
    if (a < 0 && eb < 0) return;
    pa = a >= 0 ? rumble_slot_of_player(a) : -1;
    pb = b >= 0 ? rumble_slot_of_player(b) : -1;
#if NBA99_DEBUG
    if (ea != RV_HANGRIM || !rumble_repeat(a, ea))
    LOG("rumble: %s player %d (pad slot %d)%s%s eax=%08lx edx=%08lx ebx=%08lx", a >= 0 ? g_rv[ea].key : "(repeat)", a, pa + 1,
        eb >= 0 ? ", other player " : "", eb >= 0 ? (pb >= 0 ? "on a pad" : "not on a pad") : "",
        (unsigned long)eax, (unsigned long)edx, (unsigned long)ebx);
#endif
    if (a >= 0 && pa >= 0) {
        if (ea == RV_HANGRIM) rumble_hold(pa, ea);
        else rumble_play(pa, ea);
    }
    if (eb >= 0 && pb >= 0) rumble_play(pb, eb);
}

/* one thunk per hooked event: save everything, report, then on to EA's function */
#define RUMBLE_THUNK(n)                                                                            \
    __asm__(".text\n"                                                                              \
            "nba99_rumble_thunk" #n ":\n"                                                         \
            "  pushal\n"                                                                           \
            "  pushl %ebx\n"                                                                       \
            "  pushl %edx\n"                                                                       \
            "  pushl %eax\n"                                                                       \
            "  pushl $" #n "\n"                                                                    \
            "  call nba99_rumble_event\n"                                                          \
            "  addl $16, %esp\n"                                                                   \
            "  popal\n"                                                                            \
            "  jmp *nba99_rumble_fn+" #n "*4\n");                                                  \
    void rumble_thunk##n(void) __asm__("nba99_rumble_thunk" #n);
RUMBLE_THUNK(0) RUMBLE_THUNK(1) RUMBLE_THUNK(2) RUMBLE_THUNK(3) RUMBLE_THUNK(4)
RUMBLE_THUNK(5) RUMBLE_THUNK(6) RUMBLE_THUNK(7) RUMBLE_THUNK(8) RUMBLE_THUNK(9) RUMBLE_THUNK(10)

/* EA's event functions, in thunk order (v1.1 exe) */
uint32_t g_rumble_fn[11] __asm__("nba99_rumble_fn") = {
    0x44B670 /* event_startdunk */, 0x44D130 /* event_block */, 0x44D430 /* event_steal */,
    0x44CF30 /* event_playerhardhit */, 0x44CF80 /* event_playerhit */, 0x44CEC0 /* event_playerfall */,
    0x44CFE0 /* event_playerhitsground */, 0x44BEE0 /* event_tipoff */, 0x44B8E0 /* event_basket */,
    0x44BFC0 /* event_injury */, 0x44B590 /* event_hangrim: empty on PC (a ret), still called */,
};
static void (*const g_rumble_thunk[11])(void) = { rumble_thunk0, rumble_thunk1, rumble_thunk2, rumble_thunk3,
                                                  rumble_thunk4, rumble_thunk5, rumble_thunk6, rumble_thunk7,
                                                  rumble_thunk8, rumble_thunk9, rumble_thunk10 };
/* the call sites in EA's code (caller in brackets) */
static const struct { uint32_t va; int fn; } g_rumble_site[] = {
    { 0x42908B, 0 } /* newstartdunk */,
    { 0x41C9E5, 1 } /* newballcarrier */, { 0x424CB1, 1 } /* boundcheckball */,
    { 0x41C94A, 2 } /* newballcarrier */,
    { 0x43F58D, 3 } /* stumblea2 */,
    { 0x43F699, 4 }, { 0x43FCCB, 4 } /* stumblea2 */,
    { 0x43F596, 5 } /* stumblea2 */,
    { 0x43F0E4, 6 } /* awipe */,
    { 0x43C32E, 7 } /* forcepass */,
    { 0x41FABB, 8 }, { 0x41FB21, 8 }, { 0x41FCB4, 8 } /* countbasket */,
    { 0x43F32B, 9 } /* checkinj */,
    { 0x428E61, 10 } /* anewdunk: the player hangs on the rim */,
};

/* point one call site at a thunk, if it still calls EA's function */
static int rumble_hook_site(uint32_t va, uint32_t fn, void (*thunk)(void))
{
    uint8_t expect[5], with[5];
    int32_t rel = (int32_t)(fn - (va + 5));
    expect[0] = 0xE8; memcpy(expect + 1, &rel, 4);
    rel = (int32_t)((uintptr_t)thunk - (va + 5));
    with[0] = 0xE8; memcpy(with + 1, &rel, 4);
    return patch_code(va, expect, with, 5);
}

/* every 0.1 s, on the input thread (from pad_refresh): set each pad's motors from its queue */
static void rumble_tick(void)
{
    DWORD now = GetTickCount();
    int s;
    if (!p_RumbleGamepad || now - g_rumble_last < 100) return;
    g_rumble_last = now;
#if NBA99_DEBUG
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F9) & 1))
        for (s = 0; s < PAD_SLOTS; s++) if (g_pad[s].gp) { LOG("rumble: test pattern on slot %d", s + 1); rumble_play(s, RV_DUNK); }
#endif
    EnterCriticalSection(&g_rumble_cs);
    for (s = 0; s < PAD_SLOTS; s++) {
        int k = g_rq[s].pos;
        uint32_t lo = g_rq[s].lo[k], hi = g_rq[s].hi[k];
        if (!g_rq[s].busy) continue;
        if (g_rq[s].holding) {
            if ((long)(now - g_rq[s].hold_until) < 0) {
                lo += g_rq[s].hold_lo; hi += g_rq[s].hold_hi;
                if (lo > 65535) lo = 65535;
                if (hi > 65535) hi = 65535;
            } else {
                int ev = g_rq[s].holding - 1;
                g_rq[s].holding = 0;
                rumble_mix(s, ev, 1);                /* the tail, from this step (the lock is re-entrant) */
                lo = g_rq[s].lo[k]; hi = g_rq[s].hi[k];
            }
        }
        g_rq[s].lo[k] = g_rq[s].hi[k] = 0;
        g_rq[s].pos = (k + 1) % RUMBLE_QUEUE;
        if (!g_pad[s].gp) { memset(&g_rq[s], 0, sizeof g_rq[s]); continue; }
        if (lo || hi) { p_RumbleGamepad(g_pad[s].gp, (uint16_t)lo, (uint16_t)hi, 200); g_rq[s].sent = 1; }
        else {
            int i, left = 0;
            for (i = 0; i < RUMBLE_QUEUE; i++) left |= g_rq[s].lo[i] | g_rq[s].hi[i];
            if (g_rq[s].sent) { p_RumbleGamepad(g_pad[s].gp, 0, 0, 0); g_rq[s].sent = 0; }
            if (!left && !g_rq[s].holding) g_rq[s].busy = 0;
        }
    }
    LeaveCriticalSection(&g_rumble_cs);
}

static void rumble_setup(void)
{
    char v[16];
    int i, n = 0;
    if (!g_pad_on) return;
    GetPrivateProfileStringA("input", "Rumble", "medium", v, sizeof v, g_ini);
    /* 28 Nov 2026: the first medium (65%) was too weak on everything; 100% felt like the middle */
    if (!lstrcmpiA(v, "off")) g_rumble_pct = 0;
    else if (!lstrcmpiA(v, "low")) g_rumble_pct = 60;
    else if (!lstrcmpiA(v, "medium") || !lstrcmpiA(v, "on")) g_rumble_pct = 100;
    else if (!lstrcmpiA(v, "high")) g_rumble_pct = 150;
    else g_rumble_pct = atoi(v);
    if (g_rumble_pct < 0) g_rumble_pct = 0;
    if (g_rumble_pct > 200) g_rumble_pct = 200;
    for (i = 0; i < RV_COUNT; i++) {
        g_rv_pct[i] = (int)GetPrivateProfileIntA("rumble", g_rv[i].key, 100, g_ini);
        if (g_rv_pct[i] < 0) g_rv_pct[i] = 0;
        if (g_rv_pct[i] > 200) g_rv_pct[i] = 200;
    }
    if (!g_rumble_pct) { LOG("rumble: off"); return; }
    if (!g_is_v11) { LOG("rumble: not the v1.1 exe; off"); g_rumble_pct = 0; return; }
    InitializeCriticalSection(&g_rumble_cs);
    for (i = 0; i < (int)(sizeof g_rumble_site / sizeof g_rumble_site[0]); i++) {
        if (rumble_hook_site(g_rumble_site[i].va, g_rumble_fn[g_rumble_site[i].fn], g_rumble_thunk[g_rumble_site[i].fn])) n++;
        else LOG("rumble: call site 0x%08lX not as expected; that event will not vibrate", (unsigned long)g_rumble_site[i].va);
    }
    LOG("rumble: on at %d%%, %d of %d event call sites hooked", g_rumble_pct, n,
        (int)(sizeof g_rumble_site / sizeof g_rumble_site[0]));
}

/* after SDL is loaded (pad_load) */
static void rumble_sdl_ready(void)
{
    if (g_rumble_pct) *(FARPROC *)&p_RumbleGamepad = GetProcAddress(psdl.dll, "SDL_RumbleGamepad");
}
