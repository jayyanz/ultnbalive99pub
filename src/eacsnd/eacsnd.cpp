/*
 * eacsnd.dll for NBA Live 99: the game's sound driver, on SDL3 (release 0.2.0).
 *
 * Replaces EA's eacsnd.dll (42,496 bytes, Watcom, 1998), the game's only user of
 * DirectSound. The exe loads it with LoadLibraryA("eacsnd.dll") (0x56AD31), checks the
 * version and calls 16 stdcall exports. Found by disassembling EA's DLL and the exe
 * (src/README.md "eacsnd.dll on SDL3"):
 *
 *  _iSNDdllversion@0                -> 0x00100003 (the exe refuses anything else)
 *  _iSNDdirectsetfunctions@32       8 callbacks into the game, all stdcall except 3, 4, 6:
 *      1 mixer(dst, frames)         mixes the game's own sounds into the stream
 *      2 voice_ended(voice)         a 3D voice has stopped (EA's stopbuf calls it)
 *      3 abort(fmt, ...)            cdecl: the game's abort message (EA calls it on errors)
 *      4, 6 message(fmt, ...)       cdecl: debug text
 *      5 tick()                     the game's 100 Hz sound tick (music and speech)
 *      7 position(a, b, float xyz[3])  direction of a 3D voice from its two angles
 *      8 decode(src, a2, a1, start, end, dst, 16, 0, channels)  writes a sample as
 *                                   16-bit PCM into dst
 *  _iSNDdirectcaps@4 (hwnd)         -> format bits: 1 mono8, 2 stereo8, 4 mono16,
 *      8 stereo16; 0x200 11025, 0x400 16000, 0x800 22050, 0x1000 32000, 0x2000 44100 Hz;
 *      0x20000 3D voices
 *  _iSNDdirectstart@8 (flags, hwnd) opens the stream: 22050, 16000, 32000, 44100,
 *      11025 Hz and stereo16, stereo8, mono16, mono8 in that order of preference
 *      (the game passes back the caps bits: 22050 Hz stereo 16-bit); 0x20000 turns on 3D voices
 *      (EA's DLL reports it only with hardware 3D buffers, none on Windows 11; ours always does,
 *      but the game starts in auto mode, where iSNDverifymode drops the 3D bit, and no sound in
 *      the shipped banks is marked 3D: the game never uses the 3D functions; see src/README.md)
 *  _iSNDdirectstop@0
 *  _iSNDdirectserve@0               called by the game's timer (0x56B174) under its sound
 *      lock: tops up the stream, calling the tick every 1/100 s of sound at the same
 *      sample-accurate spacing as EA's DLL
 *  _iSNDdirectrecordstart@16, recordpacket@4, recordstop@0: stubs, as in EA's DLL
 *      (-1, 0, 0): voice recording for multiplayer was never finished
 *  3D voices (EA: DirectSound 3D buffers; here: our own voices mixed into the stream):
 *      create3dbuf(a1, a2, rate, frames, start, end, src) -> source id 1..63 (0 = failed);
 *          the game's decoder fills it (length end - start + 1 if end > 0, else frames)
 *      removebuf(id); play3d(id, voice 0..15, loop, rate, vol, a, b) -> 0 or -1;
 *      vol(voice, 0..127); rate(voice, Hz); pos3d(voice, a, b); stopbuf(voice)
 *
 * All mixing happens inside serve(), on the game's thread and under its lock, as with
 * EA's DLL: the game's mixer and tick are never called from another thread. The 3D
 * voices are added to the game's mix there (mix3d.inc: direction from the position
 * callback, panned across the real speaker layout), and finished voices are reported
 * to the game from serve(), as EA's DLL did. SDL plays the result.
 *
 * Settings in nba99fix.ini [sound]: LatencyMs, DeviceFrames, Device, Volume, Mode3D,
 * Log, LogEffects (Debug). Log: logs\<session>\eacsnd.log; effects: effects.log.
 */
#define WIN32_LEAN_AND_MEAN
#include "nba99_cxx.h"   /* C++ build settings, first */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "nba99_build.h"

NBA99_C_BEGIN
#include "mix3d.inc"

