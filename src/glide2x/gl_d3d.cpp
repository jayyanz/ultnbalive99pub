/*
 * glide2x.dll for NBA Live 99 - Direct3D 11 backend.
 *
 * Two colour render targets (Glide front and back buffer) and one depth
 * buffer at the internal resolution (the largest 4:3 area of the output),
 * drawn with one uber-shader that implements Glide's colour/alpha/texture
 * combine, alpha test, chroma key and 16-bit depth semantics. Paletted
 * textures stay 8-bit and are looked up in the shader from a palette atlas, so
 * a palette change never forces a texture rebuild or a new draw call.
 *
 * The linear frame buffer is a CPU copy of each colour buffer at 640x480
 * RGB565: writes are uploaded once on unlock, reads come from the copy and
 * only fall back to a GPU readback when the GPU has newer content.
 *
 * Presentation: the window is made borderless and fills its monitor; the 4:3
 * image is scaled into the centre. Frame pacing defaults to the 60 Hz the game
 * asks for in grSstWinOpen, independent of the monitor's refresh rate.
 */
#include "nba99_cxx.h"   /* C++ build settings, first (0.7.15) */
#include "gl_internal.h"
#include "gl_shaders.h"
#include "nba99_stats.h"
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <dxgi1_3.h>     /* IDXGISwapChainMedia (0.7.18) */
#include <stdio.h>
#include <string.h>
#include <math.h>

NBA99_C_BEGIN
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *sc;
static ID3D11RenderTargetView *bb_rtv;
static ID3D11Texture2D *bb_tex;

static ID3D11Texture2D *rt_tex[2];
static ID3D11RenderTargetView *rt_rtv[2];
static ID3D11ShaderResourceView *rt_srv[2];
static ID3D11Texture2D *ds_tex;
static ID3D11DepthStencilView *ds_dsv;
static int front_idx;                  /* which rt_* is the Glide front buffer */

static ID3D11VertexShader *vs_main, *vs_full;
static ID3D11PixelShader *ps_main, *ps_clear, *ps_lfb, *ps_readback, *ps_present, *ps_overlay;
static ID3D11InputLayout *layout;
static ID3D11Buffer *vb, *cb_main, *cb_util;
static ID3D11RasterizerState *rs;
static ID3D11SamplerState *smp_point, *smp_linear;
static ID3D11DepthStencilState *ds_clear, *ds_off;
static ID3D11BlendState *bs_off;
static ID3D11BlendState *util_bs;   /* blend state for util_pass (overlay), NULL = off */

/* LFB upload / readback */
static ID3D11Texture2D *lfb_tex;
static ID3D11ShaderResourceView *lfb_srv;
/* menu layer (gl_menulayer.inc): the 99 menus over the full-resolution arena */
static ID3D11PixelShader *ps_menu;
static void ml_create(void);
static void ml_release(void);
static int g_probe_all = -1;   /* Debug: NBA99_LFB_PROBE=1 logs every LFB write and every swap after a gap */
static ID3D11Texture2D *rb_tex, *rb_staging;
static ID3D11RenderTargetView *rb_rtv;

/* palette atlas */
#define PAL_ROWS 1024
static ID3D11Texture2D *pal_tex;
static ID3D11PixelShader *ps_crt_down, *ps_crt_hyllian, *ps_crt_vga;   /* CRT filter (optional) */
static ID3D11PixelShader *ps_navbox;   /* gamepad menu selection frame (optional) */
static ID3D11Texture2D *crt_tex;
static ID3D11RenderTargetView *crt_rtv;
static ID3D11ShaderResourceView *crt_srv;
static int crt_w;   /* width of the CRT source picture (480 lines): 640, or wider in widescreen (0.7.9) */
static ID3D11ShaderResourceView *pal_srv;
static struct { uint64_t hash; int used; } pal_map[PAL_ROWS];
static int pal_next;

/* batch */
#define VB_VERTS 65536
static GVtx pend[VB_VERTS];
static int npend;
static int vb_pos;
static int batch_target = -1;          /* 0 front / 1 back for the pending batch */

/* state object caches */
#define MAX_STATES 64
static struct { uint32_t key; ID3D11BlendState *s; } blend_cache[MAX_STATES];
static int nblend;
static struct { uint32_t key; ID3D11DepthStencilState *s; } depth_cache[MAX_STATES];
static int ndepth;
static struct { uint32_t key; ID3D11SamplerState *s; } samp_cache[MAX_STATES];
static int nsamp;

/* textures */
static TexEntry **tex_list;
static int tex_count, tex_cap;
static uint32_t tex_clock;

static uint32_t present_count;

#define REL(x) do { if (x) { IUnknown_Release((IUnknown *)(x)); (x) = NULL; } } while (0)

/* ------------------------------------------------------------------------- */
/* shaders                                                                   */
/* ------------------------------------------------------------------------- */
static pD3DCompile p_compile;

static ID3DBlob *compile(const char *src, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    if (!p_compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (!m) m = LoadLibraryA("d3dcompiler_43.dll");
        if (m) p_compile = (pD3DCompile)GetProcAddress(m, "D3DCompile");
        if (!p_compile) { glog("ERROR: no d3dcompiler_47.dll / d3dcompiler_43.dll"); return NULL; }
    }
    hr = p_compile(src, strlen(src), entry, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        glog("ERROR: shader %s failed to compile (0x%08lx): %s", entry, (unsigned long)hr,
             err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        REL(err);
        return NULL;
    }
    REL(err);
    return code;
}

static int create_shaders(void)
{
    ID3DBlob *b;
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (!(b = compile(k_hlsl_main, "vs_main", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &vs_main);
    ID3D11Device_CreateInputLayout(dev, el, 5, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &layout);
    REL(b);
    if (!(b = compile(k_hlsl_main, "ps_main", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_main);
    REL(b);
#define UTIL(entry, target, kind, out) \
    if (!(b = compile(k_hlsl_util, entry, target))) return 0; \
    ID3D11Device_Create##kind##Shader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, out); REL(b);
    UTIL("vs_full", "vs_4_0", Vertex, &vs_full);
    UTIL("ps_clear", "ps_4_0", Pixel, &ps_clear);
    UTIL("ps_lfb", "ps_4_0", Pixel, &ps_lfb);
    UTIL("ps_menu", "ps_4_0", Pixel, &ps_menu);
    UTIL("ps_readback", "ps_4_0", Pixel, &ps_readback);
    UTIL("ps_present", "ps_4_0", Pixel, &ps_present);
    UTIL("ps_overlay", "ps_4_0", Pixel, &ps_overlay);
#undef UTIL
    /* the CRT filter is optional: if its shaders do not compile, it is simply off */
#define CRT(entry, out) \
    if ((b = compile(k_hlsl_crt, entry, "ps_4_0")) != NULL) { \
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, out); REL(b); }
    CRT("ps_crt_down", &ps_crt_down);
    CRT("ps_crt_hyllian", &ps_crt_hyllian);
    CRT("ps_crt_vga", &ps_crt_vga);
#undef CRT
    if ((b = compile(k_hlsl_util, "ps_navbox", "ps_4_0")) != NULL) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_navbox);
        REL(b);
    }
    if (!ps_crt_down || !ps_crt_hyllian || !ps_crt_vga) glog("CRT filter: shaders not available, filter off");
    return vs_main && ps_main && layout && vs_full && ps_clear && ps_lfb && ps_readback && ps_present;
}

/* ------------------------------------------------------------------------- */
/* resources                                                                 */
/* ------------------------------------------------------------------------- */
static void make_tex2d(int w, int h, DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpu,
                       ID3D11Texture2D **t, ID3D11ShaderResourceView **srv, ID3D11RenderTargetView **rtv)
{
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
    d.SampleDesc.Count = 1; d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, t);
    if (*t && srv) ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)*t, NULL, srv);
    if (*t && rtv) ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)*t, NULL, rtv);
}

static void create_resources(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC sd;
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_BLEND_DESC bld;
    D3D11_TEXTURE2D_DESC td;
    int i;

    for (i = 0; i < 2; i++)
        make_tex2d(G.rt_w, G.rt_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                   D3D11_USAGE_DEFAULT, 0, &rt_tex[i], &rt_srv[i], &rt_rtv[i]);
    ml_create();
    memset(&td, 0, sizeof td);
    td.Width = G.rt_w; td.Height = G.rt_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &ds_tex);
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds_tex, NULL, &ds_dsv);

    make_tex2d(FB_W, FB_H, DXGI_FORMAT_R16_UINT, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC,
               D3D11_CPU_ACCESS_WRITE, &lfb_tex, &lfb_srv, NULL);
    make_tex2d(FB_W, FB_H, DXGI_FORMAT_R16_UINT, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0,
               &rb_tex, NULL, &rb_rtv);
    make_tex2d(FB_W, FB_H, DXGI_FORMAT_R16_UINT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ,
               &rb_staging, NULL, NULL);
    make_tex2d(FB_W, FB_H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
               D3D11_USAGE_DEFAULT, 0, &crt_tex, &crt_srv, &crt_rtv);
    crt_w = FB_W;
    make_tex2d(256, PAL_ROWS, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0,
               &pal_tex, &pal_srv, NULL);
    memset(pal_map, 0, sizeof pal_map);
    pal_next = 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(GVtx) * VB_VERTS; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &vb);
    bd.ByteWidth = (sizeof(GCB) + 15) & ~15u; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb_main);
    bd.ByteWidth = 48;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb_util);
    vb_pos = 0;

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.ScissorEnable = TRUE; rd.DepthClipEnable = FALSE;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11Device_CreateSamplerState(dev, &sd, &smp_point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(dev, &sd, &smp_linear);

    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &ds_clear);
    dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ID3D11Device_CreateDepthStencilState(dev, &dd, &ds_off);

    memset(&bld, 0, sizeof bld);
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(dev, &bld, &bs_off);
}

