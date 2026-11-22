/* Exercises glide2x.dll the way NBA Live 99 does. Writes frames via DumpFrame. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../gl_internal.h"

#define FN(ret, name, args) typedef ret (__stdcall *name##_t) args; static name##_t name;
FN(void, grGlideInit, (void)) FN(void, grGlideShutdown, (void)) FN(void, grGlideGetVersion, (char *))
FN(FxBool, grSstQueryBoards, (GrHwConfiguration *)) FN(FxBool, grSstQueryHardware, (GrHwConfiguration *))
FN(void, grSstSelect, (int)) FN(FxBool, grSstWinOpen, (FxU32, FxI32, FxI32, FxI32, FxI32, int, int))
FN(void, grBufferClear, (FxU32, FxU32, FxU32)) FN(void, grBufferSwap, (int)) FN(void, grRenderBuffer, (FxI32))
FN(void, grDrawTriangle, (const GrVertex *, const GrVertex *, const GrVertex *)) FN(void, grDrawLine, (const GrVertex *, const GrVertex *))
FN(void, grColorCombine, (FxI32, FxI32, FxI32, FxI32, FxBool)) FN(void, grAlphaCombine, (FxI32, FxI32, FxI32, FxI32, FxBool))
FN(void, grTexCombine, (FxI32, FxI32, FxI32, FxI32, FxI32, FxBool, FxBool)) FN(void, grAlphaBlendFunction, (FxI32, FxI32, FxI32, FxI32))
FN(void, grDepthBufferMode, (FxI32)) FN(void, grDepthBufferFunction, (FxI32)) FN(void, grDepthMask, (FxBool)) FN(void, grDepthBiasLevel, (FxI32))
FN(void, grTexDownloadMipMap, (FxI32, FxU32, FxU32, GrTexInfo *)) FN(void, grTexSource, (FxI32, FxU32, FxU32, GrTexInfo *))
FN(void, grTexDownloadTable, (FxI32, FxI32, void *)) FN(void, grTexFilterMode, (FxI32, FxI32, FxI32)) FN(void, grTexMipMapMode, (FxI32, FxI32, FxBool))
FN(FxU32, grTexCalcMemRequired, (FxI32, FxI32, FxI32, FxI32)) FN(FxBool, grLfbLock, (FxI32, FxI32, FxI32, FxI32, FxBool, GrLfbInfo_t *))
FN(FxBool, grLfbUnlock, (FxI32, FxI32)) FN(void, grCullMode, (FxI32)) FN(void, grAlphaTestFunction, (FxI32)) FN(void, grAlphaTestReferenceValue, (FxU8))
FN(void, grClipWindow, (FxU32, FxU32, FxU32, FxU32)) FN(void, grSstIdle, (void))

static HMODULE load(void)
{
    HMODULE h = LoadLibraryA("glide2x.dll");
#define G_(n, d) n = (n##_t)GetProcAddress(h, d); if (!n) { printf("missing %s\n", d); exit(1); }
    G_(grGlideInit, "_grGlideInit@0") G_(grGlideShutdown, "_grGlideShutdown@0") G_(grGlideGetVersion, "_grGlideGetVersion@4")
    G_(grSstQueryBoards, "_grSstQueryBoards@4") G_(grSstQueryHardware, "_grSstQueryHardware@4") G_(grSstSelect, "_grSstSelect@4")
    G_(grSstWinOpen, "_grSstWinOpen@28") G_(grBufferClear, "_grBufferClear@12") G_(grBufferSwap, "_grBufferSwap@4")
    G_(grRenderBuffer, "_grRenderBuffer@4") G_(grDrawTriangle, "_grDrawTriangle@12") G_(grDrawLine, "_grDrawLine@8")
    G_(grColorCombine, "_grColorCombine@20") G_(grAlphaCombine, "_grAlphaCombine@20") G_(grTexCombine, "_grTexCombine@28")
    G_(grAlphaBlendFunction, "_grAlphaBlendFunction@16") G_(grDepthBufferMode, "_grDepthBufferMode@4")
    G_(grDepthBufferFunction, "_grDepthBufferFunction@4") G_(grDepthMask, "_grDepthMask@4") G_(grDepthBiasLevel, "_grDepthBiasLevel@4")
    G_(grTexDownloadMipMap, "_grTexDownloadMipMap@16") G_(grTexSource, "_grTexSource@16") G_(grTexDownloadTable, "_grTexDownloadTable@12")
    G_(grTexFilterMode, "_grTexFilterMode@12") G_(grTexMipMapMode, "_grTexMipMapMode@12") G_(grTexCalcMemRequired, "_grTexCalcMemRequired@16")
    G_(grLfbLock, "_grLfbLock@24") G_(grLfbUnlock, "_grLfbUnlock@8") G_(grCullMode, "_grCullMode@4")
    G_(grAlphaTestFunction, "_grAlphaTestFunction@4") G_(grAlphaTestReferenceValue, "_grAlphaTestReferenceValue@4")
    G_(grClipWindow, "_grClipWindow@16") G_(grSstIdle, "_grSstIdle@0")
    return h;
}

static GrVertex V(float x, float y, float oow, float r, float g, float b, float a, float s, float t)
{
    GrVertex v; memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.oow = oow; v.ooz = 65535.0f * 0.5f; v.r = r; v.g = g; v.b = b; v.a = a;
    v.tmuvtx[0].sow = s * oow; v.tmuvtx[0].tow = t * oow; v.tmuvtx[0].oow = 12345.0f; /* garbage: must be ignored */
    return v;
}

