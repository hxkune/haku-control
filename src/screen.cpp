// SPDX-License-Identifier: GPL-3.0-only
// The screen, for the Screen effect (ambilight): a thread copies the desktop with DXGI Desktop Duplication,
// lets the graphics card shrink it (mipmaps: a 4K picture becomes ~120x68 without the processor touching its
// pixels) and keeps that small picture, softened over time, for the effects to read. It runs only while an
// effect needs it (screen_use), and stops a few seconds after the last use. [screen] monitor = which screen
// (1 = the first; Windows' own order). Protected video and the secure desktop (UAC) come out black, as in any
// screen capture; when the desktop is lost (resolution change, UAC, sleep) the copy is set up again.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <process.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
// from common.h (C only)
typedef struct { float r, g, b; } rgbf;
extern "C" void logf_(const char *fmt, ...);
extern "C" float cfg_getf(const char *section, const char *key, float def);

#define GW 160   // largest picture kept
#define GH 120

static SRWLOCK lk = SRWLOCK_INIT;
static float grid[GH][GW][3];        // linear-ish 0..1, softened
static int gw, gh, have;              // size of the picture now, and whether there is one
static volatile LONG want;            // last time an effect asked for it (GetTickCount)
static volatile LONG running;
static volatile LONG monitors;        // outputs found
static char err[96];

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

struct cap_t {
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    IDXGIOutputDuplication *dup = nullptr;
    ID3D11Texture2D *mip = nullptr, *stage = nullptr; ID3D11ShaderResourceView *srv = nullptr;
    UINT w = 0, h = 0, level = 0, lw = 0, lh = 0;
    void close() { rel(stage); rel(srv); rel(mip); rel(dup); rel(ctx); rel(dev); }
};

static void set_err(const char *e) { AcquireSRWLockExclusive(&lk); snprintf(err, sizeof(err), "%s", e); ReleaseSRWLockExclusive(&lk); }

// the duplication of output number idx (counted over all adapters), with a device on that output's adapter
static int open_cap(cap_t &c, int idx) {
    c.close();
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) { set_err("no DXGI"); return 0; }
    IDXGIAdapter1 *ad = nullptr; IDXGIOutput *out = nullptr; int n = 0, found = 0;
    for (UINT a = 0; !found && f->EnumAdapters1(a, &ad) != DXGI_ERROR_NOT_FOUND; a++) {
        for (UINT o = 0; ad->EnumOutputs(o, &out) != DXGI_ERROR_NOT_FOUND; o++) {
            if (n++ == idx) { found = 1; break; }
            rel(out);
        }
        if (!found) rel(ad);
    }
    // count every output once, for the window's choice
    { int total = 0; IDXGIAdapter1 *a2; IDXGIOutput *o2;
      for (UINT a = 0; f->EnumAdapters1(a, &a2) != DXGI_ERROR_NOT_FOUND; a++) { for (UINT o = 0; a2->EnumOutputs(o, &o2) != DXGI_ERROR_NOT_FOUND; o++) { total++; o2->Release(); } a2->Release(); }
      InterlockedExchange(&monitors, total); }
    f->Release();
    if (!found) { set_err("no such screen"); return 0; }
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &c.dev, &fl, &c.ctx);
    rel(ad);
    if (FAILED(hr)) { rel(out); set_err("no Direct3D device"); return 0; }
    IDXGIOutput1 *o1 = nullptr;
    hr = out->QueryInterface(__uuidof(IDXGIOutput1), (void **)&o1);
    rel(out);
    if (FAILED(hr)) { c.close(); set_err("no DXGI 1.2"); return 0; }
    hr = o1->DuplicateOutput(c.dev, &c.dup);
    rel(o1);
    if (FAILED(hr)) { c.close(); set_err(hr == E_ACCESSDENIED ? "screen not readable now" : "screen copy refused"); return 0; }
    DXGI_OUTDUPL_DESC dd; c.dup->GetDesc(&dd);
    c.w = dd.ModeDesc.Width; c.h = dd.ModeDesc.Height;
    // a texture with its mip chain: the copy goes to level 0, the graphics card makes the smaller ones
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = c.w; td.Height = c.h; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(c.dev->CreateTexture2D(&td, NULL, &c.mip)) || FAILED(c.dev->CreateShaderResourceView(c.mip, NULL, &c.srv))) { c.close(); set_err("no texture"); return 0; }
    c.level = 0; c.lw = c.w; c.lh = c.h;
    while (c.lw > GW || c.lh > GH) { c.level++; c.lw = max(1u, c.lw / 2); c.lh = max(1u, c.lh / 2); }
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = c.lw; sd.Height = c.lh; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(c.dev->CreateTexture2D(&sd, NULL, &c.stage))) { c.close(); set_err("no staging texture"); return 0; }
    set_err("");
    logf_("screen: copying screen %d (%ux%u, read at %ux%u)", idx + 1, c.w, c.h, c.lw, c.lh);
    return 1;
}