/* ------------------------------------------------------------------------- */
/* window and device                                                         */
/* ------------------------------------------------------------------------- */
static nba99_stats *core_stats(void);
static int crt_mode(void);

static void fit43(int w, int h, int *ow, int *oh)
{
    if (w * 3 >= h * 4) { *oh = h; *ow = h * 4 / 3; }
    else { *ow = w; *oh = w * 3 / 4; }
}

/* where the image goes inside the current output: the render target's shape, centred
   (4:3 pillarbox/letterbox, or the whole of a wide window in widescreen) */
static void compute_view(void)
{
    if (G.ws && G.rt_h > 0) {
        if ((double)G.out_w * G.rt_h >= (double)G.rt_w * G.out_h) {
            G.view_h = G.out_h; G.view_w = (int)((double)G.out_h * G.rt_w / G.rt_h + 0.5);
        } else {
            G.view_w = G.out_w; G.view_h = (int)((double)G.out_w * G.rt_h / G.rt_w + 0.5);
        }
    } else {
        fit43(G.out_w, G.out_h, &G.view_w, &G.view_h);
    }
    G.view_x = (G.out_w - G.view_w) / 2;
    G.view_y = (G.out_h - G.view_h) / 2;
}

/* internal render size: Width/Height from the ini (made 4:3), else the 4:3 part of the window.
   Widescreen (0.7.5): the same height, as wide as the window's shape; the game's 640x480
   screen then sits in the middle (c43_x, c43_w) and match 3D is spread over the full width. */
static void compute_layout(void)
{
    if (G.cfg_width > 0 && G.cfg_height > 0) fit43(G.cfg_width, G.cfg_height, &G.rt_w, &G.rt_h);
    else fit43(G.out_w, G.out_h, &G.rt_w, &G.rt_h);
    if (G.rt_w < FB_W) { G.rt_w = FB_W; G.rt_h = FB_H; }
    G.ws = 0;
    G.c43_x = 0; G.c43_w = G.rt_w;
    if (G.cfg_widescreen && G.out_h > 0 && G.out_w * 3 > G.out_h * 4 + G.out_h / 50) {
        int w = (int)((double)G.rt_h * G.out_w / G.out_h + 0.5) & ~1;
        if (w > G.rt_w && w <= 16384) {
            G.ws = 1;
            G.c43_w = G.rt_w;
            G.rt_w = w;
            G.c43_x = (G.rt_w - G.c43_w) / 2;
        }
    }
    G.sx = (float)G.c43_w / FB_W;
    G.sy = (float)G.rt_h / FB_H;
    compute_view();
}

/* follow the window's real client size (the game or the window manager may resize it) */
static void track_client_size(void)
{
    RECT r;
    int w, h;
    if (!sc || !GetClientRect(G.hwnd, &r)) return;
    w = r.right - r.left; h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || (w == G.out_w && h == G.out_h)) return;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    REL(bb_rtv);
    REL(bb_tex);
    if (FAILED(IDXGISwapChain_ResizeBuffers(sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        glog("ResizeBuffers(%d x %d) failed", w, h);
    } else {
        G.out_w = w; G.out_h = h;
    }
    IDXGISwapChain_GetBuffer(sc, 0, IID_ID3D11Texture2D, (void **)&bb_tex);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb_tex, NULL, &bb_rtv);
    compute_view();
    glog("window is now %dx%d, image %dx%d at %d,%d", G.out_w, G.out_h, G.view_w, G.view_h, G.view_x, G.view_y);
}

static void front_flush_start(void);
static void front_flush_stop(void);

int d3d_open(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr;
    HMONITOR mon;
    MONITORINFO mi;
    IDXGIFactory *fac = NULL;
    IDXGIDevice *dxdev = NULL;
    IDXGIAdapter *ad = NULL;
    int i;

    {   /* real pixels, not DPI-scaled ones */
        HMODULE u = GetModuleHandleA("user32.dll");
        BOOL (WINAPI *aware)(void) = u ? (BOOL (WINAPI *)(void))GetProcAddress(u, "SetProcessDPIAware") : NULL;
        if (aware) aware();
    }
    G.hwnd = hwnd;
    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(mon, &mi);
    if (G.cfg_windowed) {
        RECT r;
        G.out_w = G.cfg_width > 0 ? G.cfg_width : 1280;
        G.out_h = G.cfg_height > 0 ? G.cfg_height : 960;
        r.left = 0; r.top = 0; r.right = G.out_w; r.bottom = G.out_h;
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcWork.left + 40, mi.rcWork.top + 40, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        /* borderless window covering the monitor (step 3: desktop resolution) */
        G.out_w = mi.rcMonitor.right - mi.rcMonitor.left;
        G.out_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, G.out_w, G.out_h,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    compute_layout();

    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2;
    sd.BufferDesc.Width = G.out_w;
    sd.BufferDesc.Height = G.out_h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3, D3D11_SDK_VERSION,
                                       &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        glog("flip-model swap chain failed (0x%08lx), trying the legacy one", (unsigned long)hr);
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        sd.BufferCount = 1;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3, D3D11_SDK_VERSION,
                                           &sd, &sc, &dev, &fl, &ctx);
    }
    if (FAILED(hr)) {
        glog("ERROR: could not create a Direct3D 11 device (0x%08lx)", (unsigned long)hr);
        return 0;
    }
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, IID_IDXGIDevice, (void **)&dxdev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxdev, &ad))) {
        DXGI_ADAPTER_DESC ade;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &ade))) {
            char name[128];
            WideCharToMultiByte(CP_UTF8, 0, ade.Description, -1, name, sizeof name, NULL, NULL);
            glog("Direct3D 11 device: %s, feature level 0x%x", name, fl);
        }
        if (SUCCEEDED(IDXGIAdapter_GetParent(ad, IID_IDXGIFactory, (void **)&fac))) {
            IDXGIFactory_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            REL(fac);
        }
    }
    REL(ad);
    REL(dxdev);
    IDXGISwapChain_GetBuffer(sc, 0, IID_ID3D11Texture2D, (void **)&bb_tex);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb_tex, NULL, &bb_rtv);
    glog("output %dx%d (%s), 3D rendered at %dx%d, image at %d,%d", G.out_w, G.out_h,
         G.cfg_windowed ? "window" : "borderless full screen", G.rt_w, G.rt_h, G.view_x, G.view_y);

    if (!create_shaders()) { d3d_close(); return 0; }
    create_resources();
    front_idx = 0;
    for (i = 0; i < 2; i++) {
        static const float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rt_rtv[i], black);
    }
    ID3D11DeviceContext_ClearDepthStencilView(ctx, ds_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    npend = 0;
    batch_target = -1;
    G.dirty = 1;
    G.last_swap_ms = G.last_present_ms = gnow_ms();
    front_flush_start();
    return 1;
}

void d3d_release_textures(void)
{
    int i;
    for (i = 0; i < tex_count; i++) {
        REL(tex_list[i]->srv);
        REL(tex_list[i]->tex);
        HeapFree(GetProcessHeap(), 0, tex_list[i]);
    }
    tex_count = 0;
    G.bound[0] = G.bound[1] = NULL;
}

static void ov_release(void);

static void present_stats_release(void);

void d3d_close(void)
{
    int i;
    if (!dev) return;
    front_flush_stop();
    present_stats_release();
    if (ctx) ID3D11DeviceContext_ClearState(ctx);
    d3d_release_textures();
    ov_release();
    for (i = 0; i < nblend; i++) REL(blend_cache[i].s);
    for (i = 0; i < ndepth; i++) REL(depth_cache[i].s);
    for (i = 0; i < nsamp; i++) REL(samp_cache[i].s);
    nblend = ndepth = nsamp = 0;
    for (i = 0; i < 2; i++) { REL(rt_rtv[i]); REL(rt_srv[i]); REL(rt_tex[i]); }
    REL(ds_dsv); REL(ds_tex);
    REL(lfb_srv); REL(lfb_tex); REL(rb_rtv); REL(rb_tex); REL(rb_staging);
    ml_release();
    REL(pal_srv); REL(pal_tex);
    REL(crt_rtv); REL(crt_srv); REL(crt_tex); REL(ps_crt_down); REL(ps_crt_hyllian); REL(ps_crt_vga); REL(ps_navbox);
    REL(vb); REL(cb_main); REL(cb_util);
    REL(rs); REL(smp_point); REL(smp_linear); REL(ds_clear); REL(ds_off); REL(bs_off);
    REL(vs_main); REL(vs_full); REL(ps_main); REL(ps_clear); REL(ps_lfb); REL(ps_readback); REL(ps_present);
    REL(layout);
    REL(bb_rtv); REL(bb_tex);
    REL(sc);
    REL(ctx);
    REL(dev);
    npend = 0;
    glog("Direct3D closed");
}

/* ------------------------------------------------------------------------- */
/* state objects                                                             */
/* ------------------------------------------------------------------------- */
static D3D11_BLEND map_blend(int f, int dst, int alpha)
{
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_SRC_ALPHA;
    case 2: return dst ? (alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR)
                       : (alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR);
    case 3: return D3D11_BLEND_DEST_ALPHA;
    case 4: return D3D11_BLEND_ONE;
    case 5: return D3D11_BLEND_INV_SRC_ALPHA;
    case 6: return dst ? (alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR)
                       : (alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR);
    case 7: return D3D11_BLEND_INV_DEST_ALPHA;
    case 15: return dst ? (alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR) : D3D11_BLEND_SRC_ALPHA_SAT;
    default: return dst ? D3D11_BLEND_ZERO : D3D11_BLEND_ONE;
    }
}