/* ---- the few SDL3 declarations we need (SDL3.dll is loaded at run time) ---- */
typedef uint32_t SDL_AudioDeviceID;
typedef int SDL_AudioFormat;
typedef struct { SDL_AudioFormat format; int channels; int freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
#define SDL_AUDIO_F32LE 0x8120

typedef bool (__cdecl *pSDL_Init)(uint32_t);
typedef bool (__cdecl *pSDL_SetHint)(const char *, const char *);
typedef const char *(__cdecl *pSDL_GetError)(void);
typedef const char *(__cdecl *pSDL_GetCurrentAudioDriver)(void);
typedef SDL_AudioStream *(__cdecl *pSDL_OpenAudioDeviceStream)(SDL_AudioDeviceID, const SDL_AudioSpec *, void *, void *);
typedef bool (__cdecl *pSDL_ResumeAudioStreamDevice)(SDL_AudioStream *);
typedef bool (__cdecl *pSDL_PutAudioStreamData)(SDL_AudioStream *, const void *, int);
typedef int (__cdecl *pSDL_GetAudioStreamQueued)(SDL_AudioStream *);
typedef void (__cdecl *pSDL_DestroyAudioStream)(SDL_AudioStream *);
typedef SDL_AudioDeviceID (__cdecl *pSDL_GetAudioStreamDevice)(SDL_AudioStream *);
typedef bool (__cdecl *pSDL_GetAudioDeviceFormat)(SDL_AudioDeviceID, SDL_AudioSpec *, int *);
typedef SDL_AudioDeviceID *(__cdecl *pSDL_GetAudioPlaybackDevices)(int *);
typedef const char *(__cdecl *pSDL_GetAudioDeviceName)(SDL_AudioDeviceID);
typedef void (__cdecl *pSDL_free)(void *);

static struct {
    HMODULE dll;
    pSDL_Init Init;
    pSDL_SetHint SetHint;
    pSDL_GetError GetError;
    pSDL_GetCurrentAudioDriver GetCurrentAudioDriver;
    pSDL_OpenAudioDeviceStream OpenAudioDeviceStream;
    pSDL_ResumeAudioStreamDevice ResumeAudioStreamDevice;
    pSDL_PutAudioStreamData PutAudioStreamData;
    pSDL_GetAudioStreamQueued GetAudioStreamQueued;
    pSDL_DestroyAudioStream DestroyAudioStream;
    pSDL_GetAudioStreamDevice GetAudioStreamDevice;
    pSDL_GetAudioDeviceFormat GetAudioDeviceFormat;
    pSDL_GetAudioPlaybackDevices GetAudioPlaybackDevices;
    pSDL_GetAudioDeviceName GetAudioDeviceName;
    pSDL_free free;
    int inited, failed;
} sdl;

/* ---- the game's callbacks ------------------------------------------------- */
typedef int (__stdcall *cb_mixer_fn)(void *, int);
typedef int (__stdcall *cb_voice_fn)(int);
typedef int (__stdcall *cb_tick_fn)(void);
typedef int (__stdcall *cb_position_fn)(int, int, float *);
typedef int (__stdcall *cb_decode_fn)(int, int, int, int, int, void *, int, int, int);
static cb_mixer_fn g_cb_mixer;
static cb_voice_fn g_cb_ended;
static cb_tick_fn g_cb_tick;
static cb_position_fn g_cb_position;
static cb_decode_fn g_cb_decode;

/* ---- settings ------------------------------------------------------------- */
static char g_dir[MAX_PATH];
static int g_cfg_latency_ms = 40, g_cfg_device_frames, g_cfg_log, g_cfg_effects, g_cfg_volume = 100;
static int g_mode3d = MODE3D_SPEAKERS;
static char g_cfg_device[128];
static const char *const g_mode3d_name[] = { "speakers", "stereo", "off" };

/* ---- stream state ---------------------------------------------------------- */
static SDL_AudioStream *g_stream;
static int g_running;
static int g_rate, g_channels, g_bits, g_shift;     /* the game's format; shift = log2(bytes per frame) */
static int g_out_ch;                               /* output channels (the device's, up to 8) */
static m3d_layout g_layout;
static int g_target_frames;
static int g_tick_counter, g_tick_acc, g_until_tick;
static uint8_t *g_gbuf;                            /* the game's mix */
static float *g_fbuf;                              /* the output mix */
static int g_buf_frames;
static int g_3d;                                   /* 3D voices on (start flag 0x20000) */
static float g_master = 1.0f;

/* ---- 3D sources and voices -------------------------------------------------- */
#define NSRC 64      /* ids 1..63, as EA's DLL */
#define NVOICE 16
typedef struct {
    int used;            /* created and not removed */
    int refs;            /* voices playing it */
    int16_t *data;
    int frames, rate;
    int a1, a2, start, end;
    uint32_t handle;     /* the game's source pointer (callback 8's first argument) */
} source;
typedef struct {
    int active;
    int src;
    int vol, rate, a, b;
    float x, y, z;
    m3d_voice mv;
} voice;
static source g_src[NSRC];
static voice g_voice[NVOICE];
static CRITICAL_SECTION g_lock;     /* sources and voices: the game's thread and serve() */

/* ---- log ------------------------------------------------------------------- */
static HANDLE g_logf = INVALID_HANDLE_VALUE, g_efff = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logcs;
static LARGE_INTEGER g_qpf, g_qpc0;
static double g_last_serve_ms, g_max_gap_ms, g_stat_t0;
static long g_serves, g_underruns, g_frames_mixed, g_voices_started, g_voice_peak, g_errors_logged;

static double now_ms(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void write_line(HANDLE f, const char *fmt, va_list ap)
{
    char buf[1024];
    int n;
    DWORD w;
    if (f == INVALID_HANDLE_VALUE) return;
    n = _snprintf(buf, sizeof buf, "[%10.1f] ", now_ms());
    n += _vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_logcs);
    WriteFile(f, buf, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&g_logcs);
}

static void slog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    write_line(g_logf, fmt, ap);
    va_end(ap);
}

