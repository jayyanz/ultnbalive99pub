/* ------------------------------------------------------------------------- */
/* Menu layer (NBA Live 99, [glide] MenuFullRes=1, default)                   */
/*                                                                           */
/* The 99 front end draws its menus over a live 3D arena. Every frame the    */
/* game reads the 640x480 picture back (LFB read), draws the menus into it   */
/* in software and writes the whole frame back (LFB write). Shown as it is,  */
/* the arena behind the menus is the game's 640x480 picture scaled up.       */
/*                                                                           */
/* Here the arena stays at full resolution and only the menus come from the */
/* game's picture. The menus are found by what the game changed, with two    */
/* safeguards against a menu pixel that happens to equal the arena pixel     */
/* under it (NBA Live 98's comparison showed the arena through the menus     */
/* there: the shimmer):                                                      */
/*  - marked readback: at the LFB read, the lowest bit of each colour        */
/*    channel of the picture handed to the game is set to a pattern per      */
/*    pixel (never all 0 or all 1; a change of one step in 32 or 64), and    */
/*    the pattern flips to its opposite on every read. A menu colour can     */
/*    match the marked arena on one of the two patterns at most, so every    */
/*    menu pixel comes back changed at least every other frame, even over a  */
/*    still arena (22 Nov 2026: with a fixed pattern a few dark menu pixels  */
/*    matched a still, dark roof and showed its lights as white dots);       */
/*  - held mask: a pixel the game changed counts as menu until it has come   */
/*    back unchanged for ML_HOLD frames in a row.                            */
/* Menu pixels are drawn from the game's picture (LinearMenus applies), all  */
/* others from the full-resolution frame kept at the read.                   */
/* ------------------------------------------------------------------------- */
#define ML_HOLD 30                  /* frames (half a second) */
#define ML_NEW_PICTURE 85           /* % changed: a new picture, not menus over the arena */

static ID3D11Texture2D *ml_snap_tex[2], *ml_tex;
static ID3D11ShaderResourceView *ml_snap_srv[2], *ml_srv;
static uint16_t *ml_given[2];       /* the marked 640x480 picture handed to the game, per target */
static int ml_ok[2];                /* per target: ml_snap_tex / ml_given from this frame's read */
static uint8_t *ml_mask;            /* per 640x480 pixel: frames left as menu */
static long ml_n;
static uint16_t ml_flip;            /* 0 or 0x0821: the pattern's opposite on every other read */

static void ml_create(void)
{
    int i;
    if (!G.cfg_menu_fullres) return;
    for (i = 0; i < 2; i++) {
        make_tex2d(G.rt_w, G.rt_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0,
                   &ml_snap_tex[i], &ml_snap_srv[i], NULL);
        ml_ok[i] = 0;
        if (!ml_given[i]) ml_given[i] = (uint16_t *)HeapAlloc(GetProcessHeap(), 0, FB_W * FB_H * 2);
    }
    make_tex2d(FB_W, FB_H, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC,
               D3D11_CPU_ACCESS_WRITE, &ml_tex, &ml_srv, NULL);
    if (!ml_mask) ml_mask = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, FB_W * FB_H);
    if (ml_mask) memset(ml_mask, ML_HOLD, FB_W * FB_H);
}

static void ml_release(void)
{
    int i;
    for (i = 0; i < 2; i++) { REL(ml_snap_srv[i]); REL(ml_snap_tex[i]); ml_ok[i] = 0; }
    REL(ml_srv); REL(ml_tex); REL(ps_menu);
}

static int ml_ready(void)
{
    return G.cfg_menu_fullres && ml_tex && ps_menu && ml_mask && ml_snap_tex[0] && ml_snap_tex[1] &&
           ml_given[0] && ml_given[1];
}

/* the lowest-bit pattern of a pixel: one of 001..110 (R, G, B) per position; its opposite (ml_flip)
   is also one of them */
static uint16_t ml_mark_bits(int x, int y)
{
    static const uint16_t pat[6] = { 0x0001, 0x0020, 0x0021, 0x0800, 0x0801, 0x0820 };
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return pat[h % 6];
}

/* LFB read of a frame of the live arena: keep the full-resolution frame, mark the picture */
static void ml_on_read(int buf, int idx)
{
    uint16_t *s = G.shadow[buf];
    int x, y;
    if (!ml_ready()) return;
    if (GetTickCount() - G.ws_swap_tick >= 1000) { ml_ok[idx] = 0; return; }   /* no live 3D */
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)ml_snap_tex[idx], (ID3D11Resource *)rt_tex[idx]);
    ml_flip ^= 0x0821;
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++) {
            uint16_t *p = s + y * FB_W + x;
            *p = (uint16_t)((*p & ~0x0821) | (ml_mark_bits(x, y) ^ ml_flip));
        }
    memcpy(ml_given[idx], s, FB_W * FB_H * 2);
    ml_ok[idx] = 1;
}

/* LFB write: 1 = drawn as menus over the full-resolution frame; 0 = upload the whole picture */
static int ml_on_write(int buf, int idx)
{
    D3D11_MAPPED_SUBRESOURCE m;
    const uint16_t *n = G.shadow[buf], *o;
    long changed = 0, menu = 0;
    int x, y;
    uint32_t u[4] = { 0, 0, 0, 0 };
    if (!ml_ready()) return 0;
    if (!ml_ok[idx]) { memset(ml_mask, ML_HOLD, FB_W * FB_H); return 0; }
    ml_ok[idx] = 0;   /* one write per read */
    o = ml_given[idx];
    for (y = 0; y < FB_H * FB_W; y += FB_W)
        for (x = 0; x < FB_W; x++) if (n[y + x] != o[y + x]) changed++;
    if (changed > (long)FB_W * FB_H * ML_NEW_PICTURE / 100) {   /* a new picture */
        memset(ml_mask, ML_HOLD, FB_W * FB_H);
        if (NBA99_DEBUG && ml_n < 20) glog("menu layer: %ld of the pixels new, shown as the game's whole picture", changed);
        return 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ml_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    for (y = 0; y < FB_H; y++) {
        uint32_t *d = (uint32_t *)((uint8_t *)m.pData + y * m.RowPitch);
        for (x = 0; x < FB_W; x++) {
            int i = y * FB_W + x;
            uint8_t *k = &ml_mask[i];
            if (n[i] != o[i]) *k = ML_HOLD;
            else if (*k) (*k)--;
            if (*k) menu++;
            d[x] = n[i] | (*k ? 0x10000u : 0u);
        }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ml_tex, 0);
    ml_n++;
    if (NBA99_DEBUG && (ml_n <= 20 || ml_n % 600 == 0))
        glog("menu layer (%ld): %ld pixels changed by the game, %ld drawn as menu, %ld from the full-resolution 3D",
             ml_n, changed, menu, (long)FB_W * FB_H - menu);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)rt_tex[idx], (ID3D11Resource *)ml_snap_tex[idx]);
    u[0] = G.cfg_menu_linear ? 1 : 0;
    util_pass(ps_menu, rt_rtv[idx], NULL, G.c43_x, 0, G.c43_w, G.rt_h, ds_off, ml_srv, ml_snap_srv[idx], smp_point,
              NULL, NULL, u, NULL);
    G.gpu_newer[buf] = 0;
    return 1;
}
