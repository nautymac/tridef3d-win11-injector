// SRWeaveDX9.dll - weave a DirectX 9 game's side-by-side back buffer with the SR SDK, inside
// the game process, right before Present. Meant to sit under TriDef 3D (which renders the SBS
// image into the back buffer and then calls the real Present).
//
// Two ways to get in:
//   proxy    copy this DLL into the game folder as d3d9.dll. We forward every export to the
//            System32 d3d9.dll and install the hooks the first time Direct3DCreate9 is called.
//   injected load it under its own name (Tridef3D_Play --sr does this after TriDef's DLLs).
//            DllMain starts a thread that creates a throw-away device to find the functions.
// Either way the hooks are inline hooks (MinHook) on the *real* d3d9.dll functions, not
// vtable swaps, so TriDef's own hooking - wrapper object or vtable patch - ends up on top of
// us and its SBS output is already in the back buffer when we weave.
#include <windows.h>
#include <d3d9.h>
#include <map>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "common.h"
#include "sr/management/srcontext.h"
#include "sr/weaver/dx9weaver.h"

using namespace srw;

namespace {

// ---- real d3d9 -----------------------------------------------------------------------------

HMODULE g_d3d9 = nullptr;
std::once_flag g_realOnce;

HMODULE RealD3D9()
{
    std::call_once(g_realOnce, [] { g_d3d9 = LoadSystemDll(L"d3d9.dll"); });
    return g_d3d9;
}

template <typename T> T Real(const char* name)
{
    HMODULE m = RealD3D9();
    return m ? (T)GetProcAddress(m, name) : nullptr;
}

// ---- hooks ---------------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ScPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_PresentEx)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResetEx)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

PFN_Present   g_origPresent = nullptr;
PFN_Reset     g_origReset = nullptr;
PFN_ScPresent g_origScPresent = nullptr;
PFN_PresentEx g_origPresentEx = nullptr;
PFN_ResetEx   g_origResetEx = nullptr;

struct DevState {
    SR::IDX9Weaver1* weaver = nullptr;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
    IDirect3DStateBlock9* sb = nullptr;
    UINT w = 0, h = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    HWND hwnd = nullptr;
    bool failed = false;
    bool inputBound = false;
    unsigned frames = 0;
};
std::mutex g_devMutex;
std::map<IDirect3DDevice9*, DevState> g_devs;
thread_local bool t_inPresent = false;

HWND WindowOf(IDirect3DDevice9* dev)
{
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(dev->GetCreationParameters(&cp)) && cp.hFocusWindow) return cp.hFocusWindow;
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        D3DPRESENT_PARAMETERS pp = {};
        sc->GetPresentParameters(&pp);
        sc->Release();
        if (pp.hDeviceWindow) return pp.hDeviceWindow;
    }
    return GetForegroundWindow();
}

void ReleaseDeviceObjects(DevState& s)
{
    if (s.surf) { s.surf->Release(); s.surf = nullptr; }
    if (s.tex) { s.tex->Release(); s.tex = nullptr; }
    if (s.sb) { s.sb->Release(); s.sb = nullptr; }
    s.w = s.h = 0;
    s.inputBound = false;
}

bool EnsureWeaver(IDirect3DDevice9* dev, DevState& s)
{
    if (s.weaver) return true;
    if (!SrEnsureContext()) return false;
    s.hwnd = WindowOf(dev);
    WeaverErrorCode code = WeaverErrorCode::WeaverSuccess;
    try {
        code = SR::CreateDX9Weaver(g_sr.ctx, dev, s.hwnd, &s.weaver);
    } catch (const std::exception& e) {
        Log("CreateDX9Weaver threw: %s", e.what());
        s.weaver = nullptr;
    } catch (...) {
        Log("CreateDX9Weaver threw (unknown)");
        s.weaver = nullptr;
    }
    if (code != WeaverErrorCode::WeaverSuccess || !s.weaver) {
        Log("CreateDX9Weaver failed (code %d) - weaving disabled for device %p", (int)code, dev);
        s.weaver = nullptr;
        s.failed = true;
        return false;
    }
    try {
        s.weaver->setLatencyInFrames((uint64_t)g_cfg.latencyFrames);
        s.weaver->setOutputSRGBWrite(g_cfg.srgb);
    } catch (...) {}
    Log("DX9 weaver created for device %p (hwnd %p)", dev, s.hwnd);
    SrFinishInit();
    SrSetLens(g_cfg.lens && g_cfg.weave);
    return true;
}