/* errors from the game's calls: logged, at most 200 times */
static void serr(const char *fmt, ...)
{
    va_list ap;
    if (g_errors_logged++ >= 200) return;
    va_start(ap, fmt);
    write_line(g_logf, fmt, ap);
    va_end(ap);
}

#if NBA99_DEBUG
static void elog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    write_line(g_efff, fmt, ap);
    va_end(ap);
}
#define ELOG(...) do { if (g_efff != INVALID_HANDLE_VALUE) elog(__VA_ARGS__); } while (0)
#else
#define ELOG(...) do { } while (0)
#endif

/* ---- SDL ------------------------------------------------------------------- */
static int load_sdl(void)
{
    char path[MAX_PATH];
    if (sdl.dll) return 1;
    _snprintf(path, sizeof path, "%sSDL3.dll", g_dir);
    sdl.dll = LoadLibraryA(path);
    if (!sdl.dll) { slog("ERROR: %s not found (it comes with the release, next to NBAWIN.EXE)", path); return 0; }
#define GET(n) sdl.n = (p##SDL_##n)GetProcAddress(sdl.dll, "SDL_" #n); \
               if (!sdl.n) { slog("ERROR: SDL3.dll has no SDL_" #n); return 0; }
    GET(Init) GET(SetHint) GET(GetError) GET(GetCurrentAudioDriver) GET(OpenAudioDeviceStream)
    GET(ResumeAudioStreamDevice) GET(PutAudioStreamData) GET(GetAudioStreamQueued)
    GET(DestroyAudioStream) GET(GetAudioStreamDevice) GET(GetAudioDeviceFormat)
    GET(GetAudioPlaybackDevices) GET(GetAudioDeviceName) GET(free)
#undef GET
    return 1;
}

static int init_sdl(void)
{
    if (sdl.inited) return 1;
    if (sdl.failed) return 0;
    if (!load_sdl()) { sdl.failed = 1; return 0; }
    if (g_cfg_device_frames > 0) {
        char v[16];
        _snprintf(v, sizeof v, "%d", g_cfg_device_frames);
        sdl.SetHint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", v);
    }
    sdl.SetHint("SDL_AUDIO_DEVICE_STREAM_NAME", "NBA Live 99");
    sdl.SetHint("SDL_APP_NAME", "NBA Live 99");
    if (!sdl.Init(SDL_INIT_AUDIO)) {
        slog("ERROR: SDL audio init failed: %s", sdl.GetError());
        sdl.failed = 1;
        return 0;
    }
    slog("SDL audio driver: %s", sdl.GetCurrentAudioDriver() ? sdl.GetCurrentAudioDriver() : "?");
    sdl.inited = 1;
    return 1;
}

/* the output device: [sound] Device= (part of the name, any case), else the system default,
   which SDL follows when Windows switches it */
static SDL_AudioDeviceID pick_device(void)
{
    SDL_AudioDeviceID *ids, pick = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    int i, n = 0;
    ids = sdl.GetAudioPlaybackDevices(&n);
    for (i = 0; ids && i < n; i++) {
        const char *name = sdl.GetAudioDeviceName(ids[i]);
        int match = 0;
        if (name && g_cfg_device[0]) {
            char a[256], b[128];
            int k;
            for (k = 0; name[k] && k < 255; k++) a[k] = (char)tolower((unsigned char)name[k]);
            a[k] = 0;
            for (k = 0; g_cfg_device[k] && k < 127; k++) b[k] = (char)tolower((unsigned char)g_cfg_device[k]);
            b[k] = 0;
            match = strstr(a, b) != NULL;
        }
        slog("output device %d: %s%s", i + 1, name ? name : "?", match && pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? "  <- Device=" : "");
        if (match && pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) pick = ids[i];
    }
    if (ids) sdl.free(ids);
    if (g_cfg_device[0] && pick == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)
        slog("Device=%s matches no output device: using the system default", g_cfg_device);
    return pick;
}

/* ---- mixing (serve's thread, under the game's lock) ------------------------ */
static void game_mix(uint8_t *dst, int frames)
{
    if (g_cb_mixer) g_cb_mixer(dst, frames);
    else memset(dst, g_bits == 8 ? 0x80 : 0, (size_t)frames << g_shift);
}

/* the game's part, with the tick every 1/100 s of sound: as EA's DLL (chunk sizes rounded
   down to 16 frames, the rounding carried in an accumulator, counters reset after 30000) */
static void fill_game(uint8_t *dst, int frames)
{
    while (frames > 0) {
        int n;
        if (g_until_tick <= 0) {
            int t;
            g_tick_counter++;
            if (g_cb_tick) g_cb_tick();
            t = (int)(((unsigned)g_tick_counter * (unsigned)g_rate) / 100u) - g_tick_acc;
            t &= 0x0ffffff0;
            g_tick_acc += t;
            g_until_tick = t;
            if (g_tick_counter > 30000) { g_tick_counter = 0; g_tick_acc = 0; }
            continue;
        }
        n = frames < g_until_tick ? frames : g_until_tick;
        g_until_tick -= n;
        game_mix(dst, n);
        dst += n << g_shift;
        frames -= n;
    }
}

/* the game's format into the float output: left and right go to the front pair */
static void game_to_float(const uint8_t *src, float *out, int frames)
{
    int i, L = g_layout.fl, R = g_layout.fr;
    memset(out, 0, sizeof(float) * (size_t)frames * g_out_ch);
    for (i = 0; i < frames; i++) {
        float l, r;
        if (g_bits == 16) {
            const int16_t *s = (const int16_t *)src;
            l = s[i * g_channels] * (1.0f / 32768.0f);
            r = g_channels == 2 ? s[i * 2 + 1] * (1.0f / 32768.0f) : l;
        } else {
            l = (src[i * g_channels] - 128) * (1.0f / 128.0f);
            r = g_channels == 2 ? (src[i * 2 + 1] - 128) * (1.0f / 128.0f) : l;
        }
        if (L < 0) out[i * g_out_ch] = (l + r) * 0.5f;
        else { out[i * g_out_ch + L] = l; out[i * g_out_ch + R] = r; }
    }
}

static void voice_targets(voice *v)
{
    float g[M3D_MAXCH];
    int c;
    float a = m3d_volume(v->vol);
    m3d_gains(&g_layout, g_mode3d, v->x, v->y, v->z, g);
    for (c = 0; c < g_out_ch; c++) v->mv.target[c] = g[c] * a;
}

static void voice_step(voice *v)
{
    int hz = v->rate > 0 ? v->rate : g_src[v->src].rate;
    v->mv.step = g_rate > 0 ? (double)hz / (double)g_rate : 0.0;
}

static void voice_release(int i)    /* g_lock held */
{
    voice *v = &g_voice[i];
    if (!v->active) return;
    v->active = 0;
    if (v->src > 0 && v->src < NSRC) {
        source *s = &g_src[v->src];
        if (s->refs > 0) s->refs--;
        if (!s->used && !s->refs && s->data) { HeapFree(GetProcessHeap(), 0, s->data); s->data = NULL; }
    }
}

/* adds the 3D voices into out; fills ended[] with the voices that finished */
static int mix_voices(float *out, int frames, int *ended)
{
    int i, n = 0, active = 0;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < NVOICE; i++) {
        voice *v = &g_voice[i];
        if (!v->active) continue;
        active++;
        m3d_mix(&v->mv, out, g_out_ch, frames);
        if (v->mv.ended) ended[n++] = i;
    }
    if (active > g_voice_peak) g_voice_peak = active;
    LeaveCriticalSection(&g_lock);
    return n;
}

int __stdcall S_stopbuf(int voice);

static void fill(int frames)
{
    int i, ended[NVOICE], ne = 0;
    size_t total = (size_t)frames * g_out_ch;
    fill_game(g_gbuf, frames);
    game_to_float(g_gbuf, g_fbuf, frames);
    if (g_3d) ne = mix_voices(g_fbuf, frames, ended);
    for (i = 0; i < (int)total; i++) {
        float s = g_fbuf[i] * g_master;
        g_fbuf[i] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
    }
    sdl.PutAudioStreamData(g_stream, g_fbuf, (int)(total * sizeof(float)));
    /* as EA's DLL after a fill: finished voices are stopped and reported to the game */
    for (i = 0; i < ne; i++) S_stopbuf(ended[i]);
}

/* ---- exports ----------------------------------------------------------------- */
int __stdcall S_dllversion(void)
{
    return 0x00100003;
}

int __stdcall S_setfunctions(int mixer, int ended, int abortf, int msg4, int tick, int msg6, int position, int decode)
{
    g_cb_mixer = (cb_mixer_fn)(uintptr_t)mixer;
    g_cb_ended = (cb_voice_fn)(uintptr_t)ended;
    g_cb_tick = (cb_tick_fn)(uintptr_t)tick;
    g_cb_position = (cb_position_fn)(uintptr_t)position;
    g_cb_decode = (cb_decode_fn)(uintptr_t)decode;
    slog("setfunctions: mixer 0x%08x, voice_ended 0x%08x, abort 0x%08x, message 0x%08x and 0x%08x, tick 0x%08x, "
         "position 0x%08x, decode 0x%08x", mixer, ended, abortf, msg4, msg6, tick, position, decode);
    return 0;
}

int __stdcall S_caps(int hwnd)
{
    int caps;
    (void)hwnd;
    if (!init_sdl()) return -1;
    caps = 0x0F            /* mono8, stereo8, mono16, stereo16 */
         | 0x3E00          /* 11025, 16000, 22050, 32000, 44100 Hz */
         | 0x20000;        /* 3D voices */
    slog("caps -> 0x%x", caps);
    return caps;
}

int __stdcall S_stop(void)
{
    if (g_stream) {
        sdl.DestroyAudioStream(g_stream);
        g_stream = NULL;
        slog("stop: %ld serves, %ld underruns, longest gap between serves %.1f ms",
             g_serves, g_underruns, g_max_gap_ms);
    }
    g_running = 0;
    return 0;
}

int __stdcall S_start(int flags_in, int hwnd)
{
    SDL_AudioSpec spec, dspec;
    SDL_AudioDeviceID dev;
    int dframes = 0;
    unsigned flags = (unsigned)flags_in, hi = (flags >> 8) & 0xFF, lo = flags & 0xFF;
    (void)hwnd;
    if (g_running) S_stop();
    if (!init_sdl()) return -1;

    if (hi & 0x08) g_rate = 22050;
    else if (hi & 0x04) g_rate = 16000;
    else if (hi & 0x10) g_rate = 32000;
    else if (hi & 0x20) g_rate = 44100;
    else if (hi & 0x02) g_rate = 11025;
    else { slog("start: no sample rate in flags 0x%x", flags); return -1; }
    if (lo & 0x08) { g_channels = 2; g_bits = 16; }
    else if (lo & 0x02) { g_channels = 2; g_bits = 8; }
    else if (lo & 0x04) { g_channels = 1; g_bits = 16; }
    else if (lo & 0x01) { g_channels = 1; g_bits = 8; }
    else { slog("start: no sample format in flags 0x%x", flags); return -1; }
    g_shift = (g_channels == 2 ? 1 : 0) + (g_bits == 16 ? 1 : 0);
    g_3d = (flags & 0x20000) != 0;

    dev = pick_device();
    memset(&dspec, 0, sizeof dspec);
    sdl.GetAudioDeviceFormat(dev, &dspec, &dframes);
    g_out_ch = dspec.channels >= 1 ? dspec.channels : 2;
    if (g_out_ch > M3D_MAXCH) g_out_ch = M3D_MAXCH;
    m3d_layout_init(&g_layout, g_out_ch);

    spec.format = SDL_AUDIO_F32LE;
    spec.channels = g_out_ch;
    spec.freq = g_rate;
    g_stream = sdl.OpenAudioDeviceStream(dev, &spec, NULL, NULL);
    if (!g_stream) {
        slog("ERROR: could not open the audio device: %s", sdl.GetError());
        return -1;
    }

    g_target_frames = (g_rate * g_cfg_latency_ms / 1000) & ~15;
    if (g_target_frames < 256) g_target_frames = 256;
    if (g_buf_frames < g_target_frames) {
        if (g_gbuf) HeapFree(GetProcessHeap(), 0, g_gbuf);
        if (g_fbuf) HeapFree(GetProcessHeap(), 0, g_fbuf);
        g_buf_frames = g_target_frames;
        g_gbuf = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)g_buf_frames * 4);
        g_fbuf = (float *)HeapAlloc(GetProcessHeap(), 0, (size_t)g_buf_frames * M3D_MAXCH * sizeof(float));
    }
    g_tick_counter = g_tick_acc = g_until_tick = 0;
    g_serves = g_underruns = g_frames_mixed = 0;
    g_max_gap_ms = 0;
    g_last_serve_ms = 0;
    g_stat_t0 = now_ms();
    EnterCriticalSection(&g_lock);   /* voices playing on: new layout and rate */
    {
        int i;
        for (i = 0; i < NVOICE; i++) if (g_voice[i].active) { voice_targets(&g_voice[i]); voice_step(&g_voice[i]); }
    }
    LeaveCriticalSection(&g_lock);

    /* start with half the queue as silence (EA's DLL starts with a cleared buffer);
       the game's mixer is only ever called from serve() */
    memset(g_fbuf, 0, (size_t)(g_target_frames / 2) * g_out_ch * sizeof(float));
    sdl.PutAudioStreamData(g_stream, g_fbuf, (int)((size_t)(g_target_frames / 2) * g_out_ch * sizeof(float)));
    sdl.ResumeAudioStreamDevice(g_stream);
    g_running = 1;

    memset(&dspec, 0, sizeof dspec);
    dframes = 0;
    sdl.GetAudioDeviceFormat(sdl.GetAudioStreamDevice(g_stream), &dspec, &dframes);
    slog("start: flags 0x%x -> %d Hz, %d-bit, %s, 3D voices %s; kept %d ms ahead; "
         "device %d Hz, %d channels, %d frames per buffer; output %d channels (%s), Mode3D=%s, Volume=%d",
         flags, g_rate, g_bits, g_channels == 2 ? "stereo" : "mono", g_3d ? "on" : "off", g_cfg_latency_ms,
         dspec.freq, dspec.channels, dframes, g_out_ch,
         g_layout.surround ? "surround: VBAP" : "front speakers: left/right pan",
         g_mode3d_name[g_mode3d], g_cfg_volume);
    return 0;
}

