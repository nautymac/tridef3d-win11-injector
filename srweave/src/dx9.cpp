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
//
// upscale=1 (SRWeave.ini): the weave has to land 1:1 on the panel's pixels, so a game running
// below the panel's resolution cannot be weaved into its own back buffer (the display would
// stretch the weaved picture and the lens pattern with it). Instead:
//   - CreateDevice / Reset are made windowed underneath the game (it still believes it is
//     fullscreen at its own resolution), and the game window is made borderless over the monitor;
//   - at Present the SBS back buffer is weaved into an additional swap chain that has the
//     monitor's resolution, and that swap chain is presented in place of the game's;
//   - the mouse has to arrive in the game's resolution. Under TriDef 3D nothing is done for
//     that, because it already does: the game still clips the cursor to its own resolution
//     (TriDef answers its GetClientRect / GetWindowRect with that size), so the real cursor
//     position IS the game position, and the pointer the user sees is the one drawn into the
//     picture, which is upscaled with it. Measured in Dragon Age: Origins at 1440p and 1080p on
//     a 4K panel. Scaling here as well was wrong and not even consistently wrong - TriDef's
//     faked window size reaches our own GetClientRect only some of the time - so clicks missed
//     and a dragged model jumped between two angles. Without TriDef the window messages and
//     GetCursorPos / SetCursorPos / ClipCursor are scaled here (upscale_mouse; untested).
#include <windows.h>
#include <windowsx.h>
#include <intrin.h>
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
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
PFN_CreateDevice g_origCreateDevice = nullptr;
BOOL(WINAPI* g_origGetCursorPos)(LPPOINT) = nullptr;
BOOL(WINAPI* g_origSetCursorPos)(int, int) = nullptr;
BOOL(WINAPI* g_origClipCursor)(const RECT*) = nullptr;
BOOL(WINAPI* g_origGetClientRect)(HWND, LPRECT) = nullptr;
BOOL(WINAPI* g_origGetWindowRect)(HWND, LPRECT) = nullptr;
BOOL(WINAPI* g_origScreenToClient)(HWND, LPPOINT) = nullptr;
UINT(WINAPI* g_origGetRawInputData)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = nullptr;
BOOL(WINAPI* g_origGetCursorInfo)(PCURSORINFO) = nullptr;
DWORD(WINAPI* g_origGetMessagePos)(void) = nullptr;
BOOL(WINAPI* g_origGetPhysicalCursorPos)(LPPOINT) = nullptr;
BOOL(WINAPI* g_origClientToScreen)(HWND, LPPOINT) = nullptr;

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
    // upscale
    IDirect3DSwapChain9* usc = nullptr;   // additional swap chain at the monitor's resolution
    IDirect3DSurface9* uscBB = nullptr;
    UINT nw = 0, nh = 0;
    bool upFailed = false;
    unsigned upFrames = 0;
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
    if (s.uscBB) { s.uscBB->Release(); s.uscBB = nullptr; }
    if (s.usc) { s.usc->Release(); s.usc = nullptr; }
    s.nw = s.nh = 0;
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
    LogVpcProbes();
    Log("DX9 weaver created for device %p (hwnd %p)", dev, s.hwnd);
    SrFinishInit();
    SrSetLens(g_cfg.lens && g_cfg.weave);
    return true;
}

bool EnsureTexture(IDirect3DDevice9* dev, DevState& s, const D3DSURFACE_DESC& d)
{
    if (s.tex && s.w == d.Width && s.h == d.Height && s.fmt == d.Format) return true;
    if (s.surf) { s.surf->Release(); s.surf = nullptr; }
    if (s.tex) { s.tex->Release(); s.tex = nullptr; }
    s.w = s.h = 0;
    s.inputBound = false;
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
    if (FAILED(s.tex->GetSurfaceLevel(0, &s.surf))) { s.tex->Release(); s.tex = nullptr; return false; }
    s.w = d.Width; s.h = d.Height; s.fmt = fmt;
    Log("SBS texture %ux%u fmt %d (back buffer fmt %d, msaa %d)", s.w, s.h, (int)fmt, (int)d.Format, (int)d.MultiSampleType);
    return true;
}

// Copies the game's SBS back buffer into s.tex (resolving MSAA; eye swap / test pattern applied)
// and hands the texture to the weaver once.
bool PrepareInput(IDirect3DDevice9* dev, DevState& s, IDirect3DSurface9* bb, const D3DSURFACE_DESC& d)
{
    if (!EnsureTexture(dev, s, d)) return false;
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
    if (s.weaver && !s.inputBound) {
        try { s.weaver->setInputViewTexture(s.tex, (int)hw, (int)h, s.fmt, g_cfg.srgb); } catch (...) {}
        s.inputBound = true;
    }
    return true;
}