int main(void)
{
    GrHwConfiguration hw; char ver[64]; HMODULE h; HWND w; int i, f;
    GrLfbInfo_t li; uint16_t *p; GrTexInfo ti; uint8_t texels[64 * 64]; uint32_t pal[256];
    /* detect pass, like the game */
    h = load(); grSstQueryBoards(&hw); grGlideInit(); grSstQueryHardware(&hw); grGlideGetVersion(ver); grGlideShutdown();
    printf("detect: boards %ld type %ld version %s\n", (long)hw.num_sst, (long)hw.SSTs[0].type, ver);
    FreeLibrary(h);
    h = load();
    w = CreateWindowA("STATIC", "NBA Live 99 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, 0, 0, 0, 0);
    grGlideInit(); grSstQueryHardware(&hw); grSstSelect(0);
    if (!grSstWinOpen((FxU32)(uintptr_t)w, 7, 0, 0, 0, 2, 1)) { printf("WinOpen failed\n"); return 1; }
    /* menu: LFB writes to the front buffer (frame 1) */
    grRenderBuffer(0);
    for (f = 0; f < 3; f++) {
        grLfbLock(GR_LFB_READ_ONLY, 0, 0xff, 0, 0, &li); grLfbUnlock(GR_LFB_READ_ONLY, 0);
        grLfbLock(GR_LFB_WRITE_ONLY, 0, 0xff, 0, 0, &li);
        p = (uint16_t *)li.lfbPtr;
        for (i = 0; i < 640 * 480; i++) { int x = i % 640, y = i / 640;
            p[i] = (uint16_t)(((x * 31 / 639) << 11) | ((y * 63 / 479) << 5) | (((x / 40 + y / 40) & 1) ? 31 : 0)); }
        for (i = 0; i < 640; i++) p[240 * 640 + i] = 0xFFFF;   /* white line */
        grLfbUnlock(GR_LFB_WRITE_ONLY, 0);
        Sleep(20);
    }
    grSstIdle();
    /* 3D frames */
    grRenderBuffer(1);
    for (i = 0; i < 64 * 64; i++) texels[i] = (uint8_t)((((i % 64) / 8 + (i / 64) / 8) & 1) ? 1 : 2);
    for (i = 0; i < 256; i++) pal[i] = 0;
    pal[1] = 0x00FF2020; pal[2] = 0x002020FF;
    ti.smallLod = 2; ti.largeLod = 2; ti.aspectRatio = 3; ti.format = GR_TEXFMT_P_8; ti.data = texels;  /* 64x64 */
    printf("mem required %lu\n", (unsigned long)grTexCalcMemRequired(2, 2, 3, GR_TEXFMT_P_8));
    grTexDownloadMipMap(0, 0, 3, &ti);
    grTexDownloadTable(0, GR_TEXTABLE_PALETTE, pal);
    grTexFilterMode(0, 0, 0); grTexMipMapMode(0, 0, 0);
    for (f = 0; f < 3; f++) {
        GrVertex a, b, c, d;
        grBufferClear(0x00203040, 0, 0xFFFF);
        grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER); grDepthBufferFunction(3); grDepthMask(1);
        /* gouraud triangle, far (w = 10) */
        grColorCombine(1, 0, 0, 2, 0); grAlphaCombine(1, 0, 0, 2, 0);
        a = V(100, 400, 0.1f, 255, 0, 0, 255, 0, 0); b = V(540, 400, 0.1f, 0, 255, 0, 255, 0, 0); c = V(320, 60, 0.1f, 0, 0, 255, 255, 0, 0);
        grDrawTriangle(&a, &b, &c);
        /* textured quad, nearer (w = 2), perspective: left edge nearer */
        grTexSource(0, 0, 3, &ti);
        grTexCombine(0, 1, 0, 1, 0, 0, 0);
        grColorCombine(3, 1, 0, 1, 0); grAlphaCombine(3, 1, 0, 1, 0);   /* texture * iterated */
        a = V(200, 150, 0.5f, 255, 255, 255, 255, 0, 0); b = V(440, 170, 0.25f, 255, 255, 255, 255, 256, 0);
        c = V(440, 330, 0.25f, 255, 255, 255, 255, 256, 256); d = V(200, 350, 0.5f, 255, 255, 255, 255, 0, 256);
        grDrawTriangle(&a, &b, &c); grDrawTriangle(&a, &c, &d);
        /* translucent white triangle on top, depth-tested behind the quad (w = 5) */
        grColorCombine(1, 0, 0, 2, 0); grAlphaCombine(1, 0, 0, 2, 0);
        grAlphaBlendFunction(1, 5, 4, 0);
        a = V(250, 100, 0.2f, 255, 255, 255, 128, 0, 0); b = V(600, 250, 0.2f, 255, 255, 255, 128, 0, 0); c = V(250, 420, 0.2f, 255, 255, 255, 128, 0, 0);
        grDrawTriangle(&a, &b, &c);
        grAlphaBlendFunction(4, 0, 4, 0);
        /* a yellow line */
        a = V(20, 20, 1, 255, 255, 0, 255, 0, 0); b = V(620, 460, 1, 255, 255, 0, 255, 0, 0);
        grDrawLine(&a, &b);
        grBufferSwap(1);
    }
    /* LFB read of the GPU-rendered front buffer */
    grLfbLock(GR_LFB_READ_ONLY, 0, 0xff, 0, 0, &li);
    p = (uint16_t *)li.lfbPtr;
    printf("readback pixel (320,240)=0x%04x (10,10)=0x%04x\n", p[240 * 640 + 320], p[10 * 640 + 10]);
    grLfbUnlock(GR_LFB_READ_ONLY, 0);
    grGlideShutdown();
    printf("done\n");
    return 0;
}