int __stdcall S_serve(void)
{
    int queued, need;
    double t;
    if (!g_running || !g_stream) return 0;
    t = now_ms();
    if (g_last_serve_ms > 0 && t - g_last_serve_ms > g_max_gap_ms) g_max_gap_ms = t - g_last_serve_ms;
    g_last_serve_ms = t;
    g_serves++;

    queued = sdl.GetAudioStreamQueued(g_stream);
    if (queued < 0) return 0;
    queued /= (int)(g_out_ch * sizeof(float));
    if (queued == 0) g_underruns++;
    need = (g_target_frames - queued) & ~15;
    if (need >= 16) {
        if (need > g_buf_frames) need = g_buf_frames;
        fill(need);
        g_frames_mixed += need;
    }
    if (t - g_stat_t0 >= 60000.0) {
        int i, sources = 0;
        for (i = 1; i < NSRC; i++) if (g_src[i].used) sources++;
        slog("last minute: %ld serves, %ld underruns, longest gap between serves %.1f ms; "
             "3D: %ld voices started, at most %ld at once, %d sources loaded",
             g_serves, g_underruns, g_max_gap_ms, g_voices_started, g_voice_peak, sources);
        g_serves = g_underruns = 0;
        g_voices_started = g_voice_peak = 0;
        g_max_gap_ms = 0;
        g_stat_t0 = t;
    }
    return 0;
}