static ID3D11BlendState *get_blend(void)
{
    uint32_t key = (G.blend_rgb_sf & 15) | ((G.blend_rgb_df & 15) << 4) | ((G.blend_a_sf & 15) << 8) |
                   ((G.blend_a_df & 15) << 12) | (G.color_mask_rgb << 16) | (G.color_mask_a << 17);
    D3D11_BLEND_DESC d;
    int i;
    for (i = 0; i < nblend; i++) if (blend_cache[i].key == key) return blend_cache[i].s;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = !(G.blend_rgb_sf == 4 && G.blend_rgb_df == 0 && G.blend_a_sf == 4 && G.blend_a_df == 0);
    d.RenderTarget[0].SrcBlend = map_blend(G.blend_rgb_sf, 0, 0);
    d.RenderTarget[0].DestBlend = map_blend(G.blend_rgb_df, 1, 0);
    d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = map_blend(G.blend_a_sf, 0, 1);
    d.RenderTarget[0].DestBlendAlpha = map_blend(G.blend_a_df, 1, 1);
    d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].RenderTargetWriteMask = (G.color_mask_rgb ? 7 : 0) | (G.color_mask_a ? 8 : 0);
    if (nblend == MAX_STATES) { REL(blend_cache[0].s); blend_cache[0] = blend_cache[--nblend]; }
    blend_cache[nblend].key = key;
    ID3D11Device_CreateBlendState(dev, &d, &blend_cache[nblend].s);
    return blend_cache[nblend++].s;
}

static ID3D11DepthStencilState *get_depth(void)
{
    int on = G.depth_mode != GR_DEPTHBUFFER_DISABLE;
    uint32_t key = (on ? 1 : 0) | ((G.depth_func & 7) << 1) | (G.depth_mask << 4);
    D3D11_DEPTH_STENCIL_DESC d;
    int i;
    for (i = 0; i < ndepth; i++) if (depth_cache[i].key == key) return depth_cache[i].s;
    memset(&d, 0, sizeof d);
    d.DepthEnable = on;
    d.DepthWriteMask = (on && G.depth_mask) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = (D3D11_COMPARISON_FUNC)((G.depth_func & 7) + 1);   /* GR_CMP_x + 1 = D3D11_COMPARISON_x */
    if (ndepth == MAX_STATES) { REL(depth_cache[0].s); depth_cache[0] = depth_cache[--ndepth]; }
    depth_cache[ndepth].key = key;
    ID3D11Device_CreateDepthStencilState(dev, &d, &depth_cache[ndepth].s);
    return depth_cache[ndepth++].s;
}

static ID3D11SamplerState *get_sampler(int t)
{
    int lin = G.tex_filter_min[t] || G.tex_filter_mag[t];
    int mip = G.tex_mipmode[t] != GR_MIPMAP_DISABLE;
    int bias = (int)(G.tex_lodbias[t] * 16.0f) & 0xFFF;
    uint32_t key = (lin ? 1 : 0) | (mip << 1) | (G.tex_clamp_s[t] << 2) | (G.tex_clamp_t[t] << 3) | (bias << 4);
    D3D11_SAMPLER_DESC d;
    int i;
    for (i = 0; i < nsamp; i++) if (samp_cache[i].key == key) return samp_cache[i].s;
    memset(&d, 0, sizeof d);
    /* Voodoo has no linear filtering between mip levels without trilinear mode */
    d.Filter = lin ? D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = G.tex_clamp_s[t] ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressV = G.tex_clamp_t[t] ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    d.MipLODBias = G.tex_lodbias[t];
    d.MaxLOD = mip ? D3D11_FLOAT32_MAX : 0.0f;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (nsamp == MAX_STATES) { REL(samp_cache[0].s); samp_cache[0] = samp_cache[--nsamp]; }
    samp_cache[nsamp].key = key;
    ID3D11Device_CreateSamplerState(dev, &d, &samp_cache[nsamp].s);
    return samp_cache[nsamp++].s;
}

/* ------------------------------------------------------------------------- */
/* drawing                                                                   */
/* ------------------------------------------------------------------------- */
static void set_target(int buf)
{
    int idx = buf == 0 ? front_idx : 1 - front_idx;
    D3D11_VIEWPORT vp;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rt_rtv[idx], ds_dsv);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)G.rt_w; vp.Height = (float)G.rt_h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}

/* the clip window in render target pixels (widescreen: full width for match 3D, else the 4:3 area) */
static void clip_rect(D3D11_RECT *r)
{
    float sx = G.sx, ox = (float)G.c43_x;
    if (G.ws && G.ws_layer) { sx = (float)G.rt_w / FB_W; ox = 0; }
    r->left = (LONG)floorf(G.clip[0] * sx + ox + 0.5f);
    r->top = (LONG)floorf(G.clip[1] * G.sy + 0.5f);
    r->right = (LONG)floorf(G.clip[2] * sx + ox + 0.5f);
    r->bottom = (LONG)floorf(G.clip[3] * G.sy + 0.5f);
}

static void set_scissor(void)
{
    D3D11_RECT r;
    clip_rect(&r);
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r);
}

/* widescreen: which way the next draw goes. Full width (1) for 3D in a match: buffer swaps
   seen in the last half second (the front end never swaps) or nba99core's match_3d flag (set at the match
   camera setup and each world render, so the first frame before the match's first swap counts; 0.7.11),
   the HUD not started yet
   (nba99core sets draw_2d at the match HUD call; we clear it at the swap). Since 0.7.9 the CRT
   filter is widescreen too (its source is then a wide 480-line picture). Anything else (HUD, menus, front-end 3D) goes into
   the 4:3 area. A change starts a new batch, since the scissor differs. */
static long g_wsp_draws[2];   /* Debug probe (0.7.11): draws per layer since the last swap */
void d3d_ws_layer(void)
{
    nba99_stats *st = core_stats();
    /* 0.7.14: while the front end is still in the screen state the match's world render ran in (the pause
       menu and its sub-screens, which redraw the darkened arena and the scoreboard in 3D without a world
       render and without regular swaps), the 3D is the match too */
    int in_match = st && st->game_v11 && st->match_state && *(volatile unsigned char *)(uintptr_t)0x5C8284 == st->match_state;
    int l = G.ws && (GetTickCount() - G.ws_swap_tick < 500 || (st && st->match_3d) || in_match) && !(st && st->draw_2d);
    g_wsp_draws[l]++;
    if (l != G.ws_layer) {
        static int n;
        d3d_flush();
        G.ws_layer = l;
        G.dirty = 1;
        if (n < 12) {
            n++;
            glog("widescreen: draws now %s (swap %lu ms ago, HUD flag %ld, CRT %d)", l ? "full width (match 3D)" : "in the 4:3 area",
                 (unsigned long)(GetTickCount() - G.ws_swap_tick), st ? st->draw_2d : -1L, crt_mode());
        }
    }
}

static void fill_tex_constants(int t, uint32_t *texinfo, float *su, float *sv)
{
    TexEntry *e = G.bound[t];
    if (!e) { texinfo[0] = 0; *su = *sv = 1.0f / 256.0f; return; }
    texinfo[0] = e->kind; texinfo[1] = e->width; texinfo[2] = e->height;
    texinfo[3] = G.tex_mipmode[t] != GR_MIPMAP_DISABLE ? e->levels : 1;
    {   /* Glide s/t run 0..256 along the longer side of the largest level */
        int big = e->width > e->height ? e->width : e->height;
        *su = 1.0f / (256.0f * e->width / big);
        *sv = 1.0f / (256.0f * e->height / big);
    }
}

static void apply_state(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srvs[5] = { NULL, NULL, NULL, NULL, pal_srv };
    ID3D11SamplerState *smps[2];
    UINT stride = sizeof(GVtx), off = 0;
    static const float bf[4] = { 0, 0, 0, 0 };

    fill_tex_constants(0, G.cb.tex0, &G.cb.stScale[0], &G.cb.stScale[1]);
    fill_tex_constants(1, G.cb.tex1, &G.cb.stScale[2], &G.cb.stScale[3]);
    G.cb.tsamp[0] = G.tex_filter_min[0] || G.tex_filter_mag[0];
    G.cb.tsamp[1] = G.tex_mipmode[0] != GR_MIPMAP_DISABLE;
    G.cb.tsamp[2] = G.tex_filter_min[1] || G.tex_filter_mag[1];
    G.cb.tsamp[3] = G.tex_mipmode[1] != GR_MIPMAP_DISABLE;
    G.cb.tclamp[0] = G.tex_clamp_s[0]; G.cb.tclamp[1] = G.tex_clamp_t[0];
    G.cb.tclamp[2] = G.tex_clamp_s[1]; G.cb.tclamp[3] = G.tex_clamp_t[1];
    G.cb.lodBias[0] = G.tex_lodbias[0]; G.cb.lodBias[1] = G.tex_lodbias[1];
    G.cb.inv[2] = G.depth_mode;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_main, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &G.cb, sizeof G.cb);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_main, 0);
    }
    if (G.bound[0]) srvs[G.bound[0]->kind == 1 ? 0 : 1] = G.bound[0]->srv;
    if (G.bound[1]) srvs[G.bound[1]->kind == 1 ? 2 : 3] = G.bound[1]->srv;
    smps[0] = get_sampler(0);
    smps[1] = get_sampler(1);

    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(ctx, vs_main, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb_main);
    ID3D11DeviceContext_PSSetShader(ctx, ps_main, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb_main);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 5, srvs);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, smps);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_OMSetBlendState(ctx, get_blend(), bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, get_depth(), 0);
    set_target(G.render_buffer);
    set_scissor();
    batch_target = G.render_buffer;
}

