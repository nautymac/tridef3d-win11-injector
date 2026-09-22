// SRWeaveDX11.dll - weave a DirectX 11 game's side-by-side back buffer with the SR SDK inside
// the game process, right before IDXGISwapChain::Present. Sits under TriDef 3D's DX11 output.
//
//   proxy    copy into the game folder as dxgi.dll or d3d11.dll (exports below forward to the
//            System32 originals; hooks are installed on the first factory/device call)
//   injected load under its own name (Tridef3D_Play --sr). A throw-away device+swapchain on a
//            hidden window yields the real IDXGISwapChain function addresses.
// Hooks are inline (MinHook) on the real dxgi.dll functions, so whatever TriDef installs sits on
// top and its SBS image is already in the back buffer when we run.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <map>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "common.h"
#include "sr/management/srcontext.h"
#include "sr/weaver/dx11weaver.h"

using namespace srw;

namespace {

template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---- real dxgi / d3d11 ---------------------------------------------------------------------

HMODULE g_dxgi = nullptr, g_d3d11 = nullptr;
std::once_flag g_realOnce;

void LoadReal()
{
    std::call_once(g_realOnce, [] {
        g_dxgi = LoadSystemDll(L"dxgi.dll");
        g_d3d11 = LoadSystemDll(L"d3d11.dll");
    });
}
template <typename T> T RealDxgi(const char* n) { LoadReal(); return g_dxgi ? (T)GetProcAddress(g_dxgi, n) : nullptr; }
template <typename T> T RealD3D11(const char* n) { LoadReal(); return g_d3d11 ? (T)GetProcAddress(g_d3d11, n) : nullptr; }

// ---- per-swapchain state -------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PFN_Present       g_origPresent = nullptr;
PFN_Present1      g_origPresent1 = nullptr;
PFN_ResizeBuffers g_origResizeBuffers = nullptr;

struct ScState {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    SR::IDX11Weaver1* weaver = nullptr;
    ID3D11Texture2D* tex = nullptr;          // SBS copy of the back buffer
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* texRtv = nullptr; // for the test pattern
    ID3D11RenderTargetView* bbRtv = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;   // SRV/RTV format (typeless resolved)
    HWND hwnd = nullptr;
    bool failed = false;
    bool inputBound = false;
    unsigned frames = 0;
};
std::mutex g_scMutex;
std::map<IDXGISwapChain*, ScState> g_scs;
thread_local bool t_inPresent = false;

DXGI_FORMAT ViewFormat(DXGI_FORMAT f, bool srgb)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

// Everything the weaver's own draw might clobber. Games and TriDef set their state each frame
// anyway, but be polite.
struct SavedState {
    ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr;
    UINT nvp = 16; D3D11_VIEWPORT vp[16] = {};
    UINT nsc = 16; D3D11_RECT sc[16] = {};
    ID3D11RasterizerState* rs = nullptr;
    ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT sm = 0;
    ID3D11DepthStencilState* dss = nullptr; UINT sref = 0;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr; ID3D11ComputeShader* cs = nullptr;
    ID3D11InputLayout* il = nullptr; D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb[2] = {}; UINT vbs[2] = {}, vbo[2] = {};
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibf = DXGI_FORMAT_UNKNOWN; UINT ibo = 0;
    ID3D11ShaderResourceView* psSrv[8] = {}; ID3D11SamplerState* psSamp[4] = {};
    ID3D11Buffer* vsCb[4] = {}; ID3D11Buffer* psCb[4] = {};