/* voice recording (multiplayer): stubs, exactly as EA's DLL */
int __stdcall S_recordstart(int a, int b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return -1; }
int __stdcall S_recordpacket(int a) { (void)a; return 0; }
int __stdcall S_recordstop(void) { return 0; }

/* create3dbuf(a1, a2, rate, frames, start, end, src): a source the game's decoder fills */
int __stdcall S_create3dbuf(int a1, int a2, int rate, int frames, int start, int end, int src)
{
    int id, len;
    int16_t *data;
    if (!g_3d) return 0;
    a1 &= 0xff; a2 &= 0xff; rate &= 0xffff;
    len = end > 0 ? end - start + 1 : frames;
    if (len <= 0 || len > (1 << 24)) { serr("create3dbuf: bad length %d (frames %d, %d..%d)", len, frames, start, end); return 0; }
    data = (int16_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)len * 2 + 16);
    if (!data) { serr("create3dbuf: out of memory (%d frames)", len); return 0; }
    if (g_cb_decode) g_cb_decode(src, a2, a1, start, end, data, 16, 0, 1);
    EnterCriticalSection(&g_lock);
    for (id = 1; id < NSRC; id++) if (!g_src[id].used && !g_src[id].refs) break;
    if (id < NSRC) {
        source *s = &g_src[id];
        if (s->data) HeapFree(GetProcessHeap(), 0, s->data);
        s->used = 1; s->refs = 0; s->data = data; s->frames = len; s->rate = rate;
        s->a1 = a1; s->a2 = a2; s->start = start; s->end = end; s->handle = (uint32_t)src;
    }
    LeaveCriticalSection(&g_lock);
    if (id >= NSRC) { HeapFree(GetProcessHeap(), 0, data); serr("create3dbuf: all 63 sources in use"); return 0; }
    ELOG("load  source %2d  (%d, %d) src 0x%08x  %d frames at %d Hz", id, a1, a2, (unsigned)src, len, rate);
    return id;
}