bool EnsureTexture(IDirect3DDevice9* dev, DevState& s, const D3DSURFACE_DESC& d)
{
    if (s.tex && s.w == d.Width && s.h == d.Height && s.fmt == d.Format) return true;
    ReleaseDeviceObjects(s);
    D3DFORMAT fmt = d.Format;
    HRESULT hr = dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &s.tex, nullptr);
    if (FAILED(hr)) {
        fmt = D3DFMT_A8R8G8B8;
        hr = dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &s.tex, nullptr);
    }
    if (FAILED(hr) || !s.tex) {
        Log("CreateTexture %ux%u failed 0x%08lx", d.Width, d.Height, hr);
        s.tex = nullptr;
        return false;
    }
    if (FAILED(s.tex->GetSurfaceLevel(0, &s.surf))) { ReleaseDeviceObjects(s); return false; }
    s.w = d.Width; s.h = d.Height; s.fmt = fmt;
    Log("SBS texture %ux%u fmt %d (back buffer fmt %d, msaa %d)", s.w, s.h, (int)fmt, (int)d.Format, (int)d.MultiSampleType);
    return true;
}

void Weave(IDirect3DDevice9* dev, IDirect3DSurface9* bb)
{
    DevState* sp;
    {
        std::lock_guard<std::mutex> lk(g_devMutex);
        sp = &g_devs[dev];
    }
    DevState& s = *sp;
    if (s.failed) return;
    if (PollHotkeys()) SrSetLens(g_cfg.lens && g_cfg.weave);
    if (!g_cfg.weave) return;

    D3DSURFACE_DESC d = {};
    if (FAILED(bb->GetDesc(&d)) || d.Width < 2) return;
    if (!EnsureWeaver(dev, s)) return;
    if (!EnsureTexture(dev, s, d)) { s.failed = true; return; }

    const UINT w = s.w, h = s.h, hw = w / 2;
    const RECT L = { 0, 0, (LONG)hw, (LONG)h }, R = { (LONG)hw, 0, (LONG)w, (LONG)h };
    if (g_cfg.test) {
        dev->ColorFill(s.surf, &L, D3DCOLOR_XRGB(200, 0, 0));
        dev->ColorFill(s.surf, &R, D3DCOLOR_XRGB(0, 0, 200));
    } else if (g_cfg.swapEyes) {
        dev->StretchRect(bb, &L, s.surf, &R, D3DTEXF_POINT);
        dev->StretchRect(bb, &R, s.surf, &L, D3DTEXF_POINT);
    } else {
        dev->StretchRect(bb, nullptr, s.surf, nullptr, D3DTEXF_POINT);
    }
    if (!s.inputBound) {
        try { s.weaver->setInputViewTexture(s.tex, (int)hw, (int)h, s.fmt, g_cfg.srgb); } catch (...) {}
        s.inputBound = true;
    }

    // Save the game's state, weave into the back buffer, restore.
    if (!s.sb) dev->CreateStateBlock(D3DSBT_ALL, &s.sb);
    if (s.sb) s.sb->Capture();
    IDirect3DSurface9* rt0 = nullptr; dev->GetRenderTarget(0, &rt0);
    IDirect3DSurface9* ds = nullptr;  dev->GetDepthStencilSurface(&ds);
    D3DVIEWPORT9 vp = {}; dev->GetViewport(&vp);

    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 full = { 0, 0, w, h, 0.0f, 1.0f };
    dev->SetViewport(&full);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    bool began = SUCCEEDED(dev->BeginScene());   // fails harmlessly if the game left a scene open
    try {
        s.weaver->weave();
    } catch (const std::exception& e) {
        Log("weave() threw: %s - weaving disabled", e.what());
        s.failed = true;
    } catch (...) {
        Log("weave() threw (unknown) - weaving disabled");
        s.failed = true;
    }
    if (began) dev->EndScene();

    dev->SetRenderTarget(0, rt0); if (rt0) rt0->Release();
    dev->SetDepthStencilSurface(ds); if (ds) ds->Release();
    dev->SetViewport(&vp);
    if (s.sb) s.sb->Apply();

    if (++s.frames == 1) Log("first weaved frame: %ux%u", w, h);
}