// Saves the game's state, weaves s.tex into `target` (tw x th), restores.
void DrawWeave(IDirect3DDevice9* dev, DevState& s, IDirect3DSurface9* target, UINT tw, UINT th)
{
    if (!s.sb) dev->CreateStateBlock(D3DSBT_ALL, &s.sb);
    if (s.sb) s.sb->Capture();
    IDirect3DSurface9* rt0 = nullptr; dev->GetRenderTarget(0, &rt0);
    IDirect3DSurface9* ds = nullptr;  dev->GetDepthStencilSurface(&ds);
    D3DVIEWPORT9 vp = {}; dev->GetViewport(&vp);

    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 full = { 0, 0, tw, th, 0.0f, 1.0f };
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
}

DevState& StateOf(IDirect3DDevice9* dev)
{
    std::lock_guard<std::mutex> lk(g_devMutex);
    return g_devs[dev];
}

void Weave(IDirect3DDevice9* dev, IDirect3DSurface9* bb)
{
    DevState& s = StateOf(dev);
    if (s.failed) return;
    if (PollHotkeys()) SrSetLens(g_cfg.lens && g_cfg.weave);
    if (!g_cfg.weave) return;

    D3DSURFACE_DESC d = {};
    if (FAILED(bb->GetDesc(&d)) || d.Width < 2) return;
    if (!EnsureWeaver(dev, s)) return;
    if (!PrepareInput(dev, s, bb, d)) { s.failed = true; return; }
    DrawWeave(dev, s, bb, s.w, s.h);
    if (++s.frames == 1) Log("first weaved frame: %ux%u", s.w, s.h);
}

// ---- upscale ---------------------------------------------------------------------------------

bool MonitorPixels(HWND hwnd, UINT* pw, UINT* ph, RECT* seen);

struct Upscale {
    volatile bool forced = false;   // we turned the game's fullscreen device into a windowed one
    volatile bool active = false;   // frames are going out through the native-resolution swap chain
    HWND hwnd = nullptr;
    int gw = 0, gh = 0;             // the game's resolution
    WNDPROC oldProc = nullptr;
    HWND procHwnd = nullptr;
    bool unicode = false;
    UINT fitMsg = 0;
    ULONGLONG lastFit = 0;
} g_up;

// What is scaled back to the game's resolution: bit 0 = mouse messages and WM_SIZE, bit 1 = the
// cursor API. See the note at the top of the file for why this is nothing under TriDef.
int MouseMode()
{
    static const int mode = [] {
        int m = g_cfg.upscaleMouse;
        if (m < 0) {
            const bool tridef = GetModuleHandleW(L"TriDefD3D9.dll") || GetModuleHandleW(L"TriDefIgnition.dll");
            m = tridef ? 0 : 3;
            Log("upscale: mouse scaling %s", tridef ? "off - under TriDef 3D the cursor is already in the game's resolution"
                                                    : "on (window messages and cursor API)");
        } else {
            Log("upscale: mouse scaling mode %d (SRWeave.ini)", m);
        }
        return m;
    }();
    return mode;
}

bool MonitorRectOf(HWND hwnd, RECT* out)
{
    MONITORINFO mi = { sizeof mi };
    HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!m || !GetMonitorInfoW(m, &mi)) return false;
    *out = mi.rcMonitor;
    return true;
}

// The monitor's size in real pixels and the same monitor as the calling thread sees it. They
// differ when Windows DPI-virtualizes the caller (an unaware process on a scaled display).
bool MonitorPixels(HWND hwnd, UINT* pw, UINT* ph, RECT* seen)
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof mi;
    HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!m || !GetMonitorInfoW(m, &mi)) return false;
    if (seen) *seen = mi.rcMonitor;
    DEVMODEW dm = {};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        *pw = dm.dmPelsWidth; *ph = dm.dmPelsHeight;
    } else {
        *pw = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left); *ph = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    return true;
}