int __stdcall S_removebuf(int id)
{
    if (!g_3d) return -1;
    if (id <= 0 || id >= NSRC) { serr("removebuf: bad source %d", id); return 0; }
    EnterCriticalSection(&g_lock);
    if (g_src[id].used) {
        g_src[id].used = 0;
        if (!g_src[id].refs && g_src[id].data) { HeapFree(GetProcessHeap(), 0, g_src[id].data); g_src[id].data = NULL; }
    }
    LeaveCriticalSection(&g_lock);
    return 0;
}

/* the direction of a voice from the game (callback 7), scaled as EA's DLL did (1/65536) */
static void position(int a, int b, float *x, float *y, float *z)
{
    float v[4] = { 0, 0, 1, 0 };
    if (g_cb_position) g_cb_position(a, b, v);
    *x = v[0] * (1.0f / 65536.0f);
    *y = v[1] * (1.0f / 65536.0f);
    *z = v[2] * (1.0f / 65536.0f);
}

int __stdcall S_play3d(int id, int vi, int loop, int rate, int vol, int a, int b)
{
    float x, y, z;
    voice *v;
    if (!g_3d) return -1;
    if (vi < 0 || vi >= NVOICE) { serr("play3d: bad voice %d", vi); return -1; }
    rate &= 0xffff; vol &= 0xff;
    position(a, b, &x, &y, &z);
    EnterCriticalSection(&g_lock);
    if (id <= 0 || id >= NSRC || !g_src[id].used || !g_src[id].data) {
        LeaveCriticalSection(&g_lock);
        serr("play3d: source %d is not loaded (voice %d)", id, vi);
        return -1;
    }
    voice_release(vi);              /* a voice reused while playing: replaced */
    v = &g_voice[vi];
    memset(v, 0, sizeof *v);
    v->active = 1;
    v->src = id;
    g_src[id].refs++;
    v->vol = vol; v->rate = rate; v->a = a; v->b = b;
    v->x = x; v->y = y; v->z = z;
    v->mv.data = g_src[id].data;
    v->mv.frames = g_src[id].frames;
    v->mv.loop = (loop & 0xff) != 0;
    voice_step(v);
    voice_targets(v);
    memcpy(v->mv.cur, v->mv.target, sizeof v->mv.cur);   /* starts at its place, no ramp */
    g_voices_started++;
    LeaveCriticalSection(&g_lock);
    ELOG("play  voice %2d  source %2d (%d, %d)  rate %d  vol %d  loop %d  angles %d %d  -> %.0f deg %+.2f %+.2f %+.2f",
         vi, id, g_src[id].a1, g_src[id].a2, rate, vol, loop & 0xff, a, b,
         atan2((double)x, (double)z) * 180.0 / M3D_PI, x, y, z);
    return 0;
}