void OnPresent(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc)
{
    if (t_inPresent || !dev) return;
    t_inPresent = true;
    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = sc ? sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                    : dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr) && bb) {
        Weave(dev, bb);
        bb->Release();
    }
    t_inPresent = false;
}

void OnBeforeReset(IDirect3DDevice9* dev)
{
    std::lock_guard<std::mutex> lk(g_devMutex);
    auto it = g_devs.find(dev);
    if (it == g_devs.end()) return;
    DevState& s = it->second;
    ReleaseDeviceObjects(s);
    if (s.weaver) { try { s.weaver->invalidateDeviceObjects(); } catch (...) {} }
    Log("Reset: device objects released");
}

void OnAfterReset(IDirect3DDevice9* dev, HRESULT hr)
{
    std::lock_guard<std::mutex> lk(g_devMutex);
    auto it = g_devs.find(dev);
    if (it == g_devs.end()) return;
    DevState& s = it->second;
    if (SUCCEEDED(hr) && s.weaver) { try { s.weaver->restoreDeviceObjects(); } catch (...) {} }
    Log("Reset done: 0x%08lx", hr);
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d)
{
    OnPresent(dev, nullptr);
    return g_origPresent(dev, a, b, c, d);
}

HRESULT STDMETHODCALLTYPE Hook_ScPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD f)
{
    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
        OnPresent(dev, sc);
        dev->Release();
    }
    return g_origScPresent(sc, a, b, c, d, f);
}

HRESULT STDMETHODCALLTYPE Hook_PresentEx(IDirect3DDevice9Ex* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD f)
{
    OnPresent(dev, nullptr);
    return g_origPresentEx(dev, a, b, c, d, f);
}

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    OnBeforeReset(dev);
    HRESULT hr = g_origReset(dev, pp);
    OnAfterReset(dev, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode)
{
    OnBeforeReset(dev);
    HRESULT hr = g_origResetEx(dev, pp, mode);
    OnAfterReset(dev, hr);
    return hr;
}

// ---- hook installation ---------------------------------------------------------------------


// The real d3d9 function behind slot `index` of a live vtable. TriDef copies each object's
// vtable to the heap and patches Present/Reset, so if the live slot doesn't point into d3d9.dll,
// recover the original from the on-disk image using the unpatched slots as a fingerprint.
void* ResolveReal(const char* name, void** liveVt, int nMatch, int index)
{
    void* live = nullptr;
    if (liveVt) live = liveVt[index];
    if (live && AddrInModule(live, g_d3d9)) return live;
    Log("%s: live slot %p is in %s - recovering the original from disk", name, live, ModuleOf(live).c_str());
    return CleanVtableEntryByFingerprint(g_d3d9, L"d3d9.dll", liveVt, nMatch, index);
}

void HookOne(const char* name, void* target, void* detour, void** orig)
{
    if (!target) { Log("%s: no target - not hooked", name); return; }
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    Log("hook %s @ %p (%s): %s", name, target, ModuleOf(target).c_str(), MH_StatusToString(st));
}

std::once_flag g_hookOnce;
bool g_hooked = false;