// Runs on the window's own thread (posted from the render thread, so nothing can deadlock).
void FitWindow(HWND h)
{
    RECT mr;
    if (!MonitorRectOf(h, &mr) || IsIconic(h)) return;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR want = (st & ~(LONG_PTR)(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER)) | WS_POPUP;
    if (want != st) SetWindowLongPtrW(h, GWL_STYLE, want);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR wantEx = ex & ~(LONG_PTR)(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
    if (wantEx != ex) SetWindowLongPtrW(h, GWL_EXSTYLE, wantEx);
    SetWindowPos(h, HWND_TOP, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    Log("upscale: window %p fitted to the monitor (%ld,%ld %ldx%ld)", (void*)h, mr.left, mr.top, mr.right - mr.left, mr.bottom - mr.top);
}

// The game window's client area as the calling thread sees it: origin on the desktop and size.
// Never the render thread's numbers for another thread - with DPI virtualization they differ.
bool ClientSpace(POINT* org, SIZE* size)
{
    HWND h = g_up.hwnd;
    RECT cr = {};
    POINT o = { 0, 0 };
    if (!h || !GetClientRect(h, &cr) || cr.right <= 0 || cr.bottom <= 0 || !ClientToScreen(h, &o)) return false;
    *org = o;
    size->cx = cr.right; size->cy = cr.bottom;
    return true;
}

// diag_input: what coordinate space the calling thread is in
void DiagSpace(const char* where, HWND h)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto pCtx = (void*(WINAPI*)())GetProcAddress(u32, "GetThreadDpiAwarenessContext");
    auto pWinCtx = (void*(WINAPI*)(HWND))GetProcAddress(u32, "GetWindowDpiAwarenessContext");
    auto pAw = (int(WINAPI*)(void*))GetProcAddress(u32, "GetAwarenessFromDpiAwarenessContext");
    auto pWinDpi = (UINT(WINAPI*)(HWND))GetProcAddress(u32, "GetDpiForWindow");
    auto pSysDpi = (UINT(WINAPI*)())GetProcAddress(u32, "GetDpiForSystem");
    auto pPhys = (BOOL(WINAPI*)(LPPOINT))GetProcAddress(u32, "GetPhysicalCursorPos");
    const int aw = (pCtx && pAw) ? pAw(pCtx()) : -9;
    const int waw = (pWinCtx && pAw) ? pAw(pWinCtx(h)) : -9;
    RECT cr = {}, wr = {}, mr = {};
    GetClientRect(h, &cr); GetWindowRect(h, &wr);
    UINT pw = 0, ph = 0;
    MonitorPixels(h, &pw, &ph, &mr);
    POINT cp = {}, pp = {};
    if (g_origGetCursorPos) g_origGetCursorPos(&cp); else GetCursorPos(&cp);
    if (pPhys) pPhys(&pp);
    Log("diag: space at %s: awareness thread %d window %d (0 unaware, 1 system, 2 per-monitor), dpi window %u system %u, "
        "client %ldx%ld, window %ld,%ld-%ld,%ld, monitor seen %ldx%ld real %ux%u, cursor %ld,%ld physical %ld,%ld",
        where, aw, waw, pWinDpi ? pWinDpi(h) : 0, pSysDpi ? pSysDpi() : 0, cr.right, cr.bottom, wr.left, wr.top, wr.right, wr.bottom,
        mr.right - mr.left, mr.bottom - mr.top, pw, ph, cp.x, cp.y, pp.x, pp.y);
}

// diag_input: the first calls of each kind, then one in 400
bool DiagTick(volatile LONG* n)
{
    if (!g_cfg.diagInput) return false;
    LONG c = InterlockedIncrement(n);
    return c <= 6 || c % 400 == 0;
}

LRESULT CALLBACK UpscaleWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_up.fitMsg && m == g_up.fitMsg) { FitWindow(h); return 0; }
    if (g_cfg.diagInput) {
        static volatile LONG nMove = 0, nBtn = 0, nRaw = 0;
        static volatile LONG nSpace = 0;
        if ((m == WM_LBUTTONDOWN || m == WM_MOUSEMOVE) && InterlockedIncrement(&nSpace) <= 2) {
            Log("diag: mouse message 0x%04x raw lParam %d,%d", m, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            DiagSpace("window procedure", h);
        }
        if (m == WM_MOUSEMOVE) { if ((w & MK_LBUTTON) || DiagTick(&nMove)) Log("diag: WM_MOUSEMOVE %d,%d%s", GET_X_LPARAM(l), GET_Y_LPARAM(l), (w & MK_LBUTTON) ? " (drag)" : ""); }
        else if (m >= WM_LBUTTONDOWN && m <= WM_MBUTTONDBLCLK) { InterlockedIncrement(&nBtn); Log("diag: mouse button msg 0x%04x at %d,%d", m, GET_X_LPARAM(l), GET_Y_LPARAM(l)); }
        else if (m == WM_INPUT) { if (DiagTick(&nRaw)) Log("diag: WM_INPUT"); }
        else if (m == WM_SIZE) Log("diag: WM_SIZE type %u %dx%d", (unsigned)w, LOWORD(l), HIWORD(l));
        else if (m == WM_ACTIVATEAPP || m == WM_ACTIVATE) Log("diag: %s wParam %u", m == WM_ACTIVATE ? "WM_ACTIVATE" : "WM_ACTIVATEAPP", (unsigned)w);
    }
    if (g_up.active && g_up.gw > 0 && (MouseMode() & 1)) {
        POINT org; SIZE cs;
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && ClientSpace(&org, &cs)) {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) { x -= org.x; y -= org.y; }   // these carry screen coordinates
            l = MAKELPARAM((short)(x * g_up.gw / cs.cx), (short)(y * g_up.gh / cs.cy));
        } else if (m == WM_SIZE && w != SIZE_MINIMIZED) {
            l = MAKELPARAM(g_up.gw, g_up.gh);   // the game keeps seeing its own resolution
        }
    }
    return g_up.unicode ? CallWindowProcW(g_up.oldProc, h, m, w, l) : CallWindowProcA(g_up.oldProc, h, m, w, l);
}