int __stdcall S_stopbuf(int vi)
{
    if (vi < 0 || vi >= NVOICE) { serr("stopbuf: bad voice %d", vi); return 0; }
    EnterCriticalSection(&g_lock);
    voice_release(vi);
    LeaveCriticalSection(&g_lock);
    if (g_cb_ended) g_cb_ended(vi);    /* EA's DLL tells the game every time */
    return 0;
}

int __stdcall S_vol(int vi, int vol)
{
    if (vi < 0 || vi >= NVOICE) { serr("vol: bad voice %d", vi); return 0; }
    EnterCriticalSection(&g_lock);
    g_voice[vi].vol = vol & 0xff;
    if (g_voice[vi].active) voice_targets(&g_voice[vi]);
    LeaveCriticalSection(&g_lock);
    return 0;
}

int __stdcall S_rate(int vi, int rate)
{
    if (vi < 0 || vi >= NVOICE) { serr("rate: bad voice %d", vi); return 0; }
    EnterCriticalSection(&g_lock);
    g_voice[vi].rate = rate & 0xffff;
    if (g_voice[vi].active) voice_step(&g_voice[vi]);
    LeaveCriticalSection(&g_lock);
    return 0;
}

int __stdcall S_pos3d(int vi, int a, int b)
{
    float x, y, z;
    if (vi < 0 || vi >= NVOICE) { serr("pos3d: bad voice %d", vi); return 0; }
    position(a, b, &x, &y, &z);
    EnterCriticalSection(&g_lock);
    g_voice[vi].a = a; g_voice[vi].b = b;
    g_voice[vi].x = x; g_voice[vi].y = y; g_voice[vi].z = z;
    if (g_voice[vi].active) voice_targets(&g_voice[vi]);
    LeaveCriticalSection(&g_lock);
    return 0;
}

