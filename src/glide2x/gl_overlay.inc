/*
 * Frame rate and pacing overlay (included by gl_d3d.cpp).
 *
 * Drawn on the back buffer after the game picture, top-left, in both builds:
 * [glide] Overlay=1 shows it from the start (default in the Debug build),
 * Ctrl+F11 shows or hides it at any time.
 *
 *   line 1: frames per second, frame time (avg, min-max), frames over 50 ms
 *   line 2: game simulation steps per second and the game clock (calls per
 *           second of the game's timer callback, gap between them), from
 *           nba99core.dll's statistics
 *   line 3: frames that showed 0, 1, 2, 3+ new simulation steps in the last
 *           second, and the frame pacing locks when one is on (lock, qpc, disp)
 *   graph:  the last 212 frame times (0 to 50 ms, lines at 16.7 and 33.3 ms),
 *           coloured by the steps each frame showed (grey 0, green 1, yellow 2,
 *           red 3+; white without nba99core's statistics)
 * (0.3.0: steps are counted in NBA Live 99 again, as in NBA Live 98.)
 *
 * Text is drawn with GDI into a small 32-bit DIB that is uploaded as a
 * texture each frame. Frame figures for the last second are also handed to
 * nba99core (r_ fields) for logs\timing.log.
 */
#include "nba99_stats.h"

#define OV_W 440
#define OV_H 104
#define OV_GRAPH_Y 48
#define OV_GRAPH_H 52
#define OV_N 212

static ID3D11Texture2D *ov_tex;
static ID3D11ShaderResourceView *ov_srv;
static ID3D11BlendState *ov_bs;
static HDC ov_dc;
static HBITMAP ov_bmp;
static HFONT ov_font;
static uint32_t *ov_px;
static int ov_on = -1;
static nba99_stats *ov_stats;
static int ov_stats_looked;
static LARGE_INTEGER ov_last, ov_win_start, ov_text_time;
static long ov_last_steps;
static float ov_ft[OV_N];
static uint8_t ov_steps[OV_N];
static int ov_pos;
/* current one-second window */
static long ov_w_frames, ov_w_long, ov_w_spf[4];
static double ov_w_min = 1e9, ov_w_max, ov_w_sum;
/* last published second */
static float ov_fps, ov_ft_min, ov_ft_avg, ov_ft_max;
static long ov_long, ov_spf[4];

static LARGE_INTEGER ov_qpf;
/* nba99core's statistics (NULL without a matching nba99core.dll) */
static nba99_stats *core_stats(void)
{
    if (!ov_stats_looked) {
        HMODULE core = GetModuleHandleA("nba99core.dll");
        nba99core_stats_fn f = core ? (nba99core_stats_fn)GetProcAddress(core, "nba99core_stats") : NULL;
        ov_stats_looked = 1;
        ov_stats = f ? f() : NULL;
        if (ov_stats && (ov_stats->version != NBA99_STATS_VERSION || ov_stats->size != sizeof(nba99_stats))) ov_stats = NULL;
        glog("game timing %s", ov_stats ? "from nba99core.dll" : "not available (no matching nba99core.dll)");
    }
    return ov_stats;
}

static double ov_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    if (!ov_qpf.QuadPart) QueryPerformanceFrequency(&ov_qpf);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)ov_qpf.QuadPart;
}