BOOL WINAPI Hook_GetCursorPos(LPPOINT p)
{
    BOOL r = g_origGetCursorPos(p);
    static volatile LONG nDiag = 0;
    if (r && p && DiagTick(&nDiag)) Log("diag: GetCursorPos %ld,%ld from %s", p->x, p->y, ModuleOf(_ReturnAddress()).c_str());
    POINT org; SIZE cs;
    if (r && p && g_up.active && g_up.gw > 0 && (MouseMode() & 2) && ClientSpace(&org, &cs)) {
        p->x = (p->x - org.x) * g_up.gw / cs.cx;
        p->y = (p->y - org.y) * g_up.gh / cs.cy;
    }
    return r;
}

BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    static volatile LONG nDiag = 0;
    if (DiagTick(&nDiag)) Log("diag: SetCursorPos %d,%d from %s", x, y, ModuleOf(_ReturnAddress()).c_str());
    POINT org; SIZE cs;
    if (g_up.active && g_up.gw > 0 && g_up.gh > 0 && (MouseMode() & 2) && ClientSpace(&org, &cs)) {
        x = org.x + x * cs.cx / g_up.gw;
        y = org.y + y * cs.cy / g_up.gh;
    }
    return g_origSetCursorPos(x, y);
}

BOOL WINAPI Hook_ClipCursor(const RECT* rc)
{
    static volatile LONG nDiag = 0;
    static volatile LONG nSpace = 0;
    if (g_cfg.diagInput && g_up.active && InterlockedIncrement(&nSpace) <= 1) DiagSpace("game code calling ClipCursor", g_up.hwnd);
    if (DiagTick(&nDiag)) {
        if (rc) Log("diag: ClipCursor %ld,%ld-%ld,%ld from %s", rc->left, rc->top, rc->right, rc->bottom, ModuleOf(_ReturnAddress()).c_str());
        else Log("diag: ClipCursor(null) from %s", ModuleOf(_ReturnAddress()).c_str());
    }
    POINT org; SIZE cs;
    if (rc && g_up.active && g_up.gw > 0 && g_up.gh > 0 && (MouseMode() & 2) && ClientSpace(&org, &cs)) {
        RECT s = { org.x + rc->left * cs.cx / g_up.gw,  org.y + rc->top * cs.cy / g_up.gh,
                   org.x + rc->right * cs.cx / g_up.gw, org.y + rc->bottom * cs.cy / g_up.gh };
        return g_origClipCursor(&s);
    }
    return g_origClipCursor(rc);
}

// diag_input only: who asks for the window size / converts coordinates / reads raw input
BOOL WINAPI Hook_GetClientRect(HWND h, LPRECT r)
{
    BOOL ok = g_origGetClientRect(h, r);
    static volatile LONG n = 0;
    if (ok && r && h == g_up.hwnd && !AddrInModule(_ReturnAddress(), g_self) && DiagTick(&n))
        Log("diag: GetClientRect %ldx%ld from %s", r->right, r->bottom, ModuleOf(_ReturnAddress()).c_str());
    return ok;
}

BOOL WINAPI Hook_GetWindowRect(HWND h, LPRECT r)
{
    BOOL ok = g_origGetWindowRect(h, r);
    static volatile LONG n = 0;
    if (ok && r && h == g_up.hwnd && !AddrInModule(_ReturnAddress(), g_self) && DiagTick(&n))
        Log("diag: GetWindowRect %ld,%ld-%ld,%ld from %s", r->left, r->top, r->right, r->bottom, ModuleOf(_ReturnAddress()).c_str());
    return ok;
}

BOOL WINAPI Hook_ScreenToClient(HWND h, LPPOINT p)
{
    POINT in = p ? *p : POINT{};
    BOOL ok = g_origScreenToClient(h, p);
    static volatile LONG n = 0;
    if (ok && p && DiagTick(&n))
        Log("diag: ScreenToClient(%p) %ld,%ld -> %ld,%ld from %s", (void*)h, in.x, in.y, p->x, p->y, ModuleOf(_ReturnAddress()).c_str());
    return ok;
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT a, UINT b, LPVOID c, PUINT d, UINT e)
{
    static volatile LONG n = 0;
    if (DiagTick(&n)) Log("diag: GetRawInputData from %s", ModuleOf(_ReturnAddress()).c_str());
    return g_origGetRawInputData(a, b, c, d, e);
}

BOOL WINAPI Hook_GetCursorInfo(PCURSORINFO ci)
{
    BOOL ok = g_origGetCursorInfo(ci);
    static volatile LONG n = 0;
    if (ok && ci && !AddrInModule(_ReturnAddress(), g_self) && DiagTick(&n))
        Log("diag: GetCursorInfo %ld,%ld from %s", ci->ptScreenPos.x, ci->ptScreenPos.y, ModuleOf(_ReturnAddress()).c_str());
    return ok;
}