void InstallHooks()
{
    std::call_once(g_hookOnce, [] {
        auto pCreate = Real<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
        if (!pCreate) { Log("system d3d9.dll not available - no hooks"); return; }

        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = g_self;
        wc.lpszClassName = L"SRWeaveDX9Tmp";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, g_self, nullptr);

        if (MH_Initialize() != MH_OK) Log("MH_Initialize failed (already initialized?)");

        // A throw-away device. TriDef hooks device creation inside d3d9.dll itself, so this one
        // comes back wrapped with a patched heap vtable - that's fine, we only read the vtable
        // and let ResolveReal() find the originals. The objects are deliberately never released:
        // tearing down a TriDef-wrapped device confused TriDef and crashed the game.
        IDirect3D9* d3d = pCreate(D3D_SDK_VERSION);
        IDirect3DDevice9* dev = nullptr;
        if (d3d) {
            D3DPRESENT_PARAMETERS pp = {};
            pp.Windowed = TRUE;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = hwnd;
            pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
            HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                           D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
            if (FAILED(hr)) Log("temp CreateDevice failed 0x%08lx", hr);
        }
        if (dev) {
            // What TriDef hands back is a full wrapper (every vtable slot in TriDefD3D9.dll), so
            // there's nothing to fingerprint on it. Its swap-chain wrapper, however, keeps the
            // real IDirect3DSwapChain9 in a member: find that (vtable genuinely in d3d9.dll),
            // and ask *it* for the device - that's the real device object, whose vtable pointer
            // TriDef has redirected to a heap copy with Present/Reset patched. The unpatched
            // slots of that copy fingerprint the original vtable on disk.
            void** vt = *(void***)dev;
            Log("temp device %p vtable %p in %s", (void*)dev, (void*)vt, ModuleOf(vt).c_str());
            void** devVt = nullptr;
            void** scVt = nullptr;
            IDirect3DSwapChain9* wsc = nullptr;
            HRESULT hrSc = dev->GetSwapChain(0, &wsc);
            if (FAILED(hrSc) || !wsc) Log("temp GetSwapChain(0) failed: 0x%08lx (%p)", hrSc, (void*)wsc);
            if (SUCCEEDED(hrSc) && wsc) {
                Log("temp swap chain %p vtable in %s", (void*)wsc, ModuleOf(*(void**)wsc).c_str());
                void* realSc = nullptr;
                scVt = UnwrapVtable(wsc, g_d3d9, 9, &realSc);
                if (scVt && realSc) {
                    IDirect3DDevice9* rd = nullptr;
                    if (SUCCEEDED(((IDirect3DSwapChain9*)realSc)->GetDevice(&rd)) && rd) {
                        devVt = *(void***)rd;
                        Log("real device %p (from real swap chain) vtable %p in %s", (void*)rd, (void*)devVt, ModuleOf(devVt).c_str());
                        rd->Release();
                    } else {
                        Log("real swap chain %p: GetDevice failed", realSc);
                    }
                } else {
                    Log("could not see through the swap chain wrapper");
                }
            }
            // Fallback (Binary Domain): the wrapper's GetSwapChain fails, so look for the real
            // device directly among the device wrapper's own fields. Its vtable is TriDef's heap
            // copy, but most of its 119 slots still point into d3d9.dll.
            if (!devVt) {
                void** innerVt = nullptr;
                void* inner = nullptr;
                int field = 1;
                while (void* cand = FindInnerBySlots(dev, g_d3d9, 119, 80, &innerVt, &field)) {
                    // slot 0 lives in d3d9.dll, so this is a real d3d9 COM object - QI is safe
                    IDirect3DDevice9* asDev = nullptr;
                    HRESULT hq = ((IUnknown*)cand)->QueryInterface(__uuidof(IDirect3DDevice9), (void**)&asDev);
                    if (SUCCEEDED(hq) && asDev) { asDev->Release(); inner = cand; break; }
                    Log("  object %p is not a device (QI 0x%08lx) - keep looking", cand, hq);
                }
                if (inner) {
                    devVt = innerVt;
                    if (!scVt) {
                        // ask the real device for its real swap chain (GetSwapChain slot isn't patched)
                        IDirect3DSwapChain9* rsc = nullptr;
                        HRESULT hr = ((IDirect3DDevice9*)inner)->GetSwapChain(0, &rsc);
                        if (SUCCEEDED(hr) && rsc) {
                            void** rscVt = *(void***)rsc;
                            Log("real swap chain %p vtable %p in %s", (void*)rsc, (void*)rscVt, ModuleOf(rscVt).c_str());
                            scVt = rscVt;
                            rsc->Release();
                        } else {
                            Log("real device GetSwapChain(0) failed: 0x%08lx", hr);
                        }
                    }
                } else {
                    Log("no real device found among the device wrapper's fields");
                }
            }
            if (devVt) {
                void* present = ResolveReal("IDirect3DDevice9::Present", devVt, 16, 17);
                void* reset = ResolveReal("IDirect3DDevice9::Reset", devVt, 16, 16);
                // Both must resolve into d3d9.dll or we don't trust either.
                if (present && reset && AddrInModule(present, g_d3d9) && AddrInModule(reset, g_d3d9)) {
                    HookOne("IDirect3DDevice9::Reset", reset, (void*)&Hook_Reset, (void**)&g_origReset);
                    HookOne("IDirect3DDevice9::Present", present, (void*)&Hook_Present, (void**)&g_origPresent);
                } else {
                    Log("device Present/Reset not resolved into d3d9.dll - not hooking the device");
                }
            }
            if (scVt) {
                void* scPresent = ResolveReal("IDirect3DSwapChain9::Present", scVt, 10, 3);
                if (scPresent && AddrInModule(scPresent, g_d3d9))
                    HookOne("IDirect3DSwapChain9::Present", scPresent, (void*)&Hook_ScPresent, (void**)&g_origScPresent);
                else
                    Log("swap chain Present not resolved into d3d9.dll - not hooking it");
            }
        }
        // IDirect3DDevice9Ex::PresentEx/ResetEx are not hooked yet: getting at an Ex vtable means
        // creating an Ex device, which TriDef wraps as well and which destabilised the game.
        MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
        Log("MH_EnableHook: %s", MH_StatusToString(st));
        g_hooked = (st == MH_OK);
    });
}

