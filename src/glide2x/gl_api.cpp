/*
 * glide2x.dll for NBA Live 99 - Glide 2.x API, state tracking and batching.
 *
 * Every exported Glide function takes g_lock (the game calls Glide from more
 * than one thread: grSstWinOpen on its window thread, rendering on the main
 * thread). Draw calls convert Glide vertices into GVtx and hand them to the
 * D3D11 backend in batches; state changes mark the batch dirty.
 * Functions the game never calls are generated as stubs (gl_stubs.cpp).
 */
#include "nba99_cxx.h"   /* C++ build settings, first (0.7.15) */
#include "gl_internal.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

/* C++'s math.h (cmath) removes the min() and max() macros windef.h gave the
   C code; the same macros again (0.7.15) */
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
NBA99_C_BEGIN
GState G;
CRITICAL_SECTION g_lock;
static HANDLE g_logf = INVALID_HANDLE_VALUE;
static LARGE_INTEGER g_qpf, g_qpc0;
static HMODULE g_self;
static char g_dir[MAX_PATH];

#define LOCK() EnterCriticalSection(&g_lock)
#define UNLOCK() LeaveCriticalSection(&g_lock)

/* ------------------------------------------------------------------------- */
/* logging and config                                                        */
/* ------------------------------------------------------------------------- */
double gnow_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

void glog(const char *fmt, ...)
{
    char buf[1024];
    int n;
    va_list ap;
    DWORD w;
    if (g_logf == INVALID_HANDLE_VALUE) return;
    n = _snprintf(buf, 32, "[%9.1f ms] ", gnow_ms());
    va_start(ap, fmt);
    n += _vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    WriteFile(g_logf, buf, (DWORD)n, &w, NULL);
}

static int cfg_int(const char *ini, const char *key, int def)
{
    return (int)GetPrivateProfileIntA("glide", key, def, ini);
}

static void load_config(void)
{
    char ini[MAX_PATH], buf[32];
    _snprintf(ini, sizeof ini, "%snba99fix.ini", g_dir);
    G.cfg_width = cfg_int(ini, "Width", 0);          /* 0 = desktop */
    G.cfg_height = cfg_int(ini, "Height", 0);
    G.cfg_windowed = cfg_int(ini, "Windowed", 0);
    G.cfg_vsync = cfg_int(ini, "VSync", 0);          /* 0 = own 60 Hz pacing, 1 = vsync */
    G.cfg_fps_limit = cfg_int(ini, "FrameLimit", 60);
    G.cfg_linear = cfg_int(ini, "LinearScaling", 1); /* final upscale filter */
    G.cfg_menu_linear = cfg_int(ini, "LinearMenus", 1);
    G.cfg_menu_fullres = cfg_int(ini, "MenuFullRes", 1);
    G.cfg_shots = cfg_int(ini, "Screenshots", 1);
    G.cfg_cull_invert = cfg_int(ini, "CullInvert", 0);
    G.cfg_log = cfg_int(ini, "Log", NBA99_LOG_DEFAULT);
    G.cfg_overlay = cfg_int(ini, "Overlay", NBA99_DEBUG);
    G.cfg_step_lock = cfg_int(ini, "StepLock", 0);
    G.cfg_display_lock = cfg_int(ini, "DisplayLock", 0);
    G.cfg_queue_skip = cfg_int(ini, "QueueSkip", 0);
    G.cfg_depth_wrap = cfg_int(ini, "DepthWrap", 1);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); G.qpf_hz = f.QuadPart; }
    G.cfg_dump_frame = NBA99_DEBUG ? cfg_int(ini, "DumpFrame", 0) : 0;
    G.cfg_judder = NBA99_DEBUG ? cfg_int(ini, "JudderLog", 0) : 0;
    GetPrivateProfileStringA("glide", "Gamma", "1.0", buf, sizeof buf, ini);
    G.cfg_gamma = (float)atof(buf);
    GetPrivateProfileStringA("glide", "CRT", "off", buf, sizeof buf, ini);
    G.cfg_widescreen = cfg_int(ini, "Widescreen", 0);
    G.cfg_crt = !_stricmp(buf, "auto") || !strcmp(buf, "1") ? 1 : !_stricmp(buf, "sharp") || !strcmp(buf, "2") ? 2 :
                !_stricmp(buf, "soft") || !strcmp(buf, "3") ? 3 : 0;
    if (G.cfg_gamma <= 0.1f || G.cfg_gamma > 5.0f) G.cfg_gamma = 1.0f;
}

/* ------------------------------------------------------------------------- */
/* texture helpers                                                           */
/* ------------------------------------------------------------------------- */
void tex_dims(FxI32 lod, FxI32 aspect, int *w, int *h)
{
    int big = 256 >> lod;           /* GR_LOD_256 = 0 ... GR_LOD_1 = 8 */
    int ww = big, hh = big;
    if (big < 1) big = 1;
    /* aspect: 0 = 8x1, 1 = 4x1, 2 = 2x1, 3 = 1x1, 4 = 1x2, 5 = 1x4, 6 = 1x8 (width x height) */
    if (aspect < 3) hh = big >> (3 - aspect);
    else if (aspect > 3) ww = big >> (aspect - 3);
    if (ww < 1) ww = 1;
    if (hh < 1) hh = 1;
    *w = ww;
    *h = hh;
}

int tex_bpp(FxI32 format)
{
    return format >= GR_TEXFMT_ARGB_8332 ? 2 : 1;
}

FxU32 tex_level_bytes(int w, int h, FxI32 format)
{
    FxU32 b = (FxU32)(w * h * tex_bpp(format));
    if (b < 8) b = 8;
    return (b + 7) & ~7u;
}

FxU32 tex_mem_required(FxI32 smallLod, FxI32 largeLod, FxI32 aspect, FxI32 format)
{
    FxU32 total = 0;
    FxI32 lod;
    for (lod = largeLod; lod <= smallLod; lod++) {
        int w, h;
        tex_dims(lod, aspect, &w, &h);
        total += tex_level_bytes(w, h, format);
    }
    return total;
}

/* ------------------------------------------------------------------------- */
/* vertices                                                                  */
/* ------------------------------------------------------------------------- */
static void palettes_update(void)
{
    int t;
    for (t = 0; t < NUM_TMUS; t++)
        if (G.palette_dirty[t]) {
            G.pal_row[t] = d3d_palette_row(G.palette[t]);
            G.palette_dirty[t] = 0;
        }
}

static float g_bias;   /* current depth bias level */
static uint32_t g_caller;          /* return address into the game of the current Glide call */

static FxU32 g_stw_hint; /* GR_HINT_STWHINT flags */