DWORD WINAPI Hook_GetMessagePos(void)
{
    DWORD v = g_origGetMessagePos();
    static volatile LONG n = 0;
    if (DiagTick(&n)) Log("diag: GetMessagePos %d,%d from %s", GET_X_LPARAM(v), GET_Y_LPARAM(v), ModuleOf(_ReturnAddress()).c_str());
    return v;
}

BOOL WINAPI Hook_GetPhysicalCursorPos(LPPOINT p)
{
    BOOL ok = g_origGetPhysicalCursorPos(p);
    static volatile LONG n = 0;
    if (ok && p && DiagTick(&n)) Log("diag: GetPhysicalCursorPos %ld,%ld from %s", p->x, p->y, ModuleOf(_ReturnAddress()).c_str());
    return ok;
}

BOOL WINAPI Hook_ClientToScreen(HWND h, LPPOINT p)
{
    POINT in = p ? *p : POINT{};
    BOOL ok = g_origClientToScreen(h, p);
    static volatile LONG n = 0;
    if (ok && p && !AddrInModule(_ReturnAddress(), g_self) && DiagTick(&n))
        Log("diag: ClientToScreen %ld,%ld -> %ld,%ld from %s", in.x, in.y, p->x, p->y, ModuleOf(_ReturnAddress()).c_str());
    return ok;
}

// The game asked for exclusive fullscreen below the monitor's resolution: make the device
// windowed instead (an additional swap chain only exists on a windowed device). Returns true if
// `p` was changed.
bool ForceWindowed(D3DPRESENT_PARAMETERS& p, HWND focus, const char* where)
{
    if (!g_cfg.upscale) return false;
    g_up.forced = false;
    if (p.Windowed || p.BackBufferWidth == 0 || p.BackBufferHeight == 0) return false;
    HWND hwnd = p.hDeviceWindow ? p.hDeviceWindow : (focus ? focus : g_up.hwnd);
    RECT mr;
    UINT nw = 0, nh = 0;
    if (!hwnd || !MonitorPixels(hwnd, &nw, &nh, &mr)) return false;
    if ((UINT)(mr.right - mr.left) != nw || (UINT)(mr.bottom - mr.top) != nh) {
        // DPI-virtualized: Windows would stretch whatever we present to this window, weave included
        Log("upscale: %s - the game sees the %ux%u monitor as %ldx%ld (DPI scaling of a non-DPI-aware process); not upscaling. "
            "Set the exe's compatibility option \"Override high DPI scaling behavior: Application\".",
            where, nw, nh, mr.right - mr.left, mr.bottom - mr.top);
        return false;
    }
    if (p.BackBufferWidth >= nw && p.BackBufferHeight >= nh) {
        Log("upscale: %s at %ux%u is the monitor's resolution already - nothing to do", where, p.BackBufferWidth, p.BackBufferHeight);
        return false;
    }
    Log("upscale: %s fullscreen %ux%u @%uHz -> windowed (monitor %ux%u)", where, p.BackBufferWidth, p.BackBufferHeight,
        p.FullScreen_RefreshRateInHz, nw, nh);
    p.Windowed = TRUE;
    p.FullScreen_RefreshRateInHz = 0;
    g_up.hwnd = hwnd;
    g_up.forced = true;
    return true;
}

// What the device wrote back into our copy goes to the caller, who still sees "fullscreen".
void CopyBack(D3DPRESENT_PARAMETERS* pp, const D3DPRESENT_PARAMETERS& p)
{
    D3DPRESENT_PARAMETERS r = p;
    r.Windowed = pp->Windowed;
    r.FullScreen_RefreshRateInHz = pp->FullScreen_RefreshRateInHz;
    *pp = r;
}

// Subclass the game window (mouse scaling, window fitting) and keep it over the whole monitor.
void EnsureUpscaleWindow(HWND hwnd, const RECT& mr, UINT gw, UINT gh)
{
    if (!g_up.fitMsg) g_up.fitMsg = RegisterWindowMessageW(L"SRWeaveUpscaleFit");
    if (g_up.procHwnd != hwnd) {
        g_up.unicode = IsWindowUnicode(hwnd) != FALSE;
        g_up.oldProc = (WNDPROC)(g_up.unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)UpscaleWndProc)
                                              : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)UpscaleWndProc));
        g_up.procHwnd = hwnd;
        Log("upscale: window %p subclassed (%s)", (void*)hwnd, g_up.unicode ? "unicode" : "ansi");
    }
    g_up.gw = (int)gw; g_up.gh = (int)gh;
    RECT wr = {};
    if (GetWindowRect(hwnd, &wr) && !EqualRect(&wr, &mr) && !IsIconic(hwnd)) {
        ULONGLONG now = GetTickCount64();
        if (now - g_up.lastFit > 500) { g_up.lastFit = now; PostMessageW(hwnd, g_up.fitMsg, 0, 0); }
    }
}

