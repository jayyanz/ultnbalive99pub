/*
 * glide2x.dll for NBA Live 99 - Glide 2.x on Direct3D 11.
 * Internal header shared by gl_api.cpp (Glide API, state, batching) and
 * gl_d3d.cpp (device, shaders, render targets, LFB, presentation).
 */
#ifndef GL_INTERNAL_H
#define GL_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include "nba99_cxx.h"   /* C++ build settings, first (0.7.15) */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include "nba99_build.h"

NBA99_C_BEGIN
#define GAPI __stdcall
typedef int FxBool;
typedef uint32_t FxU32;
typedef int32_t FxI32;
typedef uint16_t FxU16;
typedef uint8_t FxU8;
typedef float FxFloat;

/* ---- Glide 2 constants used here --------------------------------------- */
#define GLIDE_NUM_TMU 3

/* combine functions */
#define GR_COMBINE_FUNCTION_ZERO 0x0
#define GR_COMBINE_FUNCTION_LOCAL 0x1
#define GR_COMBINE_FUNCTION_LOCAL_ALPHA 0x2
#define GR_COMBINE_FUNCTION_SCALE_OTHER 0x3
#define GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL 0x4
#define GR_COMBINE_FUNCTION_SCALE_OTHER_ADD_LOCAL_ALPHA 0x5
#define GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL 0x6
#define GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL 0x7
#define GR_COMBINE_FUNCTION_SCALE_OTHER_MINUS_LOCAL_ADD_LOCAL_ALPHA 0x8
#define GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL 0x9
#define GR_COMBINE_FUNCTION_SCALE_MINUS_LOCAL_ADD_LOCAL_ALPHA 0x10

/* buffers */
#define GR_BUFFER_FRONTBUFFER 0
#define GR_BUFFER_BACKBUFFER 1
#define GR_BUFFER_AUXBUFFER 2

/* depth buffer modes */
#define GR_DEPTHBUFFER_DISABLE 0
#define GR_DEPTHBUFFER_ZBUFFER 1
#define GR_DEPTHBUFFER_WBUFFER 2
#define GR_DEPTHBUFFER_ZBUFFER_COMPARE_TO_BIAS 3
#define GR_DEPTHBUFFER_WBUFFER_COMPARE_TO_BIAS 4

/* cmp */
#define GR_CMP_NEVER 0
#define GR_CMP_ALWAYS 7

/* cull */
#define GR_CULL_DISABLE 0
#define GR_CULL_NEGATIVE 1
#define GR_CULL_POSITIVE 2

/* lfb */
#define GR_LFB_READ_ONLY 0
#define GR_LFB_WRITE_ONLY 1
#define GR_LFBWRITEMODE_565 0x0
#define GR_LFBWRITEMODE_555 0x1
#define GR_LFBWRITEMODE_1555 0x2
#define GR_LFBWRITEMODE_888 0x4
#define GR_LFBWRITEMODE_8888 0x5
#define GR_LFBWRITEMODE_565_DEPTH 0xc
#define GR_LFBWRITEMODE_555_DEPTH 0xd
#define GR_LFBWRITEMODE_1555_DEPTH 0xe
#define GR_LFBWRITEMODE_ZA16 0xf
#define GR_LFBWRITEMODE_ANY 0xff
#define GR_ORIGIN_UPPER_LEFT 0
#define GR_ORIGIN_LOWER_LEFT 1

/* textures */
#define GR_TEXFMT_RGB_332 0x0
#define GR_TEXFMT_YIQ_422 0x1
#define GR_TEXFMT_ALPHA_8 0x2
#define GR_TEXFMT_INTENSITY_8 0x3
#define GR_TEXFMT_ALPHA_INTENSITY_44 0x4
#define GR_TEXFMT_P_8 0x5
#define GR_TEXFMT_ARGB_8332 0x8
#define GR_TEXFMT_AYIQ_8422 0x9
#define GR_TEXFMT_RGB_565 0xa
#define GR_TEXFMT_ARGB_1555 0xb
#define GR_TEXFMT_ARGB_4444 0xc
#define GR_TEXFMT_ALPHA_INTENSITY_88 0xd
#define GR_TEXFMT_AP_88 0xe
#define GR_TEXTABLE_NCC0 0x0
#define GR_TEXTABLE_NCC1 0x1
#define GR_TEXTABLE_PALETTE 0x2
#define GR_MIPMAP_DISABLE 0x0
#define GR_TEXTUREFILTER_POINT_SAMPLED 0x0
#define GR_TEXTURECLAMP_WRAP 0x0
#define GR_TEXTURECLAMP_CLAMP 0x1