    void Capture(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(8, rtv, &dsv);
        c->RSGetViewports(&nvp, vp);
        c->RSGetScissorRects(&nsc, sc);
        c->RSGetState(&rs);
        c->OMGetBlendState(&bs, bf, &sm);
        c->OMGetDepthStencilState(&dss, &sref);
        c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr); c->CSGetShader(&cs, nullptr, nullptr);
        c->IAGetInputLayout(&il); c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, 2, vb, vbs, vbo); c->IAGetIndexBuffer(&ib, &ibf, &ibo);
        c->PSGetShaderResources(0, 8, psSrv); c->PSGetSamplers(0, 4, psSamp);
        c->VSGetConstantBuffers(0, 4, vsCb); c->PSGetConstantBuffers(0, 4, psCb);
    }
    void Restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(8, rtv, dsv);
        c->RSSetViewports(nvp, vp);
        c->RSSetScissorRects(nsc, sc);
        c->RSSetState(rs);
        c->OMSetBlendState(bs, bf, sm);
        c->OMSetDepthStencilState(dss, sref);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0); c->DSSetShader(ds, nullptr, 0); c->CSSetShader(cs, nullptr, 0);
        c->IASetInputLayout(il); c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, 2, vb, vbs, vbo); c->IASetIndexBuffer(ib, ibf, ibo);
        c->PSSetShaderResources(0, 8, psSrv); c->PSSetSamplers(0, 4, psSamp);
        c->VSSetConstantBuffers(0, 4, vsCb); c->PSSetConstantBuffers(0, 4, psCb);
        for (auto& p : rtv) SafeRelease(p);
        SafeRelease(dsv); SafeRelease(rs); SafeRelease(bs); SafeRelease(dss);
        SafeRelease(vs); SafeRelease(ps); SafeRelease(gs); SafeRelease(hs); SafeRelease(ds); SafeRelease(cs);
        SafeRelease(il); for (auto& p : vb) SafeRelease(p); SafeRelease(ib);
        for (auto& p : psSrv) SafeRelease(p); for (auto& p : psSamp) SafeRelease(p);
        for (auto& p : vsCb) SafeRelease(p); for (auto& p : psCb) SafeRelease(p);
    }
};

void ReleaseSizeObjects(ScState& s)
{
    SafeRelease(s.srv); SafeRelease(s.texRtv); SafeRelease(s.tex); SafeRelease(s.bbRtv);
    s.w = s.h = 0;
    s.inputBound = false;
}

bool EnsureWeaver(IDXGISwapChain* sc, ScState& s)
{
    if (s.weaver) return true;
    if (!SrEnsureContext()) return false;
    DXGI_SWAP_CHAIN_DESC d = {};
    sc->GetDesc(&d);
    s.hwnd = d.OutputWindow ? d.OutputWindow : GetForegroundWindow();
    WeaverErrorCode code = WeaverErrorCode::WeaverSuccess;
    try {
        code = SR::CreateDX11Weaver(g_sr.ctx, s.ctx, s.hwnd, &s.weaver);
    } catch (const std::exception& e) {
        Log("CreateDX11Weaver threw: %s", e.what());
        s.weaver = nullptr;
    } catch (...) {
        Log("CreateDX11Weaver threw (unknown)");
        s.weaver = nullptr;
    }
    if (code != WeaverErrorCode::WeaverSuccess || !s.weaver) {
        Log("CreateDX11Weaver failed (code %d) - weaving disabled for swapchain %p", (int)code, sc);
        s.weaver = nullptr;
        s.failed = true;
        return false;
    }
    try {
        s.weaver->setLatencyInFrames((uint64_t)g_cfg.latencyFrames);
        s.weaver->setContext(s.ctx);
    } catch (...) {}
    Log("DX11 weaver created for swapchain %p (hwnd %p)", sc, s.hwnd);
    SrFinishInit();
    SrSetLens(g_cfg.lens && g_cfg.weave);
    return true;
}