/* frame bookkeeping: runs on every present, overlay shown or not */
static void ov_count_frame(void)
{
    LARGE_INTEGER now;
    long steps = 0;
    int b;
    QueryPerformanceCounter(&now);
    core_stats();
    if (ov_stats) {
        long st = ov_stats->sim_steps;
        steps = ov_last_steps ? st - ov_last_steps : 0;
        ov_last_steps = st;
        InterlockedIncrement(&ov_stats->presents);
    }
    b = steps < 0 ? 0 : steps > 3 ? 3 : (int)steps;
    if (ov_last.QuadPart) {
        double ft = ov_ms(ov_last, now);
        ov_ft[ov_pos] = (float)ft;
        ov_steps[ov_pos] = (uint8_t)b;
        ov_pos = (ov_pos + 1) % OV_N;
        ov_w_frames++;
        ov_w_spf[b]++;
        if (ft > 50.0) ov_w_long++;
        if (ft < ov_w_min) ov_w_min = ft;
        if (ft > ov_w_max) ov_w_max = ft;
        ov_w_sum += ft;
    } else {
        ov_win_start = now;
    }
    ov_last = now;
    if (ov_ms(ov_win_start, now) >= 1000.0) {
        double len = ov_ms(ov_win_start, now) / 1000.0;
        ov_fps = (float)(ov_w_frames / len);
        ov_ft_min = ov_w_frames ? (float)ov_w_min : 0;
        ov_ft_max = ov_w_frames ? (float)ov_w_max : 0;
        ov_ft_avg = ov_w_frames ? (float)(ov_w_sum / ov_w_frames) : 0;
        ov_long = ov_w_long;
        memcpy(ov_spf, ov_w_spf, sizeof ov_spf);
        if (ov_stats) {
            ov_stats->r_fps = ov_fps;
            ov_stats->r_ft_min = ov_ft_min;
            ov_stats->r_ft_avg = ov_ft_avg;
            ov_stats->r_ft_max = ov_ft_max;
            ov_stats->r_long = ov_long;
            memcpy(ov_stats->r_spf, ov_spf, sizeof ov_spf);
            InterlockedIncrement(&ov_stats->r_seq);
        }
        ov_w_frames = ov_w_long = 0;
        memset(ov_w_spf, 0, sizeof ov_w_spf);
        ov_w_min = 1e9; ov_w_max = ov_w_sum = 0;
        ov_win_start = now;
    }
}

static int ov_create(void)
{
    BITMAPINFO bi;
    D3D11_TEXTURE2D_DESC d;
    D3D11_BLEND_DESC bd;
    if (!ov_dc) {
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = OV_W;
        bi.bmiHeader.biHeight = -OV_H;   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        ov_dc = CreateCompatibleDC(NULL);
        ov_bmp = ov_dc ? CreateDIBSection(ov_dc, &bi, DIB_RGB_COLORS, (void **)&ov_px, NULL, 0) : NULL;
        if (!ov_bmp) return 0;
        SelectObject(ov_dc, ov_bmp);
        ov_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
        if (ov_font) SelectObject(ov_dc, ov_font);
        SetBkMode(ov_dc, TRANSPARENT);
    }
    if (!ov_tex) {
        memset(&d, 0, sizeof d);
        d.Width = OV_W; d.Height = OV_H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &ov_tex))) return 0;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)ov_tex, NULL, &ov_srv);
        memset(&bd, 0, sizeof bd);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(dev, &bd, &ov_bs);
    }
    return ov_srv && ov_bs && ps_overlay;
}

static void ov_release(void)
{
    REL(ov_srv); REL(ov_tex); REL(ov_bs); REL(ps_overlay);
}