static void conv(const GrVertex *s, GVtx *d)
{
    float x = s->x, y = s->y;
    /* Some Glide games add 3<<18 to x/y to snap them to 1/16 pixel. */
    if (x >= 524288.0f) { x -= 786432.0f; if (!G.snap_seen) { G.snap_seen = 1; glog("vertex snapping offset detected"); } }
    if (y >= 524288.0f) y -= 786432.0f;
    /* widescreen: 2D and front-end draws go into the 4:3 area in the middle of the wide
       target; match 3D (layer 1) keeps 0..640 across the full width */
    if (G.ws && !G.ws_layer) x = (x * G.sx + (float)G.c43_x) * ((float)FB_W / (float)G.rt_w);
    d->x = x; d->y = y;
    d->ooz = s->ooz;
    /* A Voodoo keeps only the low 16 bits of an out-of-range depth value. NBA Live 99 draws the
     * backgrounds of its in-match stat boxes with ooz 32768000 (500 x 65536), which on the card
     * wraps to 0, the nearest depth, so the boxes cover the players. Clamping it to the far end,
     * as a Direct3D depth range does, put the players and their reflections over the boxes. */
    if (d->ooz >= 65536.0f && G.cfg_depth_wrap && (G.depth_mode == GR_DEPTHBUFFER_ZBUFFER ||
                                                  G.depth_mode == GR_DEPTHBUFFER_ZBUFFER_COMPARE_TO_BIAS)) {
        d->ooz -= 65536.0f * floorf(d->ooz / 65536.0f);
        if (G.depth_wrap_seen < 8 && s->ooz != G.depth_wrap_last) {
            G.depth_wrap_seen++;
            G.depth_wrap_last = s->ooz;
            glog("out-of-range depth (ooz %.0f, caller %08x, depth mode %d func %d write %d) wrapped to 16 bits, as on a Voodoo",
                 s->ooz, g_caller, G.depth_mode, G.depth_func, G.depth_mask);
        }
    }
    d->oow = s->oow;
    d->r = s->r; d->g = s->g; d->b = s->b; d->a = s->a;
    /* Glide's STW hint says which units have their own s/t and w; by default every
     * texture unit uses the vertex oow, and TMU1 uses TMU0's s/t. */
    d->s0 = s->tmuvtx[0].sow; d->t0 = s->tmuvtx[0].tow;
    d->oow0 = (g_stw_hint & 0x2) ? s->tmuvtx[0].oow : s->oow;
    if (g_stw_hint & 0x10) { d->s1 = s->tmuvtx[1].sow; d->t1 = s->tmuvtx[1].tow; }
    else { d->s1 = d->s0; d->t1 = d->t0; }
    d->oow1 = (g_stw_hint & 0x8) ? s->tmuvtx[1].oow : d->oow0;
    d->bias = g_bias;
    d->pal0 = (float)G.pal_row[0];
    d->pal1 = (float)G.pal_row[1];
    d->pad = 0;
}

static int culled(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    float dxAB, dxBC, dyAB, dyBC, area;
    int mode = G.cull_mode;
    if (mode == GR_CULL_DISABLE) return 0;
    if (G.cfg_cull_invert) mode = (mode == GR_CULL_NEGATIVE) ? GR_CULL_POSITIVE : GR_CULL_NEGATIVE;
    dxAB = a->x - b->x; dxBC = b->x - c->x;
    dyAB = a->y - b->y; dyBC = b->y - c->y;
    area = dxAB * dyBC - dxBC * dyAB;
    /* as in Glide: NEGATIVE culls area < 0, POSITIVE culls area >= 0 */
    if (mode == GR_CULL_NEGATIVE) return area < 0.0f;
    return area >= 0.0f;
}


/* ------------------------------------------------------------------------- */
/* draw trace (Debug): Ctrl+F10 writes one frame's draws, in order, to        */
/* logs\glide_trace_<frame>.txt, and saves that frame as a picture.            */
/* Consecutive triangles with the same caller and state make one line.        */
/* ------------------------------------------------------------------------- */
#define CALLER() (g_caller = (uint32_t)(uintptr_t)__builtin_return_address(0))
static HANDLE g_tr = INVALID_HANDLE_VALUE;
static long g_tr_seen;
static uint32_t g_tr_seq;
static struct {
    int on;
    uint32_t key[12];              /* caller and state */
    int n;                         /* triangles */
    float col[4];                  /* first vertex colour of the run */
    float x0, y0, x1, y1, z0, z1, w0, w1;
} g_run;

