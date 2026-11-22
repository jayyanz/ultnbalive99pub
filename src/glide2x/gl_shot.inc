/*
 * Screenshots (0.7.12; included by gl_d3d.cpp, both builds). [glide] Screenshots=1 (default).
 *
 * Page Down saves the picture on the screen (as it is about to be shown: CRT filter,
 * widescreen, the overlay and the pad box included) to screenshots\NBA99_<date>_<time>.png in
 * the game folder. The window procedure (gl_win.cpp) counts the presses (the game never sees
 * the key); the next present copies the back buffer to a staging texture and a worker thread
 * writes the PNG with the Windows Imaging Component, so the game only waits for the copy.
 */
#include <wincodec.h>

/* GUIDs written out (mingw's import libraries do not all carry them) */
static const GUID k_clsid_wic = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
static const GUID k_iid_wicfactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
static const GUID k_png = { 0x1b7cfaf4, 0x713f, 0x473c, { 0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf } };
static const GUID k_fmt_bgr24 = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

typedef struct { uint8_t *bgr; UINT w, h; char path[MAX_PATH]; } shot_job;
static long g_shot_seen;

static int shot_write_png(const shot_job *j)
{
    IWICImagingFactory *f = NULL;
    IWICStream *s = NULL;
    IWICBitmapEncoder *e = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *props = NULL;
    WCHAR wpath[MAX_PATH];
    WICPixelFormatGUID fmt = k_fmt_bgr24;
    HRESULT hr, co;
    /* ole32 is loaded only here: linking it into glide2x.dll moved the second load of the DLL
       (the game loads, unloads and loads it again at start) and the game crashed on a stale
       pointer (Wine, 0.7.12 test) */
    HMODULE ole = LoadLibraryA("ole32.dll");
    HRESULT (WINAPI *pCoInit)(LPVOID, DWORD) = ole ? (HRESULT (WINAPI *)(LPVOID, DWORD))GetProcAddress(ole, "CoInitializeEx") : NULL;
    HRESULT (WINAPI *pCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *) =
        ole ? (HRESULT (WINAPI *)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *))GetProcAddress(ole, "CoCreateInstance") : NULL;
    void (WINAPI *pCoUninit)(void) = ole ? (void (WINAPI *)(void))GetProcAddress(ole, "CoUninitialize") : NULL;
    if (!pCoInit || !pCoCreate || !pCoUninit) return 0;
    co = pCoInit(NULL, COINIT_MULTITHREADED);
    MultiByteToWideChar(CP_ACP, 0, j->path, -1, wpath, MAX_PATH);
    hr = pCoCreate(k_clsid_wic, NULL, CLSCTX_INPROC_SERVER, k_iid_wicfactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &s);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(s, wpath, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, k_png, NULL, &e);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(e, (IStream *)s, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(e, &fr, &props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, j->w, j->h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt);
    if (SUCCEEDED(hr) && memcmp(&fmt, &k_fmt_bgr24, sizeof fmt)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, j->h, j->w * 3, j->w * 3 * j->h, j->bgr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(e);
    if (props) IPropertyBag2_Release(props);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (e) IWICBitmapEncoder_Release(e);
    if (s) IWICStream_Release(s);
    if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(co)) pCoUninit();
    return SUCCEEDED(hr);
}

static DWORD WINAPI shot_thread(LPVOID arg)
{
    shot_job *j = (shot_job *)arg;
    const char *rel = strstr(j->path, "screenshots\\");
    if (shot_write_png(j)) glog("screenshot saved: %s (%ux%u)", rel ? rel : j->path, j->w, j->h);
    else glog("screenshot: writing %s failed", j->path);
    HeapFree(GetProcessHeap(), 0, j->bgr);
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

/* called by present_rt with the finished back buffer, before Present */
static void shot_take_if_asked(void)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    shot_job *j;
    SYSTEMTIME t;
    char dir[MAX_PATH], *p;
    UINT x, y;
    long req = G.shot_req;
    if (req == g_shot_seen) return;
    g_shot_seen = req;
    if (!G.cfg_shots || !bb_tex) return;
    ID3D11Texture2D_GetDesc(bb_tex, &d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) { glog("screenshot: back buffer format %d not supported", (int)d.Format); return; }
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb_tex);
    j = (shot_job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *j);
    if (j) j->bgr = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)d.Width * d.Height * 3);
    if (!j || !j->bgr || FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        if (j) { if (j->bgr) HeapFree(GetProcessHeap(), 0, j->bgr); HeapFree(GetProcessHeap(), 0, j); }
        ID3D11Texture2D_Release(st);
        glog("screenshot: no memory or the copy could not be read");
        return;
    }
    for (y = 0; y < d.Height; y++) {
        const uint8_t *s = (const uint8_t *)m.pData + (SIZE_T)y * m.RowPitch;
        uint8_t *o = j->bgr + (SIZE_T)y * d.Width * 3;
        for (x = 0; x < d.Width; x++) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o += 3; s += 4; }
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
    j->w = d.Width; j->h = d.Height;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    p = strrchr(dir, '\\');
    if (p) p[1] = 0;
    strncat(dir, "screenshots", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    GetLocalTime(&t);
    _snprintf(j->path, sizeof j->path, "%s\\NBA99_%04d-%02d-%02d_%02d%02d%02d_%03d.png", dir, t.wYear, t.wMonth, t.wDay,
              t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    {
        HANDLE th = CreateThread(NULL, 0, shot_thread, j, 0, NULL);
        if (th) CloseHandle(th);
        else { HeapFree(GetProcessHeap(), 0, j->bgr); HeapFree(GetProcessHeap(), 0, j); }
    }
}