// one frame: 1 taken, 0 nothing new, -1 the copy is lost
static int grab(cap_t &c, double dt) {
    DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = nullptr;
    HRESULT hr = c.dup->AcquireNextFrame(50, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) return -1;
    ID3D11Texture2D *tex = nullptr;
    int ok = 0;
    if (fi.LastPresentTime.QuadPart && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) {
        c.ctx->CopySubresourceRegion(c.mip, 0, 0, 0, 0, tex, 0, NULL);
        c.ctx->GenerateMips(c.srv);
        c.ctx->CopySubresourceRegion(c.stage, 0, 0, 0, 0, c.mip, c.level, NULL);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c.ctx->Map(c.stage, 0, D3D11_MAP_READ, 0, &m))) {
            // softened over time ([screen] speed: the effect's speed, 1 slow .. 10 instant)
            float a = (float)min(1.0, dt * (1.5 + 3.0 * cfg_getf("screen", "speed", 5)));
            AcquireSRWLockExclusive(&lk);
            if (!have || gw != (int)c.lw || gh != (int)c.lh) a = 1;
            gw = c.lw; gh = c.lh;
            for (UINT y = 0; y < c.lh; y++) {
                const BYTE *row = (const BYTE *)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < c.lw; x++) {
                    const BYTE *p = row + x * 4;
                    float *g = grid[y][x];
                    g[0] += (p[2] / 255.0f - g[0]) * a; g[1] += (p[1] / 255.0f - g[1]) * a; g[2] += (p[0] / 255.0f - g[2]) * a;
                }
            }
            have = 1;
            ReleaseSRWLockExclusive(&lk);
            c.ctx->Unmap(c.stage, 0);
            ok = 1;
        }
        tex->Release();
    }
    res->Release();
    c.dup->ReleaseFrame();
    return ok;
}

static unsigned __stdcall run(void *) {
    cap_t c; int idx = -1; DWORD last = GetTickCount(), retry = 0;
    while (GetTickCount() - (DWORD)want < 4000) {
        int mon = (int)cfg_getf("screen", "monitor", 1) - 1; if (mon < 0) mon = 0;
        if (!c.dup || mon != idx) {
            if (GetTickCount() < retry) { Sleep(100); continue; }
            idx = mon;
            if (!open_cap(c, idx)) { retry = GetTickCount() + 2000; continue; }
        }
        DWORD now = GetTickCount(); double dt = (now - last) / 1000.0; last = now;
        int r = grab(c, dt);
        if (r < 0) { c.close(); logf_("screen: the copy was lost, setting it up again"); retry = GetTickCount() + 500; continue; }
        Sleep(25);   // ~30 pictures a second at most
    }
    c.close();
    AcquireSRWLockExclusive(&lk); have = 0; ReleaseSRWLockExclusive(&lk);
    logf_("screen: stopped (no effect uses it)");
    InterlockedExchange(&running, 0);
    return 0;
}

extern "C" void screen_use(void) {
    InterlockedExchange(&want, (LONG)GetTickCount());
    if (InterlockedCompareExchange(&running, 1, 0) == 0) {
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, run, NULL, 0, NULL);
        if (t) CloseHandle(t); else InterlockedExchange(&running, 0);
    }
}

// the average colour of a part of the screen (u, v: 0..1 from the top left); 0 when there is no picture yet
extern "C" int screen_area(float u0, float v0, float u1, float v1, rgbf *out) {
    AcquireSRWLockShared(&lk);
    int ok = have && gw && gh;
    if (ok) {
        int x0 = (int)floorf(fminf(u0, u1) * gw), x1 = (int)ceilf(fmaxf(u0, u1) * gw), y0 = (int)floorf(fminf(v0, v1) * gh), y1 = (int)ceilf(fmaxf(v0, v1) * gh);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > gw) x1 = gw; if (y1 > gh) y1 = gh;
        if (x1 <= x0) x1 = min(gw, x0 + 1); if (y1 <= y0) y1 = min(gh, y0 + 1);
        float r = 0, g = 0, b = 0; int n = 0;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { r += grid[y][x][0]; g += grid[y][x][1]; b += grid[y][x][2]; n++; }
        if (n) { out->r = r / n; out->g = g / n; out->b = b / n; } else ok = 0;
    }
    ReleaseSRWLockShared(&lk);
    return ok;
}

static void count_outputs(void) {
    IDXGIFactory1 *f = nullptr; int total = 0;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return;
    IDXGIAdapter1 *a; IDXGIOutput *o;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) { for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) { total++; o->Release(); } a->Release(); }
    f->Release();
    InterlockedExchange(&monitors, total);
}

// "screen":{"on":1,"have":1,"monitors":2,"w":120,"h":68,"err":""}
extern "C" int screen_json(char *out, int cap) {
    static int counted;
    if (!counted) { counted = 1; count_outputs(); }
    AcquireSRWLockShared(&lk);
    int n = snprintf(out, cap, "\"screen\":{\"on\":%ld,\"have\":%d,\"monitors\":%ld,\"w\":%d,\"h\":%d,\"err\":\"%s\"}", running, have, monitors, gw, gh, err);
    ReleaseSRWLockShared(&lk);
    return n;
}