void d3d_flush(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!npend || !ctx) { npend = 0; return; }
    if (vb_pos + npend > VB_VERTS) { vb_pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
    if (vb_pos == 0) mode = D3D11_MAP_WRITE_DISCARD;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)vb, 0, mode, 0, &m))) {
        memcpy((GVtx *)m.pData + vb_pos, pend, sizeof(GVtx) * npend);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)vb, 0);
        ID3D11DeviceContext_Draw(ctx, npend, vb_pos);
        vb_pos += npend;
        G.draws++;
        G.tris += npend / 3;
    }
    if (batch_target >= 0) {
        G.gpu_newer[batch_target] = 1;
        if (batch_target == 0) G.front_dirty = 1;
    }
    npend = 0;
}

void d3d_state_changed(void) { d3d_flush(); G.dirty = 1; }

void d3d_add_tris(const GVtx *v, int n)
{
    if (!ctx) return;
    if (G.dirty) {
        d3d_flush();
        apply_state();
        G.dirty = 0;
    }
    if (npend + n > VB_VERTS) d3d_flush();
    memcpy(pend + npend, v, sizeof(GVtx) * n);
    npend += n;
}

/* full-screen pass with the util shaders; restores Glide state afterwards */
static void util_pass(ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
                      int vx, int vy, int vw, int vh, ID3D11DepthStencilState *dss,
                      ID3D11ShaderResourceView *srv_l, ID3D11ShaderResourceView *srv_s, ID3D11SamplerState *smp,
                      const float *color, const float *p0, const uint32_t *u0, const D3D11_RECT *scissor)
{
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *srvs[2];
    D3D11_RECT full;
    static const float bf[4] = { 0, 0, 0, 0 };
    struct { float color[4]; float p0[4]; uint32_t u0[4]; } u;
    memset(&u, 0, sizeof u);
    if (color) memcpy(u.color, color, 16);
    if (p0) memcpy(u.p0, p0, 16);
    if (u0) memcpy(u.u0, u0, 16);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_util, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &u, sizeof u);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_util, 0);
    }
    srvs[0] = srv_l; srvs[1] = srv_s;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
    vp.TopLeftX = (float)vx; vp.TopLeftY = (float)vy; vp.Width = (float)vw; vp.Height = (float)vh;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    full.left = vx; full.top = vy; full.right = vx + vw; full.bottom = vy + vh;
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, scissor ? scissor : &full);
    ID3D11DeviceContext_RSSetState(ctx, rs);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs_full, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb_util);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext_OMSetBlendState(ctx, util_bs ? util_bs : bs_off, bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, dss, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    srvs[0] = srvs[1] = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, srvs);
    G.dirty = 1;   /* the next Glide draw re-applies its state */
}

#include "gl_overlay.inc"
#include "gl_judder.inc"          /* Debug judder log (0.7.10) */
#include "gl_shot.inc"            /* Page Down screenshots (0.7.12) */
#include "gl_menulayer.inc"       /* 99: menus over the full-resolution arena */

void d3d_clear(FxU32 color, FxU32 alpha, FxU32 depth)
{
    float c[4], p[4] = { 0, 0, 0, 0 };
    D3D11_RECT r;
    int idx;
    if (!ctx) return;
    d3d_flush();
    idx = G.render_buffer == 0 ? front_idx : 1 - front_idx;
    c[0] = ((color >> 16) & 255) / 255.0f;
    c[1] = ((color >> 8) & 255) / 255.0f;
    c[2] = (color & 255) / 255.0f;
    c[3] = (alpha & 255) / 255.0f;
    p[0] = (depth & 0xFFFF) / 65535.0f;
    if (G.ws) d3d_ws_layer();
    clip_rect(&r);
    if (r.left <= 0 && r.top <= 0 && r.right >= G.rt_w && r.bottom >= G.rt_h) {
        if (G.color_mask_rgb) ID3D11DeviceContext_ClearRenderTargetView(ctx, rt_rtv[idx], c);
        if (G.depth_mask) ID3D11DeviceContext_ClearDepthStencilView(ctx, ds_dsv, D3D11_CLEAR_DEPTH, p[0], 0);
    } else {
        util_pass(ps_clear, rt_rtv[idx], G.depth_mask ? ds_dsv : NULL, 0, 0, G.rt_w, G.rt_h,
                  G.depth_mask ? ds_clear : ds_off, NULL, NULL, smp_point, c, p, NULL, &r);
    }
    G.gpu_newer[G.render_buffer == 0 ? 0 : 1] = 1;
    if (G.render_buffer == 0) G.front_dirty = 1;
}

/* ------------------------------------------------------------------------- */
/* LFB                                                                       */
/* ------------------------------------------------------------------------- */

void d3d_lfb_upload(int buf, int x, int y, int w, int h)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t u[4] = { 0, 0, 0, 0 };
    int idx = buf == 0 ? front_idx : 1 - front_idx, row;
    (void)x; (void)y; (void)w; (void)h;   /* always the whole frame: 600 KB, cheap */
    if (!ctx) return;
    d3d_flush();
    if (GetTickCount() - G.ws_swap_tick >= 500) {   /* front end or pause: no match 3D in flight */
        nba99_stats *st = core_stats();
        if (st) st->match_3d = 0;
    }
    if (ml_on_write(buf, idx)) return;
    if (NBA99_DEBUG && g_probe_all > 0 && GetTickCount() - G.ws_swap_tick < 30000)
        glog("LFB write: whole 640x480 picture into the 4:3 area (target %d, swap %lu ms ago)", idx,
             (unsigned long)(GetTickCount() - G.ws_swap_tick));
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)lfb_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    for (row = 0; row < FB_H; row++)
        memcpy((uint8_t *)m.pData + row * m.RowPitch, G.shadow[buf] + row * FB_W, FB_W * 2);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)lfb_tex, 0);
    u[0] = G.cfg_menu_linear ? 1 : 0;
    if (G.ws && GetTickCount() - G.ws_swap_tick >= 500) {
        /* widescreen, outside matches: the bars beside the 4:3 front end are black */
        static const float black[4] = { 0, 0, 0, 1 }, z[4] = { 0, 0, 0, 0 };
        D3D11_RECT r;
        r.left = 0; r.top = 0; r.right = G.c43_x; r.bottom = G.rt_h;
        util_pass(ps_clear, rt_rtv[idx], NULL, 0, 0, G.rt_w, G.rt_h, ds_off, NULL, NULL, smp_point, black, z, NULL, &r);
        r.left = G.c43_x + G.c43_w; r.right = G.rt_w;
        util_pass(ps_clear, rt_rtv[idx], NULL, 0, 0, G.rt_w, G.rt_h, ds_off, NULL, NULL, smp_point, black, z, NULL, &r);
    }
    /* the game's 640x480 picture goes into the 4:3 area (all of a 4:3 target) */
    util_pass(ps_lfb, rt_rtv[idx], NULL, G.c43_x, 0, G.c43_w, G.rt_h, ds_off, lfb_srv, NULL, smp_point, NULL, NULL, u, NULL);
    G.gpu_newer[buf] = 0;
}

void d3d_lfb_sync_to_shadow(int buf)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int idx = buf == 0 ? front_idx : 1 - front_idx, row;
    static int count;
    if (!ctx) return;
    d3d_flush();
    if (count++ < 10 || g_probe_all > 0) glog("LFB read of GPU-rendered %s buffer: reading back from the GPU (target %d)", buf ? "back" : "front", idx);
    {   /* the 4:3 area of the target (all of it unless widescreen) */
        float p0[4];
        p0[0] = p0[1] = 0; p0[2] = (float)G.c43_w / G.rt_w; p0[3] = (float)G.c43_x / G.rt_w;
        util_pass(ps_readback, rb_rtv, NULL, 0, 0, FB_W, FB_H, ds_off, NULL, rt_srv[idx], smp_point, NULL, p0, NULL, NULL);
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)rb_staging, (ID3D11Resource *)rb_tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)rb_staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (row = 0; row < FB_H; row++)
            memcpy(G.shadow[buf] + row * FB_W, (uint8_t *)m.pData + row * m.RowPitch, FB_W * 2);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)rb_staging, 0);
        ml_on_read(buf, idx);
    }
    G.gpu_newer[buf] = 0;
}

/* ------------------------------------------------------------------------- */
/* presentation                                                              */
/* ------------------------------------------------------------------------- */
static void dump_backbuffer(void)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH], *s;
    HANDLE f;
    DWORD wr;
    int y, x;
    ID3D11Texture2D_GetDesc(bb_tex, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb_tex);
    GetModuleFileNameA(NULL, path, sizeof path);
    s = strrchr(path, '\\');
    if (s) s[1] = 0;
    {
        char name[64], dir[MAX_PATH];
        _snprintf(name, sizeof name, "glide_frame_%u.bmp", present_count);
        strcpy(dir, path);
        nba99_log_path(dir, name, path, sizeof path);
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        BITMAPFILEHEADER fh;
        BITMAPINFOHEADER ih;
        uint8_t *line = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, d.Width * 3 + 4);
        int pitch = (d.Width * 3 + 3) & ~3;
        memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + pitch * d.Height;
        ih.biSize = sizeof ih; ih.biWidth = d.Width; ih.biHeight = d.Height; ih.biPlanes = 1; ih.biBitCount = 24;
        f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE && line) {
            WriteFile(f, &fh, sizeof fh, &wr, NULL);
            WriteFile(f, &ih, sizeof ih, &wr, NULL);
            for (y = (int)d.Height - 1; y >= 0; y--) {
                const uint8_t *p = (const uint8_t *)m.pData + y * m.RowPitch;
                for (x = 0; x < (int)d.Width; x++) { line[x * 3] = p[x * 4 + 2]; line[x * 3 + 1] = p[x * 4 + 1]; line[x * 3 + 2] = p[x * 4]; }
                WriteFile(f, line, pitch, &wr, NULL);
            }
            CloseHandle(f);
            glog("saved %s", path);
        }
        if (line) HeapFree(GetProcessHeap(), 0, line);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    }
    REL(st);
}