static void tr_write(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    DWORD wr;
    if (g_tr == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    WriteFile(g_tr, buf, (DWORD)n, &wr, NULL);
}

static void tr_flush_run(void)
{
    const uint32_t *k = g_run.key;
    if (!g_run.on) return;
    /* seq caller tris | box x0,y0-x1,y1 | ooz min-max | oow min-max | depth mode/func/mask bias | blend | alpha test, chroma | tex | cc ac | cull buf */
    tr_write("%5u %08x %5d  rgba %3.0f,%3.0f,%3.0f,%3.0f cc%08x  box %4.0f,%4.0f-%4.0f,%4.0f  ooz %7.0f-%7.0f  oow %.5f-%.5f  depth m%u f%u w%u b%d  blend %u,%u  at %u/%u ck%u  tex %08x f%u  cc %u,%u,%u,%u ac %u,%u,%u,%u  cull %u buf %u",
             g_tr_seq++, k[0], g_run.n, g_run.col[0], g_run.col[1], g_run.col[2], g_run.col[3], (unsigned)G.const_color, g_run.x0, g_run.y0, g_run.x1, g_run.y1, g_run.z0, g_run.z1, g_run.w0, g_run.w1,
             k[1] & 0xFF, (k[1] >> 8) & 0xFF, (k[1] >> 16) & 0xFF, (int)(int16_t)(k[1] >> 24 | ((k[9] & 0xFF) << 8)),
             k[2] & 0xFFFF, k[2] >> 16, k[3] & 0xFF, (k[3] >> 8) & 0xFF, k[3] >> 16, k[4], k[5],
             k[6] & 0xFF, (k[6] >> 8) & 0xFF, (k[6] >> 16) & 0xFF, k[6] >> 24, k[7] & 0xFF, (k[7] >> 8) & 0xFF, (k[7] >> 16) & 0xFF, k[7] >> 24,
             k[8] & 0xFF, (k[8] >> 8) & 0xFF);
    g_run.on = 0;
}

static void tr_tri(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    uint32_t k[12];
    const GrVertex *v[3];
    int i, bias = (int)g_bias;
    if (g_tr == INVALID_HANDLE_VALUE) return;
    memset(k, 0, sizeof k);
    k[0] = g_caller;
    k[1] = (uint32_t)(G.depth_mode & 0xFF) | ((uint32_t)(G.depth_func & 0xFF) << 8) | ((uint32_t)(G.depth_mask & 0xFF) << 16) |
           ((uint32_t)(bias & 0xFF) << 24);
    k[9] = (uint32_t)((bias >> 8) & 0xFF);
    k[2] = (uint32_t)(G.blend_rgb_sf & 0xFFFF) | ((uint32_t)G.blend_rgb_df << 16);
    k[3] = (G.cb.at[0] & 0xFF) | ((G.cb.at[1] & 0xFF) << 8) | (G.cb.at[2] << 16);
    k[4] = G.cb.tex0[0] && G.bound[0] ? G.bound[0]->addr : 0xFFFFFFFF;
    k[5] = G.cb.tex0[0] && G.bound[0] ? (uint32_t)G.bound[0]->format : 0;
    k[6] = (G.cb.cc[0] & 0xFF) | ((G.cb.cc[1] & 0xFF) << 8) | ((G.cb.cc[2] & 0xFF) << 16) | ((G.cb.cc[3] & 0xFF) << 24);
    k[7] = (G.cb.ac[0] & 0xFF) | ((G.cb.ac[1] & 0xFF) << 8) | ((G.cb.ac[2] & 0xFF) << 16) | ((G.cb.ac[3] & 0xFF) << 24);
    k[8] = (uint32_t)(G.cull_mode & 0xFF) | ((uint32_t)(G.render_buffer & 0xFF) << 8);
    if (!g_run.on || memcmp(k, g_run.key, sizeof k)) {
        tr_flush_run();
        memcpy(g_run.key, k, sizeof k);
        g_run.on = 1; g_run.n = 0;
        g_run.col[0] = a->r; g_run.col[1] = a->g; g_run.col[2] = a->b; g_run.col[3] = a->a;
        g_run.x0 = g_run.y0 = g_run.z0 = g_run.w0 = 1e30f;
        g_run.x1 = g_run.y1 = g_run.z1 = g_run.w1 = -1e30f;
    }
    v[0] = a; v[1] = b; v[2] = c;
    for (i = 0; i < 3; i++) {
        float x = v[i]->x, y = v[i]->y;
        if (x >= 524288.0f) x -= 786432.0f;
        if (y >= 524288.0f) y -= 786432.0f;
        g_run.x0 = min(g_run.x0, x); g_run.x1 = max(g_run.x1, x);
        g_run.y0 = min(g_run.y0, y); g_run.y1 = max(g_run.y1, y);
        g_run.z0 = min(g_run.z0, v[i]->ooz); g_run.z1 = max(g_run.z1, v[i]->ooz);
        g_run.w0 = min(g_run.w0, v[i]->oow); g_run.w1 = max(g_run.w1, v[i]->oow);
    }
    g_run.n++;
}

/* LFB access and clears, in draw order */
void trace_lfb(const char *what, int buf, int x, int y, int w, int h, uint32_t caller)
{
    if (g_tr == INVALID_HANDLE_VALUE) return;
    tr_flush_run();
    tr_write("%5u %08x %s buf %d  %d,%d %dx%d", g_tr_seq++, caller, what, buf, x, y, w, h);
}

/* called at each buffer swap: end a trace, or start one on request */
static void tr_swap(int interval)
{
    if (g_tr != INVALID_HANDLE_VALUE) {
        tr_flush_run();
        tr_write("%5u %08x swap interval %d (end of trace)", g_tr_seq++, g_caller, interval);
        CloseHandle(g_tr);
        g_tr = INVALID_HANDLE_VALUE;
        G.trace_dump = 1;   /* this swap presents the traced frame: save its picture */
        glog("draw trace: done (frame %u)", G.frame);
    }
    if (NBA99_DEBUG && !(G.frame % 30)) {   /* or a trace.req file in the game folder (for scripted tests) */
        char req[MAX_PATH];
        _snprintf(req, sizeof req, "%strace.req", g_dir);
        if (GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES && DeleteFileA(req)) InterlockedIncrement(&G.trace_req);
    }
    if (NBA99_DEBUG && G.trace_req != g_tr_seen) {
        char path[MAX_PATH], name[64];
        g_tr_seen = G.trace_req;
        _snprintf(name, sizeof name, "glide_trace_%u.txt", G.frame + 1);
        nba99_log_path(g_dir, name, path, sizeof path);
        g_tr = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        g_tr_seq = 0;
        g_run.on = 0;
        tr_write("draw trace of frame %u (Glide 640x480 screen coordinates; ooz 0..65535; one line per run of triangles with the same caller and state)", G.frame + 1);
        tr_write("  seq   caller  tris  box  ooz  oow  depth: m mode (0 off, 1 z, 2 w), f func (1 less, 3 lequal, 7 always), w write, b bias  blend src,dst  at alpha func/ref ck chroma  tex addr, format  cc/ac combine  cull buf");
        glog("draw trace: started, %s", path);
    }
}

static void tri(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    GVtx v[3];
    if (culled(a, b, c)) return;
    if (NBA99_DEBUG) { tr_tri(a, b, c); if (G.cfg_judder) jd_tri(a, b, c, g_caller); }
    if (G.ws) d3d_ws_layer();
    palettes_update();
    conv(a, &v[0]); conv(b, &v[1]); conv(c, &v[2]);
    d3d_add_tris(v, 3);
}

/* lines and points as quads one Glide pixel wide */
static void quad_from(const GrVertex *a, const GrVertex *b, float ox, float oy)
{
    GrVertex p[4];
    p[0] = *a; p[1] = *b; p[2] = *b; p[3] = *a;
    p[0].x -= ox; p[0].y -= oy; p[1].x -= ox; p[1].y -= oy;
    p[2].x += ox; p[2].y += oy; p[3].x += ox; p[3].y += oy;
    {
        GVtx v[6];
        if (G.ws) d3d_ws_layer();
        palettes_update();
        conv(&p[0], &v[0]); conv(&p[1], &v[1]); conv(&p[2], &v[2]);
        conv(&p[0], &v[3]); conv(&p[2], &v[4]); conv(&p[3], &v[5]);
        d3d_add_tris(v, 6);
    }
}

static void line(const GrVertex *a, const GrVertex *b)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    if (fabsf(dx) >= fabsf(dy)) quad_from(a, b, 0.0f, 0.5f);   /* x-major: widen in y */
    else quad_from(a, b, 0.5f, 0.0f);
}

/* ------------------------------------------------------------------------- */
/* init / board / window                                                     */
/* ------------------------------------------------------------------------- */
static void set_defaults(void)
{
    int t;
    G.render_buffer = GR_BUFFER_BACKBUFFER;
    G.cull_mode = GR_CULL_DISABLE;
    G.depth_mode = GR_DEPTHBUFFER_DISABLE;
    G.depth_func = 1;               /* GR_CMP_LESS */
    G.depth_mask = 0;
    G.blend_rgb_sf = 4; G.blend_rgb_df = 0; G.blend_a_sf = 4; G.blend_a_df = 0;   /* ONE, ZERO */
    G.color_mask_rgb = 1; G.color_mask_a = 1;
    G.clip[0] = 0; G.clip[1] = 0; G.clip[2] = FB_W; G.clip[3] = FB_H;
    G.const_color = 0xFFFFFFFF;
    memset(&G.cb, 0, sizeof G.cb);
    G.cb.vp[0] = 2.0f / FB_W; G.cb.vp[1] = -2.0f / FB_H;
    G.cb.cc[0] = GR_COMBINE_FUNCTION_LOCAL;   /* iterated colour */
    G.cb.ac[0] = GR_COMBINE_FUNCTION_LOCAL;
    G.cb.t0c[0] = GR_COMBINE_FUNCTION_LOCAL; G.cb.t0c[2] = GR_COMBINE_FUNCTION_LOCAL;
    G.cb.t1c[0] = GR_COMBINE_FUNCTION_LOCAL; G.cb.t1c[2] = GR_COMBINE_FUNCTION_LOCAL;
    G.cb.constColor[0] = G.cb.constColor[1] = G.cb.constColor[2] = G.cb.constColor[3] = 1.0f;
    G.cb.at[0] = GR_CMP_ALWAYS;
    for (t = 0; t < NUM_TMUS; t++) {
        G.tex_filter_min[t] = G.tex_filter_mag[t] = 1;
        G.tex_clamp_s[t] = G.tex_clamp_t[t] = 0;
        G.tex_mipmode[t] = 0;
        G.tex_lodbias[t] = 0;
        G.bound[t] = NULL;
        G.palette_dirty[t] = 1;
    }
    G.dirty = 1;
}