// diag_input, while the left button is down: mean brightness of the left eye's centre part of
// the frame about to go out, so a wrong frame can be told from a wrongly shown one.
void DiagFrame(IDirect3DDevice9* dev, DevState& s)
{
    if (!g_cfg.diagInput || !s.surf || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
    static IDirect3DSurface9* sys = nullptr;
    static UINT sw = 0, sh = 0;
    static unsigned n = 0;
    static ULONGLONG last = 0;
    if (sys && (sw != s.w || sh != s.h)) { sys->Release(); sys = nullptr; }
    if (!sys && FAILED(dev->CreateOffscreenPlainSurface(s.w, s.h, s.fmt, D3DPOOL_SYSTEMMEM, &sys, nullptr))) return;
    sw = s.w; sh = s.h;
    if (FAILED(dev->GetRenderTargetData(s.surf, sys))) return;
    D3DLOCKED_RECT lr = {};
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    unsigned long long sum = 0; unsigned cnt = 0;
    for (UINT y = s.h * 14 / 100; y < s.h * 92 / 100; y += 8) {
        const BYTE* row = (const BYTE*)lr.pBits + (size_t)y * lr.Pitch;
        for (UINT x = s.w * 12 / 100; x < s.w * 20 / 100; x += 8) { sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]; cnt++; }
    }
    sys->UnlockRect();
    ULONGLONG now = GetTickCount64();
    Log("diag: frame %u lum %.2f dt %llu", ++n, cnt ? sum / (cnt * 3.0) : 0.0, last ? now - last : 0);
    last = now;
}

// Presents the frame through the native-resolution swap chain. false = not applicable (the
// caller then weaves into the game's back buffer as usual).
// `sc` / `dstWnd` are what the game presented: only the implicit swap chain going to the game
// window is the picture on screen. A game also presents other things - Dragon Age renders the
// rotating character-creation model's helper view and presents it to another target - and those
// must not be put on the monitor (they showed up as single frames of a wrong pose).
bool PresentUpscaled(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, HWND dstWnd, IDirect3DSurface9* bb, HRESULT* outHr)
{
    if (!g_cfg.upscale || !g_up.forced) { g_up.active = false; return false; }
    DevState& s = StateOf(dev);
    if (s.upFailed) { g_up.active = false; return false; }

    D3DSURFACE_DESC d = {};
    if (FAILED(bb->GetDesc(&d)) || d.Width < 2) return false;
    D3DPRESENT_PARAMETERS ipp = {};
    IDirect3DSwapChain9* isc = nullptr;
    if (FAILED(dev->GetSwapChain(0, &isc)) || !isc) return false;
    isc->GetPresentParameters(&ipp);
    isc->Release();
    HWND hwnd = g_up.hwnd ? g_up.hwnd : WindowOf(dev);
    if ((sc && sc != isc) || (dstWnd && dstWnd != hwnd)) {
        static volatile LONG nOther = 0;
        LONG c = InterlockedIncrement(&nOther);
        if (c <= 3 || (g_cfg.diagInput && c % 200 == 0))
            Log("upscale: Present of swap chain %p to window %p is not the game picture (implicit %p, window %p) - left alone (%ld)",
                (void*)sc, (void*)dstWnd, (void*)isc, (void*)hwnd, c);
        return false;
    }
    RECT mr;
    if (!ipp.Windowed || !hwnd || !MonitorRectOf(hwnd, &mr)) { g_up.active = false; return false; }
    const UINT nw = (UINT)(mr.right - mr.left), nh = (UINT)(mr.bottom - mr.top);
    if (d.Width >= nw && d.Height >= nh) { g_up.active = false; return false; }
    if (IsIconic(hwnd)) return false;

    if (s.usc && (s.nw != nw || s.nh != nh)) {
        if (s.uscBB) { s.uscBB->Release(); s.uscBB = nullptr; }
        s.usc->Release(); s.usc = nullptr;
    }
    if (!s.usc) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth = nw; pp.BackBufferHeight = nh;
        pp.BackBufferFormat = (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8) ? d.Format : D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.hDeviceWindow = hwnd;
        // The windowed device underneath usually ends up presenting at once, and a game that was
        // locked to the display in exclusive fullscreen then runs at hundreds of frames a second:
        // the weaved picture tears and flickers whenever something moves. Default: wait for the blank.
        pp.PresentationInterval = g_cfg.upscaleVsync > 0 ? D3DPRESENT_INTERVAL_ONE
                                : g_cfg.upscaleVsync == 0 ? D3DPRESENT_INTERVAL_IMMEDIATE : ipp.PresentationInterval;
        HRESULT hr = dev->CreateAdditionalSwapChain(&pp, &s.usc);
        if (SUCCEEDED(hr) && s.usc) hr = s.usc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &s.uscBB);
        if (FAILED(hr) || !s.usc || !s.uscBB) {
            Log("upscale: %ux%u swap chain failed 0x%08lx - weaving at the game's resolution", nw, nh, hr);
            if (s.usc) { s.usc->Release(); s.usc = nullptr; }
            s.upFailed = true;
            g_up.active = false;
            return false;
        }
        s.nw = nw; s.nh = nh;
        Log("upscale: swap chain %ux%u fmt %d interval 0x%x (game's 0x%x, swap effect %d, %u back buffers) for window %p (game renders %ux%u)", nw, nh,
            (int)pp.BackBufferFormat, pp.PresentationInterval, ipp.PresentationInterval, (int)ipp.SwapEffect, ipp.BackBufferCount, (void*)hwnd, d.Width, d.Height);
    }
    EnsureUpscaleWindow(hwnd, mr, d.Width, d.Height);
    g_up.active = true;
    if (g_cfg.diagInput && (s.upFrames == 5 || s.upFrames == 900)) DiagSpace("render thread (Present)", hwnd);

    if (PollHotkeys()) SrSetLens(g_cfg.lens && g_cfg.weave);
    bool weaved = false;
    if (g_cfg.weave && !s.failed && EnsureWeaver(dev, s)) {
        if (PrepareInput(dev, s, bb, d)) {
            DrawWeave(dev, s, s.uscBB, nw, nh);
            weaved = !s.failed;
        } else {
            s.failed = true;
        }
    }
    if (!weaved) {
        // weaving off / not available: the plain SBS picture, stretched to the monitor
        if (EnsureTexture(dev, s, d)) {
            dev->StretchRect(bb, nullptr, s.surf, nullptr, D3DTEXF_POINT);   // resolves MSAA
            dev->StretchRect(s.surf, nullptr, s.uscBB, nullptr, D3DTEXF_LINEAR);
        } else {
            dev->StretchRect(bb, nullptr, s.uscBB, nullptr, D3DTEXF_LINEAR);
        }
    }
    DiagFrame(dev, s);
    *outHr = g_origScPresent ? g_origScPresent(s.usc, nullptr, nullptr, nullptr, nullptr, 0)
                             : s.usc->Present(nullptr, nullptr, nullptr, nullptr, 0);
    if (++s.upFrames == 1) Log("first upscaled frame: %ux%u -> %ux%u (%s)", d.Width, d.Height, nw, nh, weaved ? "weaved" : "not weaved");
    if (weaved && ++s.frames == 1) Log("first weaved frame: %ux%u", nw, nh);
    return true;
}