/*
 * Display lock ([glide] DisplayLock=1, default; needs StepLock and the core's
 * precise clock). With the step lock, frames follow the game clock (60.0 Hz)
 * while the screen refreshes at its own rate (e.g. 59.94 Hz). In the test
 * second timing log that beat showed as a few seconds of uneven frames every
 * 16.7 s (1 / 0.06 Hz), each time a present crossed a refresh. Here the game
 * clock is steered instead: its base speed is the display rate / 60 (for
 * 60 Hz class screens, or 120 Hz with every other refresh), and a slow
 * correction keeps each present halfway between two refreshes. Changes stay
 * under 1% (59.94 Hz runs the game 0.1% slower). Uses DWM's composition
 * timing (vblank time and period); off when that is not available.
 */
typedef HRESULT (WINAPI *DwmGetCompositionTimingInfo_fn)(HWND, DWM_TIMING_INFO *);
static DwmGetCompositionTimingInfo_fn p_dwm_timing;
static nba99core_set_clock_rate_fn p_set_rate;
static int dl_state;                /* 0 not tried, 1 available, -1 not available */
static double dl_integ, dl_rate_sent;
static double dl_w_min = 1, dl_w_max, dl_w_sum, dl_w_start;
static long dl_w_n;

static void display_lock(void)
{
    nba99_stats *st = core_stats();
    DWM_TIMING_INFO ti;
    LARGE_INTEGER now;
    double period, hz, base, ph, e, rate, k;
    if (!G.cfg_display_lock || !st || !st->precise_clock) {
        if (dl_rate_sent != 0 && dl_rate_sent != 1.0 && p_set_rate) p_set_rate(1.0);
        if (st && st->disp_lock) st->disp_lock = 0;
        dl_rate_sent = 1.0;
        return;
    }
    /* menus and dead-ball pauses: keep the clock at the locked speed, so the
       phase is still right when play resumes (no new capture each time) */
    if (!G.step_lock_on) return;
    if (!dl_state) {
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        HMODULE core = GetModuleHandleA("nba99core.dll");
        p_dwm_timing = dwm ? (DwmGetCompositionTimingInfo_fn)GetProcAddress(dwm, "DwmGetCompositionTimingInfo") : NULL;
        p_set_rate = core ? (nba99core_set_clock_rate_fn)GetProcAddress(core, "nba99core_set_clock_rate") : NULL;
        dl_state = p_dwm_timing && p_set_rate ? 1 : -1;
        glog("display lock: %s", dl_state > 0 ? "available" : "not available (no DWM timing or no clock control)");
    }
    if (dl_state < 0) return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (FAILED(p_dwm_timing(NULL, &ti)) || !ti.qpcRefreshPeriod || !ti.qpcVBlank) {
        static int warned;
        if (!warned++) glog("display lock: DWM timing not available, lock off");
        dl_state = -1;
        return;
    }
    QueryPerformanceCounter(&now);
    period = (double)ti.qpcRefreshPeriod;
    hz = ti.rateRefresh.uiDenominator ? (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator
                                      : (double)G.qpf_hz / period;
    k = floor(hz / 60.0 + 0.5);
    if (k < 1 || fabs(hz / k - 60.0) > 1.5) {   /* e.g. 75 or 144 Hz: no whole number of refreshes per frame */
        if (st->disp_lock) { st->disp_lock = 0; p_set_rate(1.0); dl_rate_sent = 1.0; glog("display lock: %.2f Hz is not a multiple of about 60 Hz, lock off", hz); }
        return;
    }
    base = hz / k / 60.0;
    ph = fmod((double)(now.QuadPart - (LONGLONG)ti.qpcVBlank), period);
    if (ph < 0) ph += period;
    ph /= period;                       /* 0 = at a refresh, 0.5 = halfway */
    e = ph - 0.5;                       /* > 0: presents come late in the interval: run the clock a little faster */
    dl_integ += e * 0.00002;
    if (dl_integ > 0.004) dl_integ = 0.004;
    if (dl_integ < -0.004) dl_integ = -0.004;
    rate = base * (1.0 + 0.004 * e + dl_integ);
    if (fabs(rate - dl_rate_sent) > 0.00001) { p_set_rate(rate); dl_rate_sent = rate; }
    if (!st->disp_lock) glog("display lock: on, display %.3f Hz, %d refresh(es) per frame, base clock x%.5f", hz, (int)k, base);
    st->disp_lock = 1;
    st->disp_hz = (float)hz;
    if (ph < dl_w_min) dl_w_min = ph;
    if (ph > dl_w_max) dl_w_max = ph;
    dl_w_sum += ph;
    dl_w_n++;
    if (gnow_ms() - dl_w_start >= 1000.0) {
        st->r_ph_min = (float)dl_w_min;
        st->r_ph_max = (float)dl_w_max;
        st->r_ph_avg = (float)(dl_w_sum / dl_w_n);
        dl_w_min = 1; dl_w_max = dl_w_sum = 0; dl_w_n = 0;
        dl_w_start = gnow_ms();
    }
}

/*
 * CRT filter ([glide] CRT, also on the Display page; applies at once). The game's picture is first
 * reduced to its own 480 lines (640 wide; in widescreen as wide as the picture, 0.7.9), then drawn
 * with a CRT shader ported from DOSBox Staging (see
 * gl_shaders.h for the credits and licence):
 *   Auto  - as DOSBox picks by screen height: crt-hyllian when the picture is at least 3x 480 lines
 *           tall (1440 and up, e.g. 4K), else vga-1080p drawn at exactly 2 screen lines per game line
 *           (960 lines, centred: needs a picture at least 960 tall; smaller ones use crt-hyllian).
 *   Sharp - crt-hyllian with the Catmull-Rom horizontal filter and beam profile 1.
 *   Soft  - crt-hyllian with the Hermite horizontal filter and beam profile 2.
 * The mode comes from the core's shared stats (crt_mode, set by the options menu) when it is there,
 * else from the ini. Returns 1 when it drew the picture.
 */
static int crt_mode(void)
{
    nba99_stats *st = core_stats();
    if (st && st->crt_mode >= 0) return (int)st->crt_mode;
    return G.cfg_crt;
}

static int crt_present(int idx, const float *gp)
{
    static int last = -1, last_w;
    int mode = crt_mode(), vga, vx = G.view_x, vy = G.view_y, vw = G.view_w, vh = G.view_h;
    float c[4], p0[4];
    uint32_t u[4];
    const char *what;
    if (mode <= 0 || !ps_crt_down || !ps_crt_hyllian || !ps_crt_vga || !crt_rtv) {
        if (last > 0) glog("CRT filter: off");
        last = 0;
        G.pic_w = 0;   /* mouse mapping back to the full picture */
        return 0;
    }
    {   /* the source: the game's 480 lines, 640 wide, or as wide as the picture in widescreen (0.7.9:
           the whole wide render target, so a match keeps its full width under the filter) */
        int w = G.ws ? ((int)((double)FB_H * G.rt_w / G.rt_h + 0.5) & ~1) : FB_W;
        if (w < FB_W) w = FB_W;
        if (w != crt_w) {
            REL(crt_rtv); REL(crt_srv); REL(crt_tex);
            make_tex2d(w, FB_H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                       D3D11_USAGE_DEFAULT, 0, &crt_tex, &crt_srv, &crt_rtv);
            crt_w = w;
            if (!crt_rtv) { glog("CRT filter: could not make a %dx%d source picture; filter off", w, FB_H); crt_w = 0; return 0; }
        }
    }
    vga = mode == 1 && vh < 3 * FB_H && vh >= 2 * FB_H && 2 * crt_w <= vw;
    /* the game's lines, with its gamma (color.x = source width for the 4 taps) */
    c[0] = (float)crt_w; c[1] = c[2] = c[3] = 0;
    p0[0] = 0; p0[1] = gp[1]; p0[2] = 1; p0[3] = 0;
    util_pass(ps_crt_down, crt_rtv, NULL, 0, 0, crt_w, FB_H, ds_off, NULL, rt_srv[idx], smp_linear, c, p0, NULL, NULL);
    if (vga) {
        int cx = vx + vw / 2, cy = vy + vh / 2;
        vh = 2 * FB_H; vw = 2 * crt_w;
        vx = cx - vw / 2; vy = cy - vh / 2;
        u[0] = 2; u[1] = (uint32_t)vx; u[2] = (uint32_t)vy; u[3] = (uint32_t)crt_w;
        util_pass(ps_crt_vga, bb_rtv, NULL, vx, vy, vw, vh, ds_off, NULL, crt_srv, smp_point, NULL, NULL, u, NULL);
        what = "vga-1080p (2 lines per game line)";
    } else {
        if (mode == 2) {        /* Sharp */
            c[0] = 0.62f; c[1] = 1.00f; c[2] = 1.00f; c[3] = 1.40f; p0[0] = 1;
            what = "crt-hyllian Sharp";
        } else if (mode == 3) { /* Soft */
            c[0] = 0.72f; c[1] = 1.00f; c[2] = 1.00f; c[3] = 1.20f; p0[0] = 0;
            what = "crt-hyllian Soft";
        } else {                /* Auto, 3x and up: the file's defaults */
            c[0] = 0.85f; c[1] = 0.95f; c[2] = 1.30f; c[3] = 2.50f; p0[0] = 0;
            what = "crt-hyllian";
        }
        p0[1] = 0.55f; p0[2] = 2.4f; p0[3] = 2.0f;   /* mask intensity, input and output gamma */
        u[0] = 2; u[1] = (uint32_t)vx; u[2] = (uint32_t)vy; u[3] = (uint32_t)crt_w;
        util_pass(ps_crt_hyllian, bb_rtv, NULL, vx, vy, vw, vh, ds_off, NULL, crt_srv, smp_point, c, p0, u, NULL);
    }
    G.pic_x = vx; G.pic_y = vy; G.pic_w = vw; G.pic_h = vh;
    if (G.ws) {   /* the mouse and the menu box belong to the 4:3 area in the middle */
        G.pic_x = vx + (int)((double)vw * G.c43_x / G.rt_w + 0.5);
        G.pic_w = (int)((double)vw * G.c43_w / G.rt_w + 0.5);
    }
    if (mode != last || crt_w != last_w) glog("CRT filter: %s, mode %d, source %dx%d, picture %dx%d at %d,%d", what, mode, crt_w, FB_H, vw, vh, vx, vy);
    last = mode; last_w = crt_w;
    return 1;
}

/* gamepad menu selection (0.7.4): nba99core publishes the selected object in game pixels;
   a dotted light grey frame is drawn round it on the shown picture. Hidden once nba99core has
   not updated it for 250 ms (a match started, or the menus stopped polling). */
static void nav_draw(void)
{
    nba99_stats *st = core_stats();
    ID3D11BlendState *keep = util_bs;
    int px = G.pic_w > 0 ? G.pic_x : G.view_x, py = G.pic_w > 0 ? G.pic_y : G.view_y;
    int pw = G.pic_w > 0 ? G.pic_w : G.view_w, ph = G.pic_w > 0 ? G.pic_h : G.view_h;
    float c[4] = { 0.80f, 0.80f, 0.80f, 1 }, p0[4];
    uint32_t u[4];
    long bx, by, bw, bh;
    int x0, y0, x1, y1, t;
    if (!st || !ps_navbox || pw <= 0 || ph <= 0) return;
    bw = st->nav_box[2]; bh = st->nav_box[3];
    if (bw <= 0 || bh <= 0 || (DWORD)(GetTickCount() - (DWORD)st->nav_time) > 250) return;
    bx = st->nav_box[0]; by = st->nav_box[1];
    x0 = px + (int)floorf(bx * pw / (float)FB_W + 0.5f);
    y0 = py + (int)floorf(by * ph / (float)FB_H + 0.5f);
    x1 = px + (int)floorf((bx + bw) * pw / (float)FB_W + 0.5f);
    y1 = py + (int)floorf((by + bh) * ph / (float)FB_H + 0.5f);
    if (x0 < px) x0 = px;
    if (y0 < py) y0 = py;
    if (x1 > px + pw) x1 = px + pw;
    if (y1 > py + ph) y1 = py + ph;
    if (x1 - x0 < 4 || y1 - y0 < 4) return;
    t = ph / FB_H;                                     /* about one game pixel wide */
    if (t < 2) t = 2;
    p0[0] = (float)x0; p0[1] = (float)y0; p0[2] = (float)(x1 - x0); p0[3] = (float)(y1 - y0);
    u[0] = (uint32_t)t; u[1] = (uint32_t)(3 * t); u[2] = u[3] = 0;
    util_bs = NULL;
    util_pass(ps_navbox, bb_rtv, NULL, x0, y0, x1 - x0, y1 - y0, ds_off, NULL, NULL, smp_point, c, p0, u, NULL);
    util_bs = keep;
}

/* the picture for the back buffer: the game's frame (CRT filter or plain), then the menu box */
static void compose_backbuffer(int idx, float *p)
{
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, bb_rtv, black);
    if (!crt_present(idx, p)) {
        util_pass(ps_present, bb_rtv, NULL, G.view_x, G.view_y, G.view_w, G.view_h, ds_off, NULL, rt_srv[idx],
                  G.cfg_linear ? smp_linear : smp_point, NULL, p, NULL, NULL);
        if (G.ws) {   /* the mouse and the menu box belong to the 4:3 area */
            G.pic_x = G.view_x + (int)((double)G.view_w * G.c43_x / G.rt_w + 0.5);
            G.pic_w = (int)((double)G.view_w * G.c43_w / G.rt_w + 0.5);
            G.pic_y = G.view_y; G.pic_h = G.view_h;
        }
    }
    {   /* tell nba99core the shape matches are drawn at (it sets the match camera from it) */
        nba99_stats *st = core_stats();
        if (st) {
            st->ws_aspect = G.ws ? (float)G.rt_w / (float)G.rt_h : 0.0f;
            st->render_h = G.rt_h;   /* the 3D is drawn at this height, CRT filter or not (0.7.9) */
        }
    }
    nav_draw();
}