/* ---- Glide 2 structures ------------------------------------------------ */
typedef struct { float sow, tow, oow; } GrTmuVertex;
typedef struct {
    float x, y, z;
    float r, g, b;
    float ooz;
    float a;
    float oow;
    GrTmuVertex tmuvtx[GLIDE_NUM_TMU];
} GrVertex;

typedef struct {
    FxI32 smallLod, largeLod, aspectRatio, format;
    void *data;
} GrTexInfo;

typedef struct {
    FxI32 size;
    void *lfbPtr;
    FxU32 strideInBytes;
    FxI32 writeMode;
    FxI32 origin;
} GrLfbInfo_t;

typedef struct { FxI32 tmuRev, tmuRam; } GrTMUConfig_t;
typedef struct {
    FxI32 fbRam, fbiRev, nTexelfx;
    FxBool sliDetect;
    GrTMUConfig_t tmuConfig[GLIDE_NUM_TMU];
} GrVoodooConfig_t;
typedef struct {
    FxI32 num_sst;
    struct {
        FxI32 type;
        union { GrVoodooConfig_t VoodooConfig; FxU8 pad[64]; } sstBoard;
    } SSTs[4];
} GrHwConfiguration;

/* ---- our state -------------------------------------------------------- */
#define FB_W 640
#define FB_H 480
#define TMU_MEM_SIZE (16u * 1024u * 1024u)   /* emulated texture memory per TMU */
#define NUM_TMUS 2

/* GPU vertex (what we upload) */
typedef struct {
    float x, y, ooz, oow;   /* Glide screen space */
    float r, g, b, a;       /* 0..255 */
    float s0, t0, s1, t1;   /* sow, tow per TMU (already /w) */
    float oow0, oow1;       /* per-TMU oow (for perspective) */
    float bias;             /* depth bias level */
    float pal0, pal1;       /* palette atlas rows */
    float pad;
} GVtx;

/* per-draw constants (must match cbuffer in shader) */
typedef struct {
    float vp[4];            /* 2/640, -2/480, 0, 0 */
    uint32_t cc[4];         /* color combine: func, factor, local, other */
    uint32_t ac[4];         /* alpha combine: func, factor, local, other */
    uint32_t inv[4];        /* color invert, alpha invert, depth mode, fog mode */
    uint32_t t0c[4];        /* tmu0: rgb func, rgb factor, a func, a factor */
    uint32_t t1c[4];        /* tmu1 */
    uint32_t tinv[4];       /* tmu0 rgb inv, tmu0 a inv, tmu1 rgb inv, tmu1 a inv */
    uint32_t tex0[4];       /* kind (0 none, 1 rgba, 2 p8, 3 ap88), width, height, levels */
    uint32_t tex1[4];
    uint32_t tsamp[4];      /* tmu0 filter, tmu0 mipmode, tmu1 filter, tmu1 mipmode */
    uint32_t tclamp[4];     /* tmu0 s, t, tmu1 s, t (1 = clamp) */
    float stScale[4];       /* tmu0 su, tv, tmu1 su, tv */
    float lodBias[4];       /* tmu0, tmu1 */
    float constColor[4];    /* 0..1 */
    uint32_t at[4];         /* alpha func, alpha ref, chroma enable, chroma key rgb */
    float fogColor[4];
    float misc[4];          /* depth bias scale, unused */
} GCB;

/* emulated TMU memory and texture cache */
typedef struct TexEntry {
    int tmu;
    FxU32 addr, size;
    FxI32 smallLod, largeLod, aspect, format;
    int width, height, levels, kind;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t last_used;
    int valid;
} TexEntry;