void GAPI grGlideInit(void)
{
    int t;
    LOCK();
    glog("grGlideInit");
    for (t = 0; t < NUM_TMUS; t++)
        if (!G.tmu_mem[t]) G.tmu_mem[t] = (uint8_t *)VirtualAlloc(NULL, TMU_MEM_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!G.shadow[0]) {
        G.shadow[0] = (uint16_t *)VirtualAlloc(NULL, FB_W * FB_H * 2, MEM_COMMIT, PAGE_READWRITE);
        G.shadow[1] = (uint16_t *)VirtualAlloc(NULL, FB_W * FB_H * 2, MEM_COMMIT, PAGE_READWRITE);
        G.lock_conv = VirtualAlloc(NULL, FB_W * FB_H * 4, MEM_COMMIT, PAGE_READWRITE);
    }
    set_defaults();
    UNLOCK();
}

void GAPI grGlideShutdown(void)
{
    LOCK();
    glog("grGlideShutdown");
    win_detach();
    d3d_close();
    G.open = 0;
    UNLOCK();
}

void GAPI grGlideGetVersion(char *version)
{
    /* the game's detect code scores versions "2.3".."2.42" highest */
    if (version) strcpy(version, "2.42");
}

/*
 * The game's GrHwConfiguration is smaller than the Glide 2.43 SDK one (its
 * headers had fewer TMUs per board), and its renderer function pointers
 * (0x640054, 0x640058 = draw triangle) sit right behind its copy at 0x63FFBC.
 * Clearing the full SDK-sized struct wiped them and the first 3D frame called
 * address 0. So write only num_sst and board 0: type plus a 2-TMU Voodoo
 * config, 40 bytes in all, which fits every Glide 2.x layout.
 */
typedef struct {
    FxI32 num_sst;
    FxI32 type;
    FxI32 fbRam, fbiRev, nTexelfx;
    FxBool sliDetect;
    GrTMUConfig_t tmu[2];
} HwConfigMin;

FxBool GAPI grSstQueryBoards(GrHwConfiguration *hw)
{
    if (hw) hw->num_sst = 1;
    return 1;
}

FxBool GAPI grSstQueryHardware(GrHwConfiguration *hw)
{
    HwConfigMin c;
    if (!hw) return 0;
    memset(&c, 0, sizeof c);
    c.num_sst = 1;
    c.type = 0;            /* GR_SSTTYPE_VOODOO (the game only checks for type 1, Voodoo Rush) */
    c.fbRam = 4;
    c.fbiRev = 2;
    c.nTexelfx = 2;
    c.tmu[0].tmuRev = 1; c.tmu[0].tmuRam = 4;
    c.tmu[1].tmuRev = 1; c.tmu[1].tmuRam = 4;
    memcpy(hw, &c, sizeof c);
    return 1;
}

void GAPI grSstSelect(int which) { (void)which; }

FxBool GAPI grSstWinOpen(FxU32 hwnd, FxI32 res, FxI32 refresh, FxI32 cformat, FxI32 origin, int ncol, int naux)
{
    FxBool ok;
    LOCK();
    glog("grSstWinOpen(hwnd 0x%08lx, res %ld, refresh %ld, colour format %ld, origin %ld, %d colour, %d aux)",
         (unsigned long)hwnd, (long)res, (long)refresh, (long)cformat, (long)origin, ncol, naux);
    if (res != 7) glog("note: resolution %ld requested; this build always uses a 640x480 Glide screen", (long)res);
    if (origin != GR_ORIGIN_UPPER_LEFT) glog("note: lower-left origin requested; not supported yet");
    if (G.open) d3d_close();
    ok = d3d_open(hwnd ? (HWND)(uintptr_t)hwnd : GetActiveWindow());
    G.open = ok;
    if (ok) {
        win_attach(G.hwnd);
        memset(G.shadow[0], 0, FB_W * FB_H * 2);
        memset(G.shadow[1], 0, FB_W * FB_H * 2);
        G.gpu_newer[0] = G.gpu_newer[1] = 0;
    }
    G.dirty = 1;
    UNLOCK();
    return ok;
}

void GAPI grSstWinClose(void)
{
    LOCK();
    glog("grSstWinClose");
    win_detach();
    d3d_close();
    G.open = 0;
    UNLOCK();
}

void GAPI grSstIdle(void) { LOCK(); d3d_flush(); d3d_present_front_if_needed(1); UNLOCK(); }
FxBool GAPI grSstIsBusy(void) { return 0; }
FxU32 GAPI grSstStatus(void) { return 0x0FFFF03F; }   /* idle, no swaps pending */
FxU32 GAPI grSstScreenWidth(void) { return FB_W; }
FxU32 GAPI grSstScreenHeight(void) { return FB_H; }
FxBool GAPI grSstVRetraceOn(void) { return 1; }
FxU32 GAPI grSstVideoLine(void) { return 0; }
void GAPI grSstOrigin(FxI32 origin) { if (origin != GR_ORIGIN_UPPER_LEFT) glog("grSstOrigin(%ld) not supported", (long)origin); }
FxBool GAPI grSstControl(FxU32 code) { (void)code; return 1; }
int GAPI grBufferNumPending(void) { return 0; }
void GAPI grHints(FxU32 type, FxU32 flags)
{
    if (type == 0) {   /* GR_HINT_STWHINT */
        LOCK();
        g_stw_hint = flags;
        glog("grHints(STWHINT, 0x%lx)", (unsigned long)flags);
        UNLOCK();
    }
}
void GAPI grDitherMode(FxI32 mode) { (void)mode; }
void GAPI grSplash(float x, float y, float w, float h, FxU32 frame) { (void)x; (void)y; (void)w; (void)h; (void)frame; }
void GAPI grGlideShamelessPlug(FxBool on) { (void)on; }

/* ------------------------------------------------------------------------- */
/* buffers                                                                   */
/* ------------------------------------------------------------------------- */
void GAPI grBufferClear(FxU32 color, FxU32 alpha, FxU32 depth)
{
    CALLER();
    if (NBA99_DEBUG) trace_lfb("clear", G.render_buffer, (int)color, (int)depth, G.depth_mask, 0, g_caller);
    LOCK();
    if (G.open) d3d_clear(color, alpha, depth);
    UNLOCK();
}

void GAPI grBufferSwap(int interval)
{
    CALLER();
    LOCK();
    if (NBA99_DEBUG) tr_swap(interval);
    if (G.open) d3d_swap(interval);
    UNLOCK();
}

void GAPI grRenderBuffer(FxI32 buffer)
{
    LOCK();
    if (buffer != G.render_buffer) {
        d3d_flush();
        G.render_buffer = buffer == GR_BUFFER_FRONTBUFFER ? 0 : 1;
        G.dirty = 1;
    }
    UNLOCK();
}

/* ------------------------------------------------------------------------- */
/* drawing                                                                   */
/* ------------------------------------------------------------------------- */
void GAPI grDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    CALLER();
    LOCK();
    if (G.open) tri(a, b, c);
    UNLOCK();
}