/*
 * Presentation statistics (0.7.18). 0.7.16 (keep a frame waiting) and 0.7.17 (drain it)
 * both failed to fix the judder on the test PC, and both guessed at what the screen did.
 * This reads what Windows reports instead: after each Present, the swap chain's frame
 * statistics (IDXGISwapChainMedia, Windows 8.1 and later; else IDXGISwapChain) give the
 * last present that reached the screen, the refresh it went out on, and how the frame was
 * shown (composed by DWM, an overlay plane, or neither: our frame scanned out directly).
 * - glide2x.log (both builds): a line when that mode changes (the first 100 changes).
 * - judder.log (Debug), per second: how many frames reached the screen, how many stayed up
 *   for 2 or more refreshes (a repeat: judder), how many were never shown, and how many
 *   presents were still queued behind the screen.
 * Changes nothing in what is drawn or when. The 0.7.16 / 0.7.17 queue code is removed.
 */
static const GUID k_iid_swapchain_media = { 0xdd95b90b, 0xf05f, 0x4f6a, { 0xbd, 0x65, 0x25, 0xbf, 0xb2, 0x64, 0xbd, 0x84 } };
static IDXGISwapChainMedia *ps_media;
static int ps_tried, ps_mode = -2, ps_mode_changes;
static UINT ps_last_pc, ps_last_rc;
static int ps_have;

static const char *ps_mode_name(int m)
{
    switch (m) {
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSED: return "composed by DWM";
    case DXGI_FRAME_PRESENTATION_MODE_OVERLAY: return "overlay plane";
    case DXGI_FRAME_PRESENTATION_MODE_NONE: return "direct (not composed)";
    case DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE: return "composition failure";
    case -1: return "no statistics";
    default: return "unknown";
    }
}

static void present_stats_release(void) { REL(ps_media); ps_tried = 0; ps_have = 0; }

static void present_stats(void)
{
    DXGI_FRAME_STATISTICS_MEDIA m;
    UINT last = 0;
    int mode = -1;
    HRESULT hr;
    if (!sc) return;
    if (!ps_tried) {
        ps_tried = 1;
        if (FAILED(IDXGISwapChain_QueryInterface(sc, k_iid_swapchain_media, (void **)&ps_media))) ps_media = NULL;
        glog("present statistics: %s", ps_media ? "IDXGISwapChainMedia" : "IDXGISwapChain only (no presentation mode)");
    }
    memset(&m, 0, sizeof m);
    if (ps_media) {
        hr = IDXGISwapChainMedia_GetFrameStatisticsMedia(ps_media, &m);
        if (SUCCEEDED(hr)) mode = (int)m.CompositionMode;
    } else {
        DXGI_FRAME_STATISTICS f;
        memset(&f, 0, sizeof f);
        hr = IDXGISwapChain_GetFrameStatistics(sc, &f);
        if (SUCCEEDED(hr)) { m.PresentCount = f.PresentCount; m.PresentRefreshCount = f.PresentRefreshCount; mode = -3; }
    }
    if (FAILED(hr)) { ps_have = 0; mode = -1; }
    if (mode != ps_mode) {
        if (ps_mode_changes++ < 100)
            glog("display: frames shown %s (was %s)", mode == -3 ? "(mode not reported)" : ps_mode_name(mode),
                 ps_mode == -2 ? "start" : ps_mode == -3 ? "(mode not reported)" : ps_mode_name(ps_mode));
        ps_mode = mode;
    }
    if (FAILED(hr)) return;
    IDXGISwapChain_GetLastPresentCount(sc, &last);
    if (ps_have && m.PresentCount != ps_last_pc) {
        UINT dp = m.PresentCount - ps_last_pc, dr = m.PresentRefreshCount - ps_last_rc;
        jd_screen(dp, dr, last - m.PresentCount, mode);
    } else if (ps_have) {
        jd_screen(0, 0, last - m.PresentCount, mode);
    }
    ps_last_pc = m.PresentCount;
    ps_last_rc = m.PresentRefreshCount;
    ps_have = 1;
}

/*
 * Display queue: skip one present (0.7.19, [glide] QueueSkip=1, default). The 0.7.18 test logs
 * (2026-11-05): every frame reached the screen exactly once in both states (overlay plane,
 * no frame held or lost), but play judders whenever each Present blocks (7 to 14 ms, a frame
 * waiting in the queue) and is smooth when Present returns at once; the judder he saw at
 * 2:20, the better stretch near 0:50 and the smooth end of the quarter match those states.
 * While Present blocks, the game's next frame starts wherever that block ends, drifting
 * against its own clock. 0.7.17 waited one refresh to empty the queue, but the game then
 * sent two quick frames to catch up and the queue filled again.
 * Here, in live play (step lock and display lock on, VSync on), after 30 frames in a row
 * whose Present blocked (2 ms or more), the next finished frame is not presented at all: the
 * one already waiting goes on screen, and from then on each frame is shown at the refresh
 * after its Present. One game step never reaches the screen (a single skipped frame). If
 * presents still block after three skips in a row, the skip stops for the session.
 */