bool EnsureObjects(ScState& s, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bd)
{
    DXGI_FORMAT vf = ViewFormat(bd.Format, g_cfg.srgb);
    if (s.tex && s.w == bd.Width && s.h == bd.Height && s.fmt == vf) return true;
    ReleaseSizeObjects(s);
    D3D11_TEXTURE2D_DESC td = bd;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = 0; td.CPUAccessFlags = 0; td.Usage = D3D11_USAGE_DEFAULT;
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    HRESULT hr = s.dev->CreateTexture2D(&td, nullptr, &s.tex);
    if (FAILED(hr)) { Log("CreateTexture2D %ux%u fmt %d failed 0x%08lx", bd.Width, bd.Height, (int)bd.Format, hr); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = vf; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    hr = s.dev->CreateShaderResourceView(s.tex, &sd, &s.srv);
    if (FAILED(hr)) { Log("CreateShaderResourceView failed 0x%08lx", hr); ReleaseSizeObjects(s); return false; }
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = vf; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    s.dev->CreateRenderTargetView(s.tex, &rd, &s.texRtv);
    rd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = s.dev->CreateRenderTargetView(bb, &rd, &s.bbRtv);
    if (FAILED(hr)) { Log("CreateRenderTargetView(back buffer) failed 0x%08lx", hr); ReleaseSizeObjects(s); return false; }
    s.w = bd.Width; s.h = bd.Height; s.fmt = vf;
    Log("SBS texture %ux%u view fmt %d (back buffer fmt %d, msaa %u)", s.w, s.h, (int)vf, (int)bd.Format, bd.SampleDesc.Count);
    return true;
}

void Weave(IDXGISwapChain* sc)
{
    ScState* sp;
    {
        std::lock_guard<std::mutex> lk(g_scMutex);
        sp = &g_scs[sc];
    }
    ScState& s = *sp;
    if (s.failed) return;
    if (PollHotkeys()) SrSetLens(g_cfg.lens && g_cfg.weave);
    if (!g_cfg.weave) return;

    if (!s.dev) {
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&s.dev)) || !s.dev) { s.failed = true; Log("swapchain has no D3D11 device"); return; }
        s.dev->GetImmediateContext(&s.ctx);
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC bd = {};
    bb->GetDesc(&bd);
    if (bd.Width < 2) { bb->Release(); return; }

    if (!EnsureWeaver(sc, s) || !EnsureObjects(s, bb, bd)) { bb->Release(); return; }

    const UINT w = s.w, h = s.h, hw = w / 2;
    ID3D11DeviceContext* c = s.ctx;
    if (g_cfg.test && s.texRtv) {
        ID3D11DeviceContext1* c1 = nullptr;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) && c1) {
            const float red[4] = { 0.8f, 0, 0, 1 }, blue[4] = { 0, 0, 0.8f, 1 };
            D3D11_RECT L = { 0, 0, (LONG)hw, (LONG)h }, R = { (LONG)hw, 0, (LONG)w, (LONG)h };
            c1->ClearView(s.texRtv, red, &L, 1);
            c1->ClearView(s.texRtv, blue, &R, 1);
            c1->Release();
        } else {
            const float red[4] = { 0.8f, 0, 0, 1 };
            c->ClearRenderTargetView(s.texRtv, red);
        }
    } else if (bd.SampleDesc.Count > 1) {
        c->ResolveSubresource(s.tex, 0, bb, 0, s.fmt);
    } else if (g_cfg.swapEyes) {
        D3D11_BOX L = { 0, 0, 0, hw, h, 1 }, R = { hw, 0, 0, w, h, 1 };
        c->CopySubresourceRegion(s.tex, 0, hw, 0, 0, bb, 0, &L);
        c->CopySubresourceRegion(s.tex, 0, 0, 0, 0, bb, 0, &R);
    } else {
        c->CopyResource(s.tex, bb);
    }
    if (!s.inputBound) {
        try { s.weaver->setInputViewTexture(s.srv, (int)hw, (int)h, s.fmt); } catch (...) {}
        s.inputBound = true;
    }

    SavedState st;
    st.Capture(c);
    ID3D11RenderTargetView* rt = s.bbRtv;
    c->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    c->RSSetViewports(1, &vp);
    D3D11_RECT full = { 0, 0, (LONG)w, (LONG)h };
    c->RSSetScissorRects(1, &full);
    try {
        s.weaver->weave();
    } catch (const std::exception& e) {
        Log("weave() threw: %s - weaving disabled", e.what());
        s.failed = true;
    } catch (...) {
        Log("weave() threw (unknown) - weaving disabled");
        s.failed = true;
    }
    st.Restore(c);
    bb->Release();
    if (++s.frames == 1) Log("first weaved frame: %ux%u", w, h);
}

void OnPresent(IDXGISwapChain* sc, UINT flags)
{
    if (t_inPresent || !sc || (flags & DXGI_PRESENT_TEST)) return;
    t_inPresent = true;
    Weave(sc);
    t_inPresent = false;
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    OnPresent(sc, flags);
    return g_origPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    OnPresent(sc, flags);
    return g_origPresent1(sc, sync, flags, pp);
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    {
        std::lock_guard<std::mutex> lk(g_scMutex);
        auto it = g_scs.find(sc);
        if (it != g_scs.end()) { ReleaseSizeObjects(it->second); Log("ResizeBuffers %ux%u: size objects released", w, h); }
    }
    return g_origResizeBuffers(sc, n, w, h, f, flags);
}

// ---- hook installation ---------------------------------------------------------------------


// See dx9.cpp: the real dxgi function behind a live (possibly TriDef-patched heap copy) vtable slot.
void* ResolveReal(const char* name, void** liveVt, int nMatch, int index)
{
    void* live = nullptr;
    if (liveVt) live = liveVt[index];
    if (live && AddrInModule(live, g_dxgi)) return live;
    Log("%s: live slot %p is in %s - recovering the original from disk", name, live, ModuleOf(live).c_str());
    return CleanVtableEntryByFingerprint(g_dxgi, L"dxgi.dll", liveVt, nMatch, index);
}