void GAPI grAADrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c,
                           FxBool ab, FxBool bc, FxBool ca)
{
    CALLER();
    (void)ab; (void)bc; (void)ca;
    LOCK();
    if (G.open) tri(a, b, c);
    UNLOCK();
}

void GAPI guDrawTriangleWithClip(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    CALLER();
    LOCK();
    if (G.open) tri(a, b, c);
    UNLOCK();
}

void GAPI guAADrawTriangleWithClip(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    CALLER();
    LOCK();
    if (G.open) tri(a, b, c);
    UNLOCK();
}

void GAPI guMPDrawTriangle(const GrVertex *a, const GrVertex *b, const GrVertex *c)
{
    CALLER();
    LOCK();
    if (G.open) tri(a, b, c);
    UNLOCK();
}

void GAPI grDrawLine(const GrVertex *a, const GrVertex *b)
{
    CALLER();
    LOCK();
    if (G.open) line(a, b);
    UNLOCK();
}

void GAPI grAADrawLine(const GrVertex *a, const GrVertex *b)
{
    CALLER();
    LOCK();
    if (G.open) line(a, b);
    UNLOCK();
}

void GAPI grDrawPoint(const GrVertex *p)
{
    CALLER();
    GrVertex q = *p;
    LOCK();
    q.x += 0.5f;
    if (G.open) { GrVertex a = *p; a.x -= 0.5f; quad_from(&a, &q, 0.0f, 0.5f); }
    UNLOCK();
}

void GAPI grAADrawPoint(const GrVertex *p) { grDrawPoint(p); }

static void fan(int n, const int *ilist, const GrVertex *verts)
{
    int i;
    for (i = 1; i + 1 < n; i++) {
        const GrVertex *a = &verts[ilist ? ilist[0] : 0];
        const GrVertex *b = &verts[ilist ? ilist[i] : i];
        const GrVertex *c = &verts[ilist ? ilist[i + 1] : i + 1];
        tri(a, b, c);
    }
}

void GAPI grDrawPolygon(int n, const int *ilist, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, ilist, v); UNLOCK(); }
void GAPI grDrawPlanarPolygon(int n, const int *ilist, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, ilist, v); UNLOCK(); }
void GAPI grAADrawPolygon(int n, const int *ilist, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, ilist, v); UNLOCK(); }
void GAPI grDrawPolygonVertexList(int n, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, NULL, v); UNLOCK(); }
void GAPI grDrawPlanarPolygonVertexList(int n, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, NULL, v); UNLOCK(); }
void GAPI grAADrawPolygonVertexList(int n, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, NULL, v); UNLOCK(); }
void GAPI guDrawPolygonVertexListWithClip(int n, const GrVertex *v) { CALLER(); LOCK(); if (G.open) fan(n, NULL, v); UNLOCK(); }

/* ------------------------------------------------------------------------- */
/* render state                                                              */
/* ------------------------------------------------------------------------- */
#define SET4(arr, a0, a1, a2, a3) do { \
    if ((arr)[0] != (uint32_t)(a0) || (arr)[1] != (uint32_t)(a1) || (arr)[2] != (uint32_t)(a2) || (arr)[3] != (uint32_t)(a3)) { \
        d3d_flush(); (arr)[0] = (a0); (arr)[1] = (a1); (arr)[2] = (a2); (arr)[3] = (a3); G.dirty = 1; } } while (0)

void GAPI grColorCombine(FxI32 func, FxI32 factor, FxI32 local, FxI32 other, FxBool invert)
{
    LOCK();
    SET4(G.cb.cc, func, factor, local, other);
    if (G.cb.inv[0] != (uint32_t)(invert != 0)) { d3d_flush(); G.cb.inv[0] = invert != 0; G.dirty = 1; }
    UNLOCK();
}

void GAPI grAlphaCombine(FxI32 func, FxI32 factor, FxI32 local, FxI32 other, FxBool invert)
{
    LOCK();
    SET4(G.cb.ac, func, factor, local, other);
    if (G.cb.inv[1] != (uint32_t)(invert != 0)) { d3d_flush(); G.cb.inv[1] = invert != 0; G.dirty = 1; }
    UNLOCK();
}

void GAPI grTexCombine(FxI32 tmu, FxI32 rgb_func, FxI32 rgb_factor, FxI32 a_func, FxI32 a_factor,
                       FxBool rgb_inv, FxBool a_inv)
{
    LOCK();
    if (tmu == 0) {
        SET4(G.cb.t0c, rgb_func, rgb_factor, a_func, a_factor);
        if (G.cb.tinv[0] != (uint32_t)(rgb_inv != 0) || G.cb.tinv[1] != (uint32_t)(a_inv != 0)) {
            d3d_flush(); G.cb.tinv[0] = rgb_inv != 0; G.cb.tinv[1] = a_inv != 0; G.dirty = 1;
        }
    } else if (tmu == 1) {
        SET4(G.cb.t1c, rgb_func, rgb_factor, a_func, a_factor);
        if (G.cb.tinv[2] != (uint32_t)(rgb_inv != 0) || G.cb.tinv[3] != (uint32_t)(a_inv != 0)) {
            d3d_flush(); G.cb.tinv[2] = rgb_inv != 0; G.cb.tinv[3] = a_inv != 0; G.dirty = 1;
        }
    }
    UNLOCK();
}

/* old-style texture combine presets (GR_TEXTURECOMBINE_*) */
void GAPI grTexCombineFunction(FxI32 tmu, FxI32 fnc)
{
    switch (fnc) {
    case 0: grTexCombine(tmu, 0, 0, 0, 0, 0, 0); break;                       /* ZERO */
    case 1: grTexCombine(tmu, 1, 0, 1, 0, 0, 0); break;                       /* DECAL */
    case 2: grTexCombine(tmu, 3, 8, 3, 8, 0, 0); break;                       /* OTHER */
    case 3: grTexCombine(tmu, 4, 8, 4, 8, 0, 0); break;                       /* ADD */
    case 4: grTexCombine(tmu, 3, 1, 3, 1, 0, 0); break;                       /* MULTIPLY */
    case 5: grTexCombine(tmu, 6, 8, 6, 8, 0, 0); break;                       /* SUBTRACT */
    case 6: grTexCombine(tmu, 7, 4, 7, 4, 0, 0); break;                       /* DETAIL */
    case 7: grTexCombine(tmu, 7, 4, 7, 4, 0, 0); break;                       /* DETAIL_OTHER */
    case 8: grTexCombine(tmu, 7, 5, 7, 5, 0, 0); break;                       /* TRILINEAR_ODD */
    case 9: grTexCombine(tmu, 7, 0xd, 7, 0xd, 0, 0); break;                   /* TRILINEAR_EVEN */
    case 10: grTexCombine(tmu, 1, 0, 1, 0, 1, 1); break;                      /* ONE */
    default: grTexCombine(tmu, 1, 0, 1, 0, 0, 0); break;
    }
}
void GAPI guTexCombineFunction(FxI32 tmu, FxI32 fnc) { grTexCombineFunction(tmu, fnc); }