static int qs_run, qs_skip, qs_pending, qs_after, qs_fails, qs_off;
static long qs_skips;

static void queue_watch(double took)
{
    nba99_stats *st = core_stats();
    if (!G.cfg_queue_skip || qs_off || !G.cfg_vsync || !G.step_lock_on || !st || !st->disp_lock) { qs_run = 0; qs_skip = 0; return; }
    if (took < 2.0) {                        /* Present did not block: the state we want */
        if (took < 1.0 && qs_pending) { qs_pending = 0; qs_fails = 0; }
        qs_run = 0;
        return;
    }
    if (qs_pending && ++qs_after >= 30) {    /* the last skip did not hold */
        qs_pending = 0;
        if (++qs_fails >= 3) {
            qs_off = 1;
            glog("display queue: presents keep blocking after 3 skips, skip off");
            return;
        }
    }
    if (++qs_run >= 30) { qs_run = 0; qs_skip = 1; }
}

static void present_rt(int idx, int sync)
{
    float p[4] = { 0, 1, 0, 0 };
    float gamma = (G.gamma > 0.1f ? G.gamma : 1.0f) * G.cfg_gamma;
    double t0, took;
    HRESULT hr;
    if (qs_skip) {                           /* this frame is not shown (see queue_watch) */
        qs_skip = 0;
        qs_skips++;
        qs_pending = 1;
        qs_after = 0;
        if (qs_skips <= 20 || qs_skips % 50 == 0)
            glog("display queue: Present blocked for 30 frames (a frame waiting), one frame not shown to clear it (%ld so far)", qs_skips);
        jd_note_skip();
        G.last_present_ms = gnow_ms();
        return;
    }
    p[1] = 1.0f / gamma;
    track_client_size();
    compose_backbuffer(idx, p);
    ov_frame();
    present_count++;
    if (NBA99_DEBUG && G.cfg_dump_frame && (present_count == (uint32_t)G.cfg_dump_frame ||
                             (G.cfg_dump_frame < 0 && present_count % (uint32_t)(-G.cfg_dump_frame) == 0)))
        dump_backbuffer();
    if (NBA99_DEBUG && G.trace_dump) { G.trace_dump = 0; dump_backbuffer(); }
    shot_take_if_asked();
    display_lock();
    t0 = gnow_ms();
    hr = IDXGISwapChain_Present(sc, sync, 0);
    took = gnow_ms() - t0;
    if (FAILED(hr)) {
        static int n;
        if (n++ < 5) glog("Present failed (0x%08lx)", (unsigned long)hr);
    }
    present_stats();
    queue_watch(took);
    G.last_present_ms = gnow_ms();
    /* the swap chain unbinds the back buffer on flip: Glide state must be re-applied */
    G.dirty = 1;
}

static void pace(int interval)
{
    double frame, target, now;
    if (G.cfg_vsync || G.cfg_fps_limit <= 0 || interval <= 0) return;
    frame = 1000.0 / G.cfg_fps_limit * interval;
    target = G.last_swap_ms + frame;
    now = gnow_ms();
    if (now < target) {
        double left = target - now;
        if (left > 2.0) Sleep((DWORD)(left - 1.5));
        while (gnow_ms() < target) YieldProcessor();
        G.last_swap_ms = target;
    } else {
        /* running late: do not try to catch up with a burst of frames */
        G.last_swap_ms = (now - target > frame) ? now : target;
    }
}

/*
 * Step lock. In a match the game runs one simulation step per input frame,
 * and its 120 Hz timer makes an input frame every 2 ticks. The game draws
 * whenever no input frame is waiting, so with our own 60 Hz pacing the
 * frames we showed held 0, 1 or 2 new steps (one frame in three wrong in
 * the first timing log). With StepLock=1 (default) each swap instead
 * waits until the game has made one more input frame than at the last
 * swap (until 0.7.5: 2 more ticks): the game then finds exactly one new
 * input frame, runs one step and draws it. Frame rate follows the game clock (60 Hz). Active only while
 * simulation steps are running (matches); menus keep the normal pacing.
 * Falls back to pacing if the clock stops (pause, loading) for 50 ms.
 * StepLock=2 shows every other step (an even 30 Hz) for machines that
 * cannot draw 60 frames a second.
 */
static long sl_target, sl_last_steps;
static double sl_last_step_ms;

/*
 * 0.7.6: the lock counts the game's input frames (stats input_frames, made by nba99core
 * when the game's input task samples) instead of raw ticks, so it always releases right
 * after the tick that made a frame, whatever tick it started from. Counting ticks, half of
 * all (re)starts (match start, dead balls, late frames) released on the tick in between;
 * with the display lock at mid-refresh the frame-making tick then fell on the vblank and
 * the frames showed 0, 1 or 2 steps at random for as long as the run lasted (up to 55 s in
 * the 0.6.9 test logs, longer as the session went on). Without the counter (older core)
 * it counts ticks as before.
 */
static long sl_count(const nba99_stats *st, int frames) { return frames ? st->input_frames : st->ticks_done; }

static int step_lock(void)
{
    nba99_stats *st = core_stats();
    double now = gnow_ms();
    int frames;
    long cur, per;
    static int logged = -1;
    if (!G.cfg_step_lock || !st || !st->tick_event) return 0;
    if (st->sim_steps != sl_last_steps) { sl_last_steps = st->sim_steps; sl_last_step_ms = now; }
    if (now - sl_last_step_ms > 250.0) { sl_target = 0; return 0; }   /* not in a match */
    frames = st->input_frames != 0;
    if (frames != logged) {
        glog("step lock: counting %s", frames ? "input frames (one per step)" : "ticks (no input frame counter from nba99core)");
        logged = frames;
        sl_target = 0;
    }
    /* 1 = every step (60 Hz), 2 = every other step (30 Hz) */
    per = frames ? (G.cfg_step_lock == 2 ? 2 : 1) : (G.cfg_step_lock == 2 ? 4 : 2);
    cur = sl_count(st, frames);
    if (!sl_target || cur - sl_target < -2 * per) sl_target = cur;   /* (re)start */
    if (cur - (sl_target + per) >= 0) {
        sl_target = cur;   /* drawing took longer than a step: show it now, no wait */
        G.last_swap_ms = gnow_ms();
        return 1;
    }
    sl_target += per;
    {
        /* at most 50 ms in all (0.7.7). The event is set on every tick, so a per-wait
           timeout never fires while the clock runs; when the game stops making input
           frames with the clock still running (auto replays, 2026-10-31) the loop
           waited for ever. */
        double start = gnow_ms(), waited;
        while (sl_count(st, frames) - sl_target < 0) {
            waited = gnow_ms() - start;
            if (waited >= 50.0 ||
                WaitForSingleObject((HANDLE)st->tick_event, (DWORD)(50.0 - waited) + 1) == WAIT_TIMEOUT) {
                sl_target = sl_count(st, frames);
                break;
            }
        }
    }
    G.last_swap_ms = gnow_ms();
    return 1;
}

void d3d_swap(int interval)
{
    if (!ctx) return;
    jd_swap_in();
    if (NBA99_DEBUG && G.ws) {   /* probe: the first swaps of a match */
        static int n = 100;
        nba99_stats *st = core_stats();
        if (g_probe_all < 0) { char v[4]; g_probe_all = GetEnvironmentVariableA("NBA99_LFB_PROBE", v, sizeof v) > 0; }
        if (GetTickCount() - G.ws_swap_tick >= 500) n = 0;
        if (n < 12 || (g_probe_all && n < 400))
            glog("ws probe: swap %d after a gap: draws 4:3 %ld, full width %ld, camera %s, match 3D flag %ld, screen state %d, render buffer %d", n,
                 g_wsp_draws[0], g_wsp_draws[1], st && st->ws_fov != 0 ? "wide" : "4:3", st ? st->match_3d : -1L,
                 st && st->game_v11 ? (int)*(volatile unsigned char *)(uintptr_t)0x5C8284 : -1, (int)G.render_buffer);
        n++;
    }
    g_wsp_draws[0] = g_wsp_draws[1] = 0;
    d3d_flush();
    jd_flushed();
    G.step_lock_on = step_lock();
    if (!G.step_lock_on) pace(interval);
    jd_locked();
    present_rt(1 - front_idx, G.cfg_vsync ? (interval > 0 ? 1 : 0) : 0);
    jd_swap_out();
    G.ws_swap_tick = GetTickCount();
    {   /* the next frame starts with 3D (nba99core marks the HUD again) */
        nba99_stats *st = core_stats();
        if (st) { st->draw_2d = 0; st->match_3d = 0; }
    }
    /* the back buffer becomes the front buffer */
    front_idx = 1 - front_idx;
    {
        uint16_t *t = G.shadow[0]; G.shadow[0] = G.shadow[1]; G.shadow[1] = t;
        int g = G.gpu_newer[0]; G.gpu_newer[0] = G.gpu_newer[1]; G.gpu_newer[1] = g;
    }
    G.front_dirty = 0;
    G.frame++;
    G.dirty = 1;
}

/* Front buffer flush thread. When the game draws into the Glide front buffer
 * (menus: LFB writes or triangles to the front), the image is shown at most
 * once per frame period; an update arriving sooner was left waiting for the
 * game's next Glide call. On screens that draw once and then sit (the
 * starting line-up cards, for one) the last update could stay hidden for
 * seconds while the game polled input without calling Glide. This thread
 * shows any such waiting update one frame period after the last present. */