static void ov_text(void)
{
    char l1[96], l2[128], l3[128];
    RECT r;
    int i;
    memset(ov_px, 0, OV_W * OV_GRAPH_Y * 4);
    _snprintf(l1, sizeof l1, "%5.1f fps  frame %4.1f ms (%4.1f-%4.1f)  >50ms %ld", ov_fps, ov_ft_avg, ov_ft_min,
              ov_ft_max, ov_long);
    if (ov_stats)
        _snprintf(l2, sizeof l2, "game %5.1f steps/s  clock %5.1f/s (%4.1f-%4.1f ms)", ov_stats->sec_steps,
                  ov_stats->sec_ticks, ov_stats->sec_gap_min, ov_stats->sec_gap_max);
    else
        _snprintf(l2, sizeof l2, "game timing not available");
    _snprintf(l3, sizeof l3, "steps per frame  0:%-3ld 1:%-3ld 2:%-3ld 3+:%-3ld %s%s", ov_spf[0], ov_spf[1], ov_spf[2],
              ov_spf[3], G.step_lock_on ? "lock" : "", ov_stats && ov_stats->precise_clock ? " qpc" : "");
    if (ov_stats && ov_stats->disp_lock) {
        size_t n = strlen(l3);
        _snprintf(l3 + n, sizeof l3 - n, " disp %.2f", ov_stats->r_ph_avg);
    }
    SetTextColor(ov_dc, RGB(255, 255, 255));
    SetRect(&r, 6, 2, OV_W, 17);  DrawTextA(ov_dc, l1, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(ov_dc, RGB(200, 220, 255));
    SetRect(&r, 6, 17, OV_W, 32); DrawTextA(ov_dc, l2, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SetRect(&r, 6, 32, OV_W, 47); DrawTextA(ov_dc, l3, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    /* GDI leaves alpha at 0: text opaque, background see-through dark */
    for (i = 0; i < OV_W * OV_GRAPH_Y; i++)
        ov_px[i] = (ov_px[i] & 0xFFFFFF) ? (ov_px[i] | 0xFF000000u) : 0xB0000000u;
}

static void ov_graph(void)
{
    static const uint32_t col[4] = { 0xFF9A9A9Au, 0xFF40D040u, 0xFFF0D020u, 0xFFF04040u };
    int x, y, i;
    uint32_t *g = ov_px + OV_W * OV_GRAPH_Y;
    for (i = 0; i < OV_W * (OV_H - OV_GRAPH_Y); i++) g[i] = 0xB0000000u;
    for (y = 0; y < OV_GRAPH_H; y++) {
        double ms = (OV_GRAPH_H - 1 - y) * 50.0 / (OV_GRAPH_H - 1);
        if (fabs(ms - 16.7) < 0.5 || fabs(ms - 33.3) < 0.5)
            for (x = 6; x < OV_W - 6; x++) g[(y + 2) * OV_W + x] = 0xFF506080u;
    }
    for (i = 0; i < OV_N; i++) {
        int k = (ov_pos + i) % OV_N, h;
        uint32_t c = ov_stats ? col[ov_steps[k]] : 0xFFFFFFFFu;
        h = (int)(ov_ft[k] * (OV_GRAPH_H - 1) / 50.0f + 0.5f);
        if (h > OV_GRAPH_H) h = OV_GRAPH_H;
        if (ov_ft[k] > 0 && h < 1) h = 1;
        x = 8 + i * 2;
        for (y = 0; y < h; y++) g[(OV_GRAPH_H + 1 - y) * OV_W + x] = c;
    }
}

/* called by present_rt after the game picture is on the back buffer */
static void ov_frame(void)
{
    LARGE_INTEGER now;
    int s;
    static long toggles_seen;
    ov_count_frame();
    if (ov_on < 0) ov_on = G.cfg_overlay;
    if (G.overlay_toggles != toggles_seen) {
        if ((G.overlay_toggles - toggles_seen) & 1) ov_on = !ov_on;
        toggles_seen = G.overlay_toggles;
        glog("overlay %s (Ctrl+F11)", ov_on ? "on" : "off");
    }
    if (!ov_on || !ov_create()) return;
    QueryPerformanceCounter(&now);
    if (!ov_text_time.QuadPart || ov_ms(ov_text_time, now) >= 250.0) {
        ov_text();
        ov_text_time = now;
    }
    ov_graph();
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)ov_tex, 0, NULL, ov_px, OV_W * 4, 0);
    s = G.out_h >= 1440 ? 3 : G.out_h >= 900 ? 2 : 1;
    util_bs = ov_bs;
    util_pass(ps_overlay, bb_rtv, NULL, 8 * s, 8 * s, OV_W * s, OV_H * s, ds_off, NULL, ov_srv, smp_point,
              NULL, NULL, NULL, NULL);
    util_bs = NULL;
}