typedef struct {
    /* config */
    int cfg_width, cfg_height, cfg_windowed, cfg_vsync, cfg_linear, cfg_cull_invert;
    int cfg_fps_limit, cfg_log, cfg_dump_frame, cfg_menu_linear, cfg_overlay;
    int cfg_menu_fullres;           /* 99: menus over the full-resolution arena (gl_menulayer.inc) */
    int cfg_judder;             /* [glide] JudderLog (Debug): logs\<session>\judder.log */
    volatile long overlay_toggles;
    int cfg_step_lock, cfg_display_lock, cfg_depth_wrap;
    int cfg_queue_skip;           /* [glide] QueueSkip (0.7.19) */
    volatile long shot_req;         /* Page Down presses (0.7.12: screenshot at the next present) */
    int cfg_shots;                  /* [glide] Screenshots */
    volatile long trace_req;        /* Ctrl+F10 presses (Debug: trace the next frame's draws) */
    int trace_dump;                 /* save the picture of the frame just traced */
    LONGLONG qpf_hz;               /* present right after each new game input frame (match only) */
    int step_lock_on;                /* step lock active for the last frame */   /* Ctrl+F11 presses seen by the window procedure */
    float cfg_gamma;
    int cfg_crt;                /* CRT filter: 0 off, 1 auto, 2 sharp, 3 soft */
    int cfg_widescreen;         /* [glide] Widescreen: matches use the whole width of a wide screen */
    int ws;                     /* the render target is wide (widescreen on and the window wider than 4:3) */
    int c43_x, c43_w;           /* the 4:3 area inside a wide render target, in its pixels */
    int ws_layer;               /* 1: draws go across the full width (match 3D), 0: into the 4:3 area */
    DWORD ws_swap_tick;         /* GetTickCount() of the last buffer swap (only matches swap) */

    /* window / device */
    HWND hwnd;
    int open;
    int out_w, out_h;               /* swap chain size */
    int rt_w, rt_h;                 /* internal render target size (4:3) */
    int view_x, view_y, view_w, view_h;
    int pic_x, pic_y, pic_w, pic_h;   /* where the picture is drawn now (the CRT VGA preset draws it smaller); mouse mapping */
    float sx, sy;                   /* Glide pixels -> RT pixels */

    /* Glide state */
    int render_buffer;              /* 0 front, 1 back */
    GCB cb;                         /* shader constants for pending state */
    int cull_mode;
    int depth_mode, depth_func, depth_mask;
    int blend_rgb_sf, blend_rgb_df, blend_a_sf, blend_a_df;
    int color_mask_rgb, color_mask_a;
    int clip[4];                    /* minx, miny, maxx, maxy */
    float gamma;
    FxU32 const_color;
    int tex_filter_min[NUM_TMUS], tex_filter_mag[NUM_TMUS];
    int tex_clamp_s[NUM_TMUS], tex_clamp_t[NUM_TMUS];
    int tex_mipmode[NUM_TMUS];
    float tex_lodbias[NUM_TMUS];
    TexEntry *bound[NUM_TMUS];
    int pal_row[NUM_TMUS];
    FxU32 palette[NUM_TMUS][256];
    int palette_dirty[NUM_TMUS];
    int dirty;                      /* render state changed since last batch */

    /* emulated texture memory */
    uint8_t *tmu_mem[NUM_TMUS];

    /* LFB */
    uint16_t *shadow[2];            /* 640x480 RGB565 per colour buffer */
    int gpu_newer[2];               /* GPU render target has content not in shadow */
    int lock_count;
    int lock_write_buffer, lock_write_mode;
    void *lock_conv;                /* scratch for non-565 write modes */
    int front_dirty;                /* front buffer changed, not yet presented */
    double last_present_ms;
    double last_swap_ms;

    /* stats */
    uint32_t frame;
    uint32_t draws, tris;
    int snap_seen;
    int depth_wrap_seen;
    float depth_wrap_last;
} GState;

extern GState G;
extern CRITICAL_SECTION g_lock;

/* logging */
void glog(const char *fmt, ...);
double gnow_ms(void);
void jd_tri(const GrVertex *a, const GrVertex *b, const GrVertex *c, uint32_t caller);   /* gl_judder.inc */

/* backend (gl_d3d.cpp) */
int  d3d_open(HWND hwnd);
void d3d_close(void);
void d3d_flush(void);
void d3d_ws_layer(void);
void d3d_add_tris(const GVtx *v, int nverts);       /* multiple of 3 */
void d3d_state_changed(void);
void d3d_clear(FxU32 color, FxU32 alpha, FxU32 depth);
void d3d_swap(int interval);
void d3d_present_front_if_needed(int force);
void d3d_lfb_sync_to_shadow(int buf);                /* GPU -> shadow */
void d3d_lfb_upload(int buf, int x, int y, int w, int h); /* shadow -> GPU */
TexEntry *d3d_tex_get(int tmu, FxU32 addr, const GrTexInfo *info);
void d3d_tex_invalidate(int tmu, FxU32 addr, FxU32 size);
int  d3d_palette_row(const FxU32 *pal);
void d3d_release_textures(void);

/* window glue (gl_win.cpp) */
void win_attach(HWND hwnd);
void win_detach(void);

/* texture helpers (gl_api.cpp) */
/* draw trace (gl_api.cpp, Debug) */
void trace_lfb(const char *what, int buf, int x, int y, int w, int h, uint32_t caller);

void tex_dims(FxI32 lod, FxI32 aspect, int *w, int *h);
FxU32 tex_level_bytes(int w, int h, FxI32 format);
FxU32 tex_mem_required(FxI32 smallLod, FxI32 largeLod, FxI32 aspect, FxI32 format);
int tex_bpp(FxI32 format);

NBA99_C_END
#endif