/* ---- DLL entry ------------------------------------------------------------------ */
static int ini_int(const char *ini, const char *key, int def)
{
    return (int)GetPrivateProfileIntA("sound", key, def, ini);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char ini[MAX_PATH], mode[32], path[MAX_PATH], *s;
        SYSTEMTIME st;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_logcs);
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_qpc0);
        GetModuleFileNameA(inst, g_dir, sizeof g_dir);
        s = strrchr(g_dir, '\\');
        if (s) s[1] = 0;
        _snprintf(ini, sizeof ini, "%snba99fix.ini", g_dir);
        g_cfg_latency_ms = ini_int(ini, "LatencyMs", 40);
        if (g_cfg_latency_ms < 10) g_cfg_latency_ms = 10;
        if (g_cfg_latency_ms > 500) g_cfg_latency_ms = 500;
        g_cfg_device_frames = ini_int(ini, "DeviceFrames", 0);
        g_cfg_volume = ini_int(ini, "Volume", 100);
        if (g_cfg_volume < 0) g_cfg_volume = 0;
        if (g_cfg_volume > 100) g_cfg_volume = 100;
        g_master = (float)g_cfg_volume / 100.0f;
        GetPrivateProfileStringA("sound", "Device", "", g_cfg_device, sizeof g_cfg_device, ini);
        GetPrivateProfileStringA("sound", "Mode3D", "speakers", mode, sizeof mode, ini);
        if (!_stricmp(mode, "stereo")) g_mode3d = MODE3D_STEREO;
        else if (!_stricmp(mode, "off")) g_mode3d = MODE3D_OFF;
        else g_mode3d = MODE3D_SPEAKERS;
        g_cfg_log = ini_int(ini, "Log", NBA99_LOG_DEFAULT);
        g_cfg_effects = NBA99_DEBUG && ini_int(ini, "LogEffects", 0);
        if (g_cfg_log) {
            nba99_log_path(g_dir, "eacsnd.log", path, sizeof path);
            g_logf = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (g_cfg_effects) {
            nba99_log_path(g_dir, "effects.log", path, sizeof path);
            g_efff = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        GetLocalTime(&st);
        slog("NBA Live 99 eacsnd.dll (SDL3) " NBA99_VERSION_LINE "  %04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
        slog("settings: LatencyMs=%d DeviceFrames=%d Device=%s Volume=%d Mode3D=%s LogEffects=%d", g_cfg_latency_ms,
             g_cfg_device_frames, g_cfg_device[0] ? g_cfg_device : "(system default)", g_cfg_volume,
             g_mode3d_name[g_mode3d], g_cfg_effects);
        ELOG("NBA Live 99 effect log " NBA99_VERSION_LINE ": 3D sounds the game loads and starts");
    } else if (reason == DLL_PROCESS_DETACH) {
        /* no SDL calls here: the loader lock is held */
        if (g_logf != INVALID_HANDLE_VALUE) CloseHandle(g_logf);
        if (g_efff != INVALID_HANDLE_VALUE) CloseHandle(g_efff);
        g_logf = g_efff = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}
NBA99_C_END