// true: the frame has been presented here (upscale) and *outHr is the result; the caller must
// not call the original Present.
bool OnPresent(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, HWND dstWnd, HRESULT* outHr)
{
    if (t_inPresent || !dev) return false;
    t_inPresent = true;
    bool presented = false;
    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = sc ? sc->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                    : dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr) && bb) {
        presented = PresentUpscaled(dev, sc, dstWnd, bb, outHr);
        // while upscaling, anything else the game presents is not the screen picture: don't weave it
        if (!presented && !g_up.active) Weave(dev, bb);
        bb->Release();
    }
    t_inPresent = false;
    return presented;
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

// diag_input: Present calls that are not a plain "whole back buffer to the device window"
void DiagPresent(const char* what, void* obj, const RECT* a, const RECT* b, HWND c, DWORD flags)
{
    if (!g_cfg.diagInput || t_inPresent) return;
    static volatile LONG nAll = 0, nOdd = 0;
    LONG all = InterlockedIncrement(&nAll);
    if (a || b || c || flags) {
        LONG n = InterlockedIncrement(&nOdd);
        if (n <= 20 || n % 100 == 0)
            Log("diag: %s(%p) src %ld,%ld-%ld,%ld dst %ld,%ld-%ld,%ld wnd %p flags 0x%lx (odd %ld of %ld)", what, obj,
                a ? a->left : -1, a ? a->top : -1, a ? a->right : -1, a ? a->bottom : -1,
                b ? b->left : -1, b ? b->top : -1, b ? b->right : -1, b ? b->bottom : -1, (void*)c, flags, n, all);
    } else if (all <= 3 || all % 300 == 0) {
        Log("diag: %s(%p) plain (%ld)", what, obj, all);
    }
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d)
{
    DiagPresent("Device::Present", dev, a, b, c, 0);
    HRESULT hr = D3D_OK;
    if (OnPresent(dev, nullptr, c, &hr)) return hr;
    return g_origPresent(dev, a, b, c, d);
}