void HookOne(const char* name, void* target, void* detour, void** orig)
{
    if (!target) { Log("%s: no target - not hooked", name); return; }
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    Log("hook %s @ %p (%s): %s", name, target, ModuleOf(target).c_str(), MH_StatusToString(st));
}

std::once_flag g_hookOnce;

void InstallHooks()
{
    std::call_once(g_hookOnce, [] {
        typedef HRESULT(WINAPI* PFN_CreateDevSc)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                  const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        auto pCreate = RealD3D11<PFN_CreateDevSc>("D3D11CreateDeviceAndSwapChain");
        if (!pCreate) { Log("system d3d11.dll not available - no hooks"); return; }

        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = g_self;
        wc.lpszClassName = L"SRWeaveDX11Tmp";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        // Throw-away swap chain; possibly TriDef-wrapped, never released (see dx9.cpp).
        IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
        HRESULT hr = pCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (FAILED(hr) || !sc) { Log("temp D3D11CreateDeviceAndSwapChain failed 0x%08lx", hr); }

        if (MH_Initialize() != MH_OK) Log("MH_Initialize failed (already initialized?)");
        if (sc) {
            void** vt = *(void***)sc;
            Log("temp swap chain %p vtable %p in %s", (void*)sc, (void*)vt, ModuleOf(vt).c_str());
            HookOne("IDXGISwapChain::Present", ResolveReal("IDXGISwapChain::Present", vt, 18, 8), (void*)&Hook_Present, (void**)&g_origPresent);
            HookOne("IDXGISwapChain::ResizeBuffers", ResolveReal("IDXGISwapChain::ResizeBuffers", vt, 18, 13), (void*)&Hook_ResizeBuffers, (void**)&g_origResizeBuffers);
            IDXGISwapChain1* sc1 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
                void** vt1 = *(void***)sc1;
                HookOne("IDXGISwapChain1::Present1", ResolveReal("IDXGISwapChain1::Present1", vt1, 18, 22), (void*)&Hook_Present1, (void**)&g_origPresent1);
            }
        }
        MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
        Log("MH_EnableHook: %s", MH_StatusToString(st));
    });
}

DWORD WINAPI HookThread(LPVOID)
{
    if (g_cfg.hookDelayMs > 0) Sleep((DWORD)g_cfg.hookDelayMs);
    InstallHooks();
    return 0;
}

}   // namespace

// ---- proxy exports (when renamed to dxgi.dll or d3d11.dll) ---------------------------------------

extern "C" {

HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** out)
{
    InstallHooks();
    auto f = RealDxgi<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory");
    return f ? f(riid, out) : E_FAIL;
}
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** out)
{
    InstallHooks();
    auto f = RealDxgi<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory1");
    return f ? f(riid, out) : E_FAIL;
}
HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    InstallHooks();
    auto f = RealDxgi<HRESULT(WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    return f ? f(flags, riid, out) : E_FAIL;
}
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void)
{
    auto f = RealDxgi<HRESULT(WINAPI*)(void)>("DXGIDeclareAdapterRemovalSupport");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** out)
{
    auto f = RealDxgi<HRESULT(WINAPI*)(UINT, REFIID, void**)>("DXGIGetDebugInterface1");
    return f ? f(flags, riid, out) : E_FAIL;
}
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                                 UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** ctx)
{
    InstallHooks();
    typedef HRESULT(WINAPI* F)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    auto f = RealD3D11<F>("D3D11CreateDevice");
    return f ? f(a, t, sw, flags, fl, nfl, sdk, dev, outFl, ctx) : E_FAIL;
}
HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl, UINT nfl,
                                             UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** dev,
                                             D3D_FEATURE_LEVEL* outFl, ID3D11DeviceContext** ctx)
{
    InstallHooks();
    typedef HRESULT(WINAPI* F)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*,
                               IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    auto f = RealD3D11<F>("D3D11CreateDeviceAndSwapChain");
    return f ? f(a, t, sw, flags, fl, nfl, sdk, sd, sc, dev, outFl, ctx) : E_FAIL;
}

}   // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init(inst, "SRWeaveDX11");
        std::wstring n = SelfFileName();
        bool proxy = (n == L"dxgi.dll" || n == L"d3d11.dll");
        Log("mode: %s", proxy ? "proxy" : "injected");
        if (!proxy) {
            HANDLE t = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