DWORD WINAPI HookThread(LPVOID)
{
    if (g_cfg.hookDelayMs > 0) Sleep((DWORD)g_cfg.hookDelayMs);
    InstallHooks();
    return 0;
}

}   // namespace

// ---- proxy exports (used when this file is renamed to d3d9.dll) ---------------------------------

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk)
{
    InstallHooks();   // proxy mode: hook before the game gets its IDirect3D9
    auto f = Real<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
    return f ? f(sdk) : nullptr;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    InstallHooks();
    auto f = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
    return f ? f(sdk, out) : E_FAIL;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { auto f = Real<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent"); return f ? f(c, n) : 0; }
int WINAPI D3DPERF_EndEvent(void) { auto f = Real<int(WINAPI*)(void)>("D3DPERF_EndEvent"); return f ? f() : 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) { auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"); if (f) f(c, n); }
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) { auto f = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"); if (f) f(c, n); }
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { auto f = Real<BOOL(WINAPI*)(void)>("D3DPERF_QueryRepeatFrame"); return f ? f() : FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD o) { auto f = Real<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions"); if (f) f(o); }
DWORD WINAPI D3DPERF_GetStatus(void) { auto f = Real<DWORD(WINAPI*)(void)>("D3DPERF_GetStatus"); return f ? f() : 0; }
void WINAPI DebugSetMute(void) { auto f = Real<void(WINAPI*)(void)>("DebugSetMute"); if (f) f(); }
int WINAPI DebugSetLevel(void) { auto f = Real<int(WINAPI*)(void)>("DebugSetLevel"); return f ? f() : 0; }
void* WINAPI Direct3DShaderValidatorCreate9(void) { auto f = Real<void*(WINAPI*)(void)>("Direct3DShaderValidatorCreate9"); return f ? f() : nullptr; }
void WINAPI PSGPError(void* a, int b, unsigned c) { auto f = Real<void(WINAPI*)(void*, int, unsigned)>("PSGPError"); if (f) f(a, b, c); }
void WINAPI PSGPSampleTexture(void* a, unsigned b, float(*c)[4], unsigned d, float(*e)[4]) { auto f = Real<void(WINAPI*)(void*, unsigned, float(*)[4], unsigned, float(*)[4])>("PSGPSampleTexture"); if (f) f(a, b, c, d, e); }
int WINAPI Direct3D9EnableMaximizedWindowedModeShim(UINT a) { auto f = Real<int(WINAPI*)(UINT)>("Direct3D9EnableMaximizedWindowedModeShim"); return f ? f(a) : 0; }

}   // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        Init(inst, "SRWeaveDX9");
        bool proxy = SelfFileName() == L"d3d9.dll";
        Log("mode: %s", proxy ? "proxy (d3d9.dll)" : "injected");
        if (!proxy) {
            // Don't touch d3d9 from inside the loader lock; do it on our own thread.
            HANDLE t = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