HRESULT STDMETHODCALLTYPE Hook_ScPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD f)
{
    IDirect3DDevice9* dev = nullptr;
    DiagPresent("SwapChain::Present", sc, a, b, c, f);
    if (SUCCEEDED(sc->GetDevice(&dev)) && dev) {
        HRESULT hr = D3D_OK;
        bool presented = OnPresent(dev, sc, c, &hr);
        dev->Release();
        if (presented) return hr;
    }
    return g_origScPresent(sc, a, b, c, d, f);
}

HRESULT STDMETHODCALLTYPE Hook_PresentEx(IDirect3DDevice9Ex* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD f)
{
    HRESULT hr = D3D_OK;
    if (OnPresent(dev, nullptr, c, &hr)) return hr;
    return g_origPresentEx(dev, a, b, c, d, f);
}

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    OnBeforeReset(dev);
    D3DPRESENT_PARAMETERS p = {};
    bool forced = false;
    if (pp) { p = *pp; forced = ForceWindowed(p, WindowOf(dev), "Reset"); }
    HRESULT hr = g_origReset(dev, forced ? &p : pp);
    if (forced) {
        if (SUCCEEDED(hr)) {
            CopyBack(pp, p);
        } else {
            Log("upscale: windowed Reset failed 0x%08lx - retrying as the game asked", hr);
            g_up.forced = false;
            hr = g_origReset(dev, pp);
        }
    }
    OnAfterReset(dev, hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                            D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
    D3DPRESENT_PARAMETERS p = {};
    bool forced = false;
    if (pp) { p = *pp; forced = ForceWindowed(p, focus, "CreateDevice"); }
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, focus, flags, forced ? &p : pp, out);
    if (forced) {
        if (SUCCEEDED(hr)) {
            CopyBack(pp, p);
        } else {
            Log("upscale: windowed CreateDevice failed 0x%08lx - retrying as the game asked", hr);
            g_up.forced = false;
            hr = g_origCreateDevice(d3d, adapter, type, focus, flags, pp, out);
        }
    }
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
        if (g_cfg.upscale && d3d) {
            // IDirect3D9::CreateDevice is slot 16, the last one. The object is the real one or a
            // TriDef wrapper around it.
            void** d3dVt = UnwrapVtable(d3d, g_d3d9, 16);
            void* create = d3dVt ? d3dVt[16] : ResolveReal("IDirect3D9::CreateDevice", *(void***)d3d, 16, 16);
            if (create && AddrInModule(create, g_d3d9))
                HookOne("IDirect3D9::CreateDevice", create, (void*)&Hook_CreateDevice, (void**)&g_origCreateDevice);
            else
                Log("upscale: IDirect3D9::CreateDevice not resolved into d3d9.dll - only Reset can switch the device");
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            if (u32 && ((MouseMode() & 2) || g_cfg.diagInput)) {
                HookOne("GetCursorPos", (void*)GetProcAddress(u32, "GetCursorPos"), (void*)&Hook_GetCursorPos, (void**)&g_origGetCursorPos);
                HookOne("SetCursorPos", (void*)GetProcAddress(u32, "SetCursorPos"), (void*)&Hook_SetCursorPos, (void**)&g_origSetCursorPos);
                HookOne("ClipCursor", (void*)GetProcAddress(u32, "ClipCursor"), (void*)&Hook_ClipCursor, (void**)&g_origClipCursor);
                if (g_cfg.diagInput) {
                    HookOne("GetClientRect", (void*)GetProcAddress(u32, "GetClientRect"), (void*)&Hook_GetClientRect, (void**)&g_origGetClientRect);
                    HookOne("GetWindowRect", (void*)GetProcAddress(u32, "GetWindowRect"), (void*)&Hook_GetWindowRect, (void**)&g_origGetWindowRect);
                    HookOne("ScreenToClient", (void*)GetProcAddress(u32, "ScreenToClient"), (void*)&Hook_ScreenToClient, (void**)&g_origScreenToClient);
                    HookOne("GetRawInputData", (void*)GetProcAddress(u32, "GetRawInputData"), (void*)&Hook_GetRawInputData, (void**)&g_origGetRawInputData);
                    HookOne("GetCursorInfo", (void*)GetProcAddress(u32, "GetCursorInfo"), (void*)&Hook_GetCursorInfo, (void**)&g_origGetCursorInfo);
                    HookOne("GetMessagePos", (void*)GetProcAddress(u32, "GetMessagePos"), (void*)&Hook_GetMessagePos, (void**)&g_origGetMessagePos);
                    HookOne("GetPhysicalCursorPos", (void*)GetProcAddress(u32, "GetPhysicalCursorPos"), (void*)&Hook_GetPhysicalCursorPos, (void**)&g_origGetPhysicalCursorPos);
                    HookOne("ClientToScreen", (void*)GetProcAddress(u32, "ClientToScreen"), (void*)&Hook_ClientToScreen, (void**)&g_origClientToScreen);
                }
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
        if (g_cfg.upscale && g_cfg.upscaleDpi) MakeDpiAware();
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