void GAPI grConstantColorValue(FxU32 c)
{
    LOCK();
    if (c != G.const_color) {
        d3d_flush();
        G.const_color = c;
        /* Glide 2 default colour format is ARGB */
        G.cb.constColor[0] = ((c >> 16) & 255) / 255.0f;
        G.cb.constColor[1] = ((c >> 8) & 255) / 255.0f;
        G.cb.constColor[2] = (c & 255) / 255.0f;
        G.cb.constColor[3] = ((c >> 24) & 255) / 255.0f;
        G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grConstantColorValue4(float a, float r, float g, float b)
{
    LOCK();
    d3d_flush();
    G.cb.constColor[0] = r / 255.0f; G.cb.constColor[1] = g / 255.0f;
    G.cb.constColor[2] = b / 255.0f; G.cb.constColor[3] = a / 255.0f;
    G.dirty = 1;
    UNLOCK();
}

void GAPI grAlphaBlendFunction(FxI32 rgb_sf, FxI32 rgb_df, FxI32 a_sf, FxI32 a_df)
{
    LOCK();
    if (rgb_sf != G.blend_rgb_sf || rgb_df != G.blend_rgb_df || a_sf != G.blend_a_sf || a_df != G.blend_a_df) {
        d3d_flush();
        G.blend_rgb_sf = rgb_sf; G.blend_rgb_df = rgb_df; G.blend_a_sf = a_sf; G.blend_a_df = a_df;
        G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grAlphaTestFunction(FxI32 f)
{
    LOCK();
    if (G.cb.at[0] != (uint32_t)f) { d3d_flush(); G.cb.at[0] = f; G.dirty = 1; }
    UNLOCK();
}

void GAPI grAlphaTestReferenceValue(FxU8 v)
{
    LOCK();
    if (G.cb.at[1] != v) { d3d_flush(); G.cb.at[1] = v; G.dirty = 1; }
    UNLOCK();
}

void GAPI grChromakeyMode(FxI32 mode)
{
    LOCK();
    if (G.cb.at[2] != (uint32_t)(mode != 0)) { d3d_flush(); G.cb.at[2] = mode != 0; G.dirty = 1; }
    UNLOCK();
}

void GAPI grChromakeyValue(FxU32 v)
{
    LOCK();
    if (G.cb.at[3] != (v & 0xFFFFFF)) { d3d_flush(); G.cb.at[3] = v & 0xFFFFFF; G.dirty = 1; }
    UNLOCK();
}

void GAPI grCullMode(FxI32 mode) { G.cull_mode = mode; }   /* culling is done on the CPU */

void GAPI grDepthBufferMode(FxI32 mode)
{
    LOCK();
    if (mode != G.depth_mode) { d3d_flush(); G.depth_mode = mode; G.cb.inv[2] = mode; G.dirty = 1; }
    UNLOCK();
}

void GAPI grDepthBufferFunction(FxI32 f)
{
    LOCK();
    if (f != G.depth_func) { d3d_flush(); G.depth_func = f; G.dirty = 1; }
    UNLOCK();
}

void GAPI grDepthMask(FxBool on)
{
    LOCK();
    if ((on != 0) != G.depth_mask) { d3d_flush(); G.depth_mask = on != 0; G.dirty = 1; }
    UNLOCK();
}

/* per-vertex, so changing it never breaks a batch */
void GAPI grDepthBiasLevel(FxI32 level) { g_bias = (float)(int16_t)level; }

void GAPI grColorMask(FxBool rgb, FxBool a)
{
    LOCK();
    if ((rgb != 0) != G.color_mask_rgb || (a != 0) != G.color_mask_a) {
        d3d_flush(); G.color_mask_rgb = rgb != 0; G.color_mask_a = a != 0; G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grClipWindow(FxU32 minx, FxU32 miny, FxU32 maxx, FxU32 maxy)
{
    LOCK();
    if ((int)minx != G.clip[0] || (int)miny != G.clip[1] || (int)maxx != G.clip[2] || (int)maxy != G.clip[3]) {
        d3d_flush();
        G.clip[0] = minx; G.clip[1] = miny; G.clip[2] = maxx; G.clip[3] = maxy;
        G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grFogMode(FxI32 mode)
{
    LOCK();
    if (G.cb.inv[3] != (uint32_t)mode) { d3d_flush(); G.cb.inv[3] = mode; G.dirty = 1; }
    if (mode) glog("grFogMode(%ld): fog is not implemented yet", (long)mode);
    UNLOCK();
}

void GAPI grFogColorValue(FxU32 c)
{
    LOCK();
    G.cb.fogColor[0] = ((c >> 16) & 255) / 255.0f;
    G.cb.fogColor[1] = ((c >> 8) & 255) / 255.0f;
    G.cb.fogColor[2] = (c & 255) / 255.0f;
    UNLOCK();
}

void GAPI grFogTable(const FxU8 *table) { (void)table; }

void GAPI grGammaCorrectionValue(float v)
{
    LOCK();
    G.gamma = v;
    glog("grGammaCorrectionValue(%.2f)", v);
    UNLOCK();
}

/* ------------------------------------------------------------------------- */
/* textures                                                                  */
/* ------------------------------------------------------------------------- */
FxU32 GAPI grTexMinAddress(FxI32 tmu) { (void)tmu; return 0; }
FxU32 GAPI grTexMaxAddress(FxI32 tmu) { (void)tmu; return TMU_MEM_SIZE - 8; }

FxU32 GAPI grTexCalcMemRequired(FxI32 smallLod, FxI32 largeLod, FxI32 aspect, FxI32 format)
{
    return tex_mem_required(smallLod, largeLod, aspect, format);
}

FxU32 GAPI grTexTextureMemRequired(FxU32 evenOdd, GrTexInfo *info)
{
    (void)evenOdd;
    return info ? tex_mem_required(info->smallLod, info->largeLod, info->aspectRatio, info->format) : 0;
}

void GAPI grTexDownloadMipMap(FxI32 tmu, FxU32 start, FxU32 evenOdd, GrTexInfo *info)
{
    FxU32 size;
    (void)evenOdd;
    if (tmu < 0 || tmu >= NUM_TMUS || !info || !info->data) return;
    LOCK();
    size = tex_mem_required(info->smallLod, info->largeLod, info->aspectRatio, info->format);
    if (start + size <= TMU_MEM_SIZE && G.tmu_mem[tmu]) {
        /* source data is the levels packed without padding; copy each level to its padded slot */
        const uint8_t *src = (const uint8_t *)info->data;
        FxU32 off = start;
        FxI32 lod;
        for (lod = info->largeLod; lod <= info->smallLod; lod++) {
            int w, h;
            FxU32 raw, slot;
            tex_dims(lod, info->aspectRatio, &w, &h);
            raw = (FxU32)(w * h * tex_bpp(info->format));
            slot = tex_level_bytes(w, h, info->format);
            memcpy(G.tmu_mem[tmu] + off, src, raw);
            src += raw;
            off += slot;
        }
        d3d_tex_invalidate(tmu, start, size);
    } else {
        glog("grTexDownloadMipMap: address 0x%lx + %lu outside texture memory", (unsigned long)start, (unsigned long)size);
    }
    UNLOCK();
}

void GAPI grTexDownloadMipMapLevelPartial(FxI32 tmu, FxU32 start, FxI32 thisLod, FxI32 largeLod, FxI32 aspect,
                                          FxI32 format, FxU32 evenOdd, void *data, int first, int last)
{
    FxU32 off = start;
    FxI32 lod;
    int w, h, bpp = tex_bpp(format);
    (void)evenOdd;
    if (tmu < 0 || tmu >= NUM_TMUS || !data) return;
    LOCK();
    for (lod = largeLod; lod < thisLod; lod++) {
        tex_dims(lod, aspect, &w, &h);
        off += tex_level_bytes(w, h, format);
    }
    tex_dims(thisLod, aspect, &w, &h);
    if (first < 0) first = 0;
    if (last >= h) last = h - 1;
    if (off + tex_level_bytes(w, h, format) <= TMU_MEM_SIZE && G.tmu_mem[tmu] && last >= first) {
        memcpy(G.tmu_mem[tmu] + off + (FxU32)(first * w * bpp), data, (size_t)(last - first + 1) * w * bpp);
        d3d_tex_invalidate(tmu, start, tex_mem_required(thisLod, largeLod, aspect, format));
    }
    UNLOCK();
}

void GAPI grTexDownloadMipMapLevel(FxI32 tmu, FxU32 start, FxI32 thisLod, FxI32 largeLod, FxI32 aspect,
                                   FxI32 format, FxU32 evenOdd, void *data)
{
    int w, h;
    tex_dims(thisLod, aspect, &w, &h);
    grTexDownloadMipMapLevelPartial(tmu, start, thisLod, largeLod, aspect, format, evenOdd, data, 0, h - 1);
}

void GAPI grTexSource(FxI32 tmu, FxU32 start, FxU32 evenOdd, GrTexInfo *info)
{
    TexEntry *e;
    (void)evenOdd;
    if (tmu < 0 || tmu >= NUM_TMUS || !info) return;
    LOCK();
    e = d3d_tex_get(tmu, start, info);
    if (e != G.bound[tmu]) {
        d3d_flush();
        G.bound[tmu] = e;
        G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grTexDownloadTable(FxI32 tmu, FxI32 type, void *data)
{
    if (!data) return;
    LOCK();
    if (type == GR_TEXTABLE_PALETTE) {
        if (tmu >= 0 && tmu < NUM_TMUS && memcmp(G.palette[tmu], data, 1024)) {
            memcpy(G.palette[tmu], data, 1024);
            G.palette_dirty[tmu] = 1;
        }
    } else {
        static int logged;
        if (!logged++) glog("grTexDownloadTable: NCC tables (YIQ textures) are not supported yet");
    }
    UNLOCK();
}

void GAPI grTexDownloadTablePartial(FxI32 tmu, FxI32 type, void *data, int start, int end)
{
    if (!data || type != GR_TEXTABLE_PALETTE || tmu < 0 || tmu >= NUM_TMUS) return;
    LOCK();
    if (start < 0) start = 0;
    if (end > 255) end = 255;
    if (end >= start) {
        memcpy(&G.palette[tmu][start], (FxU32 *)data + start, (size_t)(end - start + 1) * 4);
        G.palette_dirty[tmu] = 1;
    }
    UNLOCK();
}

void GAPI grTexFilterMode(FxI32 tmu, FxI32 minf, FxI32 magf)
{
    if (tmu < 0 || tmu >= NUM_TMUS) return;
    LOCK();
    if (minf != G.tex_filter_min[tmu] || magf != G.tex_filter_mag[tmu]) {
        d3d_flush(); G.tex_filter_min[tmu] = minf; G.tex_filter_mag[tmu] = magf; G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grTexClampMode(FxI32 tmu, FxI32 s, FxI32 t)
{
    if (tmu < 0 || tmu >= NUM_TMUS) return;
    LOCK();
    if (s != G.tex_clamp_s[tmu] || t != G.tex_clamp_t[tmu]) {
        d3d_flush(); G.tex_clamp_s[tmu] = s; G.tex_clamp_t[tmu] = t; G.dirty = 1;
    }
    UNLOCK();
}

void GAPI grTexMipMapMode(FxI32 tmu, FxI32 mode, FxBool lodBlend)
{
    (void)lodBlend;
    if (tmu < 0 || tmu >= NUM_TMUS) return;
    LOCK();
    if (mode != G.tex_mipmode[tmu]) { d3d_flush(); G.tex_mipmode[tmu] = mode; G.dirty = 1; }
    UNLOCK();
}

void GAPI grTexLodBiasValue(FxI32 tmu, float bias)
{
    if (tmu < 0 || tmu >= NUM_TMUS) return;
    LOCK();
    if (bias != G.tex_lodbias[tmu]) { d3d_flush(); G.tex_lodbias[tmu] = bias; G.dirty = 1; }
    UNLOCK();
}

/* ------------------------------------------------------------------------- */
/* linear frame buffer                                                       */
/* ------------------------------------------------------------------------- */
static uint16_t to565(uint32_t argb)
{
    return (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));
}

FxBool GAPI grLfbLock(FxI32 type, FxI32 buffer, FxI32 writeMode, FxI32 origin, FxBool pipeline, GrLfbInfo_t *info)
{
    int buf;
    static int logged_mode, logged_aux;
    (void)origin; (void)pipeline;
    if (!info) return 0;
    CALLER();
    LOCK();
    if (NBA99_DEBUG) trace_lfb(type == GR_LFB_WRITE_ONLY ? "lfb lock write" : "lfb lock read", (int)buffer, (int)writeMode, 0, 0, 0, g_caller);
    if (buffer == GR_BUFFER_AUXBUFFER) {
        if (!logged_aux++) glog("grLfbLock on the aux (depth) buffer: returning a scratch buffer");
        info->lfbPtr = G.lock_conv;
        info->strideInBytes = FB_W * 2;
        info->writeMode = GR_LFBWRITEMODE_ZA16;
        info->origin = GR_ORIGIN_UPPER_LEFT;
        G.lock_count++;
        UNLOCK();
        return 1;
    }
    buf = buffer == GR_BUFFER_FRONTBUFFER ? 0 : 1;
    if (G.open) {
        d3d_flush();
        if (G.gpu_newer[buf]) d3d_lfb_sync_to_shadow(buf);
    }
    info->origin = GR_ORIGIN_UPPER_LEFT;
    if (type == GR_LFB_WRITE_ONLY && writeMode != GR_LFBWRITEMODE_565 && writeMode != GR_LFBWRITEMODE_ANY) {
        /* other write formats go to a scratch buffer and are converted on unlock */
        int bpp = (writeMode == GR_LFBWRITEMODE_888 || writeMode == GR_LFBWRITEMODE_8888) ? 4 : 2;
        if (!logged_mode++) glog("grLfbLock: write mode %ld (converted on unlock)", (long)writeMode);
        info->lfbPtr = G.lock_conv;
        info->strideInBytes = FB_W * bpp;
        info->writeMode = writeMode;
        G.lock_write_mode = writeMode;
        /* start from the current image so untouched pixels survive */
        if (bpp == 4) {
            int i;
            uint32_t *d = (uint32_t *)G.lock_conv;
            for (i = 0; i < FB_W * FB_H; i++) {
                uint16_t p = G.shadow[buf][i];
                d[i] = 0xFF000000u | ((p & 0xF800u) << 8) | ((p & 0x07E0u) << 5) | ((p & 0x001Fu) << 3);
            }
        } else {
            int i;
            uint16_t *d = (uint16_t *)G.lock_conv;
            for (i = 0; i < FB_W * FB_H; i++) {
                uint16_t p = G.shadow[buf][i];
                d[i] = (uint16_t)(0x8000u | ((p & 0xF800u) >> 1) | ((p & 0x07C0u) >> 1) | (p & 0x1Fu));
            }
        }
    } else {
        info->lfbPtr = G.shadow[buf];
        info->strideInBytes = FB_W * 2;
        info->writeMode = GR_LFBWRITEMODE_565;
        G.lock_write_mode = GR_LFBWRITEMODE_565;
    }
    if (type == GR_LFB_WRITE_ONLY) G.lock_write_buffer = buf;
    G.lock_count++;
    UNLOCK();
    return 1;
}

FxBool GAPI grLfbUnlock(FxI32 type, FxI32 buffer)
{
    int buf = buffer == GR_BUFFER_FRONTBUFFER ? 0 : 1;
    CALLER();
    LOCK();
    if (NBA99_DEBUG) trace_lfb(type == GR_LFB_WRITE_ONLY ? "lfb unlock write" : "lfb unlock read", (int)buffer, 0, 0, 0, 0, g_caller);
    if (G.lock_count > 0) G.lock_count--;
    if (buffer != GR_BUFFER_AUXBUFFER && type == GR_LFB_WRITE_ONLY) {
        if (G.lock_write_mode != GR_LFBWRITEMODE_565) {
            int i;
            if (G.lock_write_mode == GR_LFBWRITEMODE_888 || G.lock_write_mode == GR_LFBWRITEMODE_8888) {
                const uint32_t *s = (const uint32_t *)G.lock_conv;
                for (i = 0; i < FB_W * FB_H; i++) G.shadow[buf][i] = to565(s[i]);
            } else {
                const uint16_t *s = (const uint16_t *)G.lock_conv;
                for (i = 0; i < FB_W * FB_H; i++) {
                    uint16_t p = s[i];   /* 555 / 1555 */
                    G.shadow[buf][i] = (uint16_t)(((p & 0x7C00u) << 1) | ((p & 0x03E0u) << 1) | (p & 0x1Fu));
                }
            }
            G.lock_write_mode = GR_LFBWRITEMODE_565;
        }
        if (G.open) d3d_lfb_upload(buf, 0, 0, FB_W, FB_H);
        if (buf == 0) {
            G.front_dirty = 1;
            d3d_present_front_if_needed(0);
        }
    }
    UNLOCK();
    return 1;
}

FxBool GAPI grLfbReadRegion(FxI32 src, FxU32 x, FxU32 y, FxU32 w, FxU32 h, FxU32 stride, void *dst)
{
    int buf = src == GR_BUFFER_FRONTBUFFER ? 0 : 1;
    FxU32 row;
    if (src == GR_BUFFER_AUXBUFFER || !dst || x + w > FB_W || y + h > FB_H) return 0;
    LOCK();
    if (G.open) { d3d_flush(); if (G.gpu_newer[buf]) d3d_lfb_sync_to_shadow(buf); }
    for (row = 0; row < h; row++)
        memcpy((uint8_t *)dst + row * stride, G.shadow[buf] + (y + row) * FB_W + x, w * 2);
    UNLOCK();
    return 1;
}

FxBool GAPI grLfbWriteRegion(FxI32 dstbuf, FxU32 x, FxU32 y, FxI32 fmt, FxU32 w, FxU32 h, FxI32 stride, void *src)
{
    int buf = dstbuf == GR_BUFFER_FRONTBUFFER ? 0 : 1;
    FxU32 row, col;
    if (dstbuf == GR_BUFFER_AUXBUFFER || !src || x + w > FB_W || y + h > FB_H) return 0;
    CALLER();
    LOCK();
    if (NBA99_DEBUG) trace_lfb("lfb write region", (int)dstbuf, (int)x, (int)y, (int)w, (int)h, g_caller);
    if (G.open) { d3d_flush(); if (G.gpu_newer[buf]) d3d_lfb_sync_to_shadow(buf); }
    for (row = 0; row < h; row++) {
        uint16_t *d = G.shadow[buf] + (y + row) * FB_W + x;
        const uint8_t *s = (const uint8_t *)src + row * stride;
        if (fmt == GR_LFBWRITEMODE_565) memcpy(d, s, w * 2);
        else if (fmt == GR_LFBWRITEMODE_888 || fmt == GR_LFBWRITEMODE_8888)
            for (col = 0; col < w; col++) d[col] = to565(((const uint32_t *)s)[col]);
        else
            for (col = 0; col < w; col++) {
                uint16_t p = ((const uint16_t *)s)[col];
                d[col] = (uint16_t)(((p & 0x7C00u) << 1) | ((p & 0x03E0u) << 1) | (p & 0x1Fu));
            }
    }
    if (G.open) d3d_lfb_upload(buf, x, y, w, h);
    if (buf == 0) { G.front_dirty = 1; d3d_present_front_if_needed(0); }
    UNLOCK();
    return 1;
}

FxBool GAPI guFbReadRegion(int x, int y, int w, int h, void *dst, int stride)
{
    return grLfbReadRegion(G.render_buffer == 0 ? 0 : 1, x, y, w, h, stride, dst);
}

FxBool GAPI guFbWriteRegion(int x, int y, int w, int h, void *src, int stride)
{
    return grLfbWriteRegion(G.render_buffer == 0 ? 0 : 1, x, y, GR_LFBWRITEMODE_565, w, h, stride, src);
}

/* ------------------------------------------------------------------------- */
/* DLL entry                                                                 */
/* ------------------------------------------------------------------------- */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char *s, path[MAX_PATH];
        SYSTEMTIME st;
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        QueryPerformanceFrequency(&g_qpf);
        QueryPerformanceCounter(&g_qpc0);
        GetModuleFileNameA(inst, g_dir, sizeof g_dir);
        s = strrchr(g_dir, '\\');
        if (s) s[1] = 0;
        load_config();
        if (G.cfg_log) {
            char mark[32], env[32];
            nba99_log_path(g_dir, "glide2x.log", path, sizeof path);
            /* the game loads us twice (detect, then for real): start a new log on the first
               load in this process, append on the second so both are kept */
            _snprintf(mark, sizeof mark, "%lu", GetCurrentProcessId());
            if (GetEnvironmentVariableA("NBA99_GLIDE_LOG", env, sizeof env) && !strcmp(env, mark))
                g_logf = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            else
                g_logf = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            SetEnvironmentVariableA("NBA99_GLIDE_LOG", mark);
        }
        GetLocalTime(&st);
        glog("---- NBA Live 99 glide2x (D3D11) " NBA99_VERSION_LINE " loaded %04d-%02d-%02d %02d:%02d:%02d ----",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    } else if (reason == DLL_PROCESS_DETACH) {
        glog("unloading%s", reserved ? " (process exit)" : "");
        if (!reserved) {
            /* FreeLibrary after the detect pass: release everything */
            win_detach();
            d3d_close();
            d3d_release_textures();
        }
        if (g_logf != INVALID_HANDLE_VALUE) CloseHandle(g_logf);
        g_logf = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}
NBA99_C_END