static HANDLE ff_thread;
static volatile LONG ff_stop;
static long ff_late;
static DWORD WINAPI front_flush_thread(LPVOID arg)
{
    (void)arg;
    while (!ff_stop) {
        Sleep(4);
        if (!G.front_dirty || !ctx) continue;
        if (!TryEnterCriticalSection(&g_lock)) continue;
        if (ctx && G.front_dirty && G.open) {
            double period = G.cfg_fps_limit > 0 ? 1000.0 / G.cfg_fps_limit : 16.7;
            if (gnow_ms() - G.last_present_ms >= period) {
                d3d_present_front_if_needed(1);
                if (ff_late++ < 20) glog("front buffer: waiting update shown by the flush thread");
            }
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static void front_flush_start(void)
{
    if (ff_thread) return;
    ff_stop = 0;
    ff_late = 0;
    ff_thread = CreateThread(NULL, 0, front_flush_thread, NULL, 0, NULL);
}

static void front_flush_stop(void)
{
    if (!ff_thread) return;
    ff_stop = 1;
    WaitForSingleObject(ff_thread, 1000);
    CloseHandle(ff_thread);
    ff_thread = NULL;
    glog("front buffer: %ld waiting update(s) shown by the flush thread", ff_late);
}

void d3d_present_front_if_needed(int force)
{
    double now;
    if (!ctx || !G.front_dirty) return;
    now = gnow_ms();
    /* menus update the front buffer through the LFB about 60 times a second;
     * show the latest image at most once per frame period, without waiting for vsync */
    if (!force && G.cfg_fps_limit > 0 && now - G.last_present_ms < 1000.0 / G.cfg_fps_limit - 1.0) return;
    d3d_flush();
    present_rt(front_idx, 0);
    G.front_dirty = 0;
}

/* ------------------------------------------------------------------------- */
/* palettes                                                                  */
/* ------------------------------------------------------------------------- */
int d3d_palette_row(const FxU32 *pal)
{
    uint64_t h = 1469598103934665603ULL;
    int i, row;
    uint32_t rgba[256];
    D3D11_BOX box;
    if (!ctx) return 0;
    for (i = 0; i < 256; i++) { h ^= pal[i] & 0xFFFFFF; h *= 1099511628211ULL; }
    for (i = 0; i < pal_next; i++) if (pal_map[i].used && pal_map[i].hash == h) return i;
    if (pal_next == PAL_ROWS) {
        /* atlas full: everything queued may still reference old rows */
        d3d_flush();
        memset(pal_map, 0, sizeof pal_map);
        pal_next = 0;
    }
    row = pal_next++;
    pal_map[row].hash = h;
    pal_map[row].used = 1;
    for (i = 0; i < 256; i++) {
        FxU32 p = pal[i];   /* 0x00RRGGBB */
        rgba[i] = ((p >> 16) & 255) | (p & 0xFF00) | ((p & 255) << 16) | 0xFF000000u;
    }
    box.left = 0; box.right = 256; box.top = row; box.bottom = row + 1; box.front = 0; box.back = 1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)pal_tex, 0, &box, rgba, 1024, 0);
    return row;
}

/* ------------------------------------------------------------------------- */
/* textures                                                                  */
/* ------------------------------------------------------------------------- */
static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }

static void convert_level(const uint8_t *src, int n, FxI32 fmt, uint32_t *dst32, uint16_t *dst16)
{
    int i;
    for (i = 0; i < n; i++) {
        uint32_t v;
        switch (fmt) {
        case GR_TEXFMT_P_8: dst16[i] = (uint16_t)(src[i] | 0xFF00); break;
        case GR_TEXFMT_AP_88: v = ((const uint16_t *)src)[i]; dst16[i] = (uint16_t)v; break; /* lo = index, hi = alpha */
        case GR_TEXFMT_RGB_332: v = src[i];
            dst32[i] = rgba(((v >> 5) & 7) * 255 / 7, ((v >> 2) & 7) * 255 / 7, (v & 3) * 255 / 3, 255); break;
        case GR_TEXFMT_ALPHA_8: v = src[i]; dst32[i] = rgba(v, v, v, v); break;
        case GR_TEXFMT_INTENSITY_8: v = src[i]; dst32[i] = rgba(v, v, v, 255); break;
        case GR_TEXFMT_ALPHA_INTENSITY_44: v = src[i];
            dst32[i] = rgba((v & 15) * 17, (v & 15) * 17, (v & 15) * 17, (v >> 4) * 17); break;
        case GR_TEXFMT_ARGB_8332: v = ((const uint16_t *)src)[i];
            dst32[i] = rgba(((v >> 5) & 7) * 255 / 7, ((v >> 2) & 7) * 255 / 7, (v & 3) * 255 / 3, v >> 8); break;
        case GR_TEXFMT_RGB_565: v = ((const uint16_t *)src)[i];
            dst32[i] = rgba(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255); break;
        case GR_TEXFMT_ARGB_1555: v = ((const uint16_t *)src)[i];
            dst32[i] = rgba(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31, (v & 0x8000) ? 255 : 0); break;
        case GR_TEXFMT_ARGB_4444: v = ((const uint16_t *)src)[i];
            dst32[i] = rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17); break;
        case GR_TEXFMT_ALPHA_INTENSITY_88: v = ((const uint16_t *)src)[i];
            dst32[i] = rgba(v & 255, v & 255, v & 255, v >> 8); break;
        default: dst32[i] = rgba(128, 128, 128, 255); break;   /* YIQ formats: not supported yet */
        }
    }
}

static int build_texture(TexEntry *e)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init[9];
    void *bufs[9];
    const uint8_t *mem = G.tmu_mem[e->tmu] + e->addr;
    int L, pal = (e->format == GR_TEXFMT_P_8 || e->format == GR_TEXFMT_AP_88);
    int ok;
    FxU32 off = 0;
    static int logged_fmt[16];
    if (!pal && (e->format == GR_TEXFMT_YIQ_422 || e->format == GR_TEXFMT_AYIQ_8422) && !logged_fmt[e->format]++)
        glog("texture format %ld (YIQ) is not supported yet: drawn grey", (long)e->format);
    for (L = 0; L < e->levels; L++) {
        int w, h;
        tex_dims(e->largeLod + L, e->aspect, &w, &h);
        bufs[L] = HeapAlloc(GetProcessHeap(), 0, (size_t)w * h * 4);
        if (!bufs[L]) { while (L--) HeapFree(GetProcessHeap(), 0, bufs[L]); return 0; }
        convert_level(mem + off, w * h, e->format, (uint32_t *)bufs[L], (uint16_t *)bufs[L]);
        init[L].pSysMem = bufs[L];
        init[L].SysMemPitch = w * (pal ? 2 : 4);
        init[L].SysMemSlicePitch = 0;
        off += tex_level_bytes(w, h, e->format);
    }
    memset(&d, 0, sizeof d);
    d.Width = e->width; d.Height = e->height; d.MipLevels = e->levels; d.ArraySize = 1;
    d.Format = pal ? DXGI_FORMAT_R8G8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ok = SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, init, &e->tex)) &&
         SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)e->tex, NULL, &e->srv));
    for (L = 0; L < e->levels; L++) HeapFree(GetProcessHeap(), 0, bufs[L]);
    e->kind = pal ? (e->format == GR_TEXFMT_AP_88 ? 3 : 2) : 1;
    e->valid = ok;
    return ok;
}

void d3d_tex_invalidate(int tmu, FxU32 addr, FxU32 size)
{
    int i;
    for (i = 0; i < tex_count; i++) {
        TexEntry *e = tex_list[i];
        if (e->tmu == tmu && e->valid && e->addr < addr + size && addr < e->addr + e->size) {
            if (G.bound[tmu] == e) { d3d_flush(); G.bound[tmu] = NULL; G.dirty = 1; }
            REL(e->srv);
            REL(e->tex);
            e->valid = 0;
        }
    }
}

TexEntry *d3d_tex_get(int tmu, FxU32 addr, const GrTexInfo *info)
{
    TexEntry *e = NULL, *free_slot = NULL;
    int i;
    if (!dev) return NULL;
    tex_clock++;
    for (i = 0; i < tex_count; i++) {
        TexEntry *t = tex_list[i];
        if (t->tmu == tmu && t->addr == addr) {
            if (t->valid && t->smallLod == info->smallLod && t->largeLod == info->largeLod &&
                t->aspect == info->aspectRatio && t->format == info->format) {
                t->last_used = tex_clock;
                return t;
            }
            e = t;
            break;
        }
        if (!t->valid && !free_slot) free_slot = t;
    }
    if (!e) e = free_slot;
    if (!e) {
        if (tex_count == tex_cap) {
            int ncap = tex_cap ? tex_cap * 2 : 256;
            TexEntry **nl = (TexEntry **)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TexEntry *) * ncap);
            if (!nl) return NULL;
            if (tex_list) { memcpy(nl, tex_list, sizeof(TexEntry *) * tex_count); HeapFree(GetProcessHeap(), 0, tex_list); }
            tex_list = nl;
            tex_cap = ncap;
        }
        e = (TexEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TexEntry));
        if (!e) return NULL;
        tex_list[tex_count++] = e;
    }
    if (G.bound[tmu] == e) { d3d_flush(); G.bound[tmu] = NULL; G.dirty = 1; }
    REL(e->srv);
    REL(e->tex);
    e->tmu = tmu;
    e->addr = addr;
    e->smallLod = info->smallLod;
    e->largeLod = info->largeLod;
    e->aspect = info->aspectRatio;
    e->format = info->format;
    tex_dims(info->largeLod, info->aspectRatio, &e->width, &e->height);
    e->levels = info->smallLod - info->largeLod + 1;
    if (e->levels < 1 || e->levels > 9) e->levels = 1;
    e->size = tex_mem_required(info->smallLod, info->largeLod, info->aspectRatio, info->format);
    e->last_used = tex_clock;
    if (addr + e->size > TMU_MEM_SIZE || !build_texture(e)) { e->valid = 0; return NULL; }
    return e;
}
NBA99_C_END
