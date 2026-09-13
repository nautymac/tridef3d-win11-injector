// Custom depth-based stereo (Half-SBS) D3D9 hook - the D3D9 analogue of
// hook.cpp (which does the same thing for D3D11/DXGI). Written from
// scratch on public, documented Win32/D3D9 APIs; no TriDef code involved.
// See the long investigation in this project's history for why TriDef's
// own D3D9 activation entry point (TriDef3DSDKFunc) can't be called
// reliably from external code - this file replaces that dependency
// entirely with our own, fully-controlled pipeline.
//
// Technique:
//  1. Hook IDirect3D9::CreateDevice (shared-vtable trick: create our own
//     throwaway IDirect3D9 via Direct3DCreate9, patch its vtable - every
//     IDirect3D9 instance from the same d3d9.dll shares one vtable, so
//     this affects the real device creation call too).
//  2. Before forwarding to the real CreateDevice, if the caller asked for
//     an auto depth-stencil buffer, force its format to D3DFMT_INTZ - a
//     de-facto-standard (AMD-originated, now universally supported)
//     FourCC that makes the depth buffer readable as a regular texture
//     in a shader. This is the same trick ReShade and countless D3D9
//     mods have used for depth-buffer access for over a decade.
//  3. Once the real IDirect3DDevice9 exists, hook ITS OWN vtable's
//     EndScene (called once per frame, right after the game finishes
//     rendering and right before Present) to run our stereo composite.
//  4. In the EndScene hook: copy the back buffer to a readable texture,
//     grab the (now INTZ, therefore texture-readable) depth-stencil
//     surface, and draw a full-screen quad with a pixel shader that does
//     depth-weighted horizontal reprojection into Half-SBS, writing the
//     result directly into the real back buffer.

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "d3dcompiler.lib")

#define D3DFMT_INTZ ((D3DFORMAT)(MAKEFOURCC('I','N','T','Z')))

// ---------------------------------------------------------------- logging --
static void Log(const char* msg) {
    OutputDebugStringA(msg);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\nauty\\tridef_d3d9_hook_log.txt", "a") == 0 && f) {
        fputs(msg, f);
        fputs("\n", f);
        fclose(f);
    }
}
static void LogF(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);
    Log(buf);
}

// ----------------------------------------------------------- config file --
// Persists separation/convergence (set via the Ctrl+F3..F6 hotkeys) across
// game restarts. Stored next to this DLL so it stays with the tool rather
// than any one game install.
static HMODULE g_hModule = nullptr;
static void GetConfigPath(char* outPath, size_t outSize) {
    char dllPath[MAX_PATH] = {0};
    GetModuleFileNameA(g_hModule, dllPath, MAX_PATH);
    char* lastSlash = strrchr(dllPath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0'; else dllPath[0] = '\0';
    sprintf_s(outPath, outSize, "%stridef_d3d9_hook_config.ini", dllPath);
}

// -------------------------------------------------------------- vtables ---
typedef HRESULT (__stdcall *CreateDevice_t)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (__stdcall *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT (__stdcall *Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (__stdcall *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static CreateDevice_t oCreateDevice = nullptr;
static EndScene_t oEndScene = nullptr;
static Present_t oPresent = nullptr;
static Reset_t oReset = nullptr;
static bool g_deviceHookInstalled = false;

// ------------------------------------------------------------- pipeline ---
static IDirect3DPixelShader9* g_ps = nullptr;
static IDirect3DVertexBuffer9* g_quadVB = nullptr;
static IDirect3DVertexBuffer9* g_cursorVB = nullptr;
static IDirect3DTexture9* g_colorCopyTex = nullptr;
static UINT g_colorW = 0, g_colorH = 0;
static D3DFORMAT g_colorFmt = D3DFMT_UNKNOWN;
static bool g_pipelineReady = false;
static float g_shiftStrength = 0.03f;   // separation: max disparity, in UV units
static float g_reverseZ = 0.0f;
static float g_convergence = 1.0f;      // depth value (0=near,1=far) that gets zero disparity
static HWND g_hwnd = nullptr;           // captured from the real device, used to map cursor pos
static WNDPROC g_origWndProc = nullptr;
static bool g_systemCursorHidden = false;

// Both Win32 ShowCursor()/IDirect3DDevice9::ShowCursor() and a WM_SETCURSOR
// window-subclass hook (below) failed to hide this game's cursor - it kept
// showing the plain Windows arrow regardless, meaning the game re-asserts
// it through some path those don't intercept. The one technique that
// bypasses ALL app-side cursor-selection logic at once: replace the shared
// SYSTEM cursor resource itself. Every SetCursor(LoadCursor(NULL,
// IDC_ARROW)) call anywhere - regardless of thread, window, or timing -
// resolves through this shared resource, so swapping it for a blank/
// transparent cursor hides the arrow no matter how or when the game sets
// it. This is process-wide/OS-wide until restored, so it must be undone on
// exit (DLL_PROCESS_DETACH) or every app on the desktop loses its cursor.
static void HideSystemCursorGlobally() {
    if (g_systemCursorHidden) return;
    BYTE andMask[32]; memset(andMask, 0xFF, sizeof(andMask)); // AND=1 everywhere -> fully transparent
    BYTE xorMask[32] = {0};
    HCURSOR blank = CreateCursor(GetModuleHandle(nullptr), 0, 0, 32, 32, andMask, xorMask);
    if (!blank) { LogF("HideSystemCursorGlobally: CreateCursor failed, err=%lu", GetLastError()); return; }
    // SetSystemCursor takes ownership of the handle (destroys it internally)
    // - must not call DestroyCursor on it ourselves. 32512 = OCR_NORMAL.
    if (SetSystemCursor(blank, 32512)) {
        g_systemCursorHidden = true;
        Log("HideSystemCursorGlobally: OCR_NORMAL replaced with blank cursor");
    } else {
        LogF("HideSystemCursorGlobally: SetSystemCursor failed, err=%lu", GetLastError());
    }
}

static void RestoreSystemCursor() {
    if (!g_systemCursorHidden) return;
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    g_systemCursorHidden = false;
    Log("RestoreSystemCursor: system cursors restored to default");
}

// Win32 ShowCursor() and IDirect3DDevice9::ShowCursor() are both no-ops
// here (confirmed by testing) - most likely because the game manages its
// cursor from a different thread than the one calling Present(), and
// Windows' displayed cursor is tied to whichever thread's message loop is
// currently servicing WM_SETCURSOR for the window under the pointer, not
// to whichever thread merely calls ShowCursor(). Subclassing the window
// and forcing SetCursor(NULL) on every WM_SETCURSOR sidesteps that: it
// intercepts cursor selection at the point the OS actually decides it,
// regardless of which thread owns rendering.
static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETCURSOR) {
        SetCursor(nullptr);
        return TRUE;
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam);
}

static void SaveConfig() {
    char path[MAX_PATH];
    GetConfigPath(path, sizeof(path));
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") == 0 && f) {
        fprintf(f, "separation=%.4f\nconvergence=%.4f\n", g_shiftStrength, g_convergence);
        fclose(f);
    }
}

static void LoadConfig() {
    char path[MAX_PATH];
    GetConfigPath(path, sizeof(path));
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        float v;
        if (sscanf_s(line, "separation=%f", &v) == 1) g_shiftStrength = v;
        else if (sscanf_s(line, "convergence=%f", &v) == 1) g_convergence = v;
    }
    fclose(f);
    LogF("LoadConfig: separation=%.4f convergence=%.4f", g_shiftStrength, g_convergence);
}

struct ColorVertex { float x, y, z, rhw; DWORD color; };
#define CURSOR_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// We swap the device's active depth-stencil surface for one backed by a
// texture we created ourselves (D3DFMT_INTZ), so we always have a live
// IDirect3DTexture9* to sample from - no GetContainer() needed, which
// only works for surfaces that were already texture-backed to begin with
// (auto depth-stencil buffers usually aren't).
static IDirect3DTexture9* g_depthTex = nullptr;
static IDirect3DSurface9* g_depthSurf = nullptr;
static UINT g_depthW = 0, g_depthH = 0;

struct QuadVertex { float x, y, z, rhw; float u, v; };
#define QUAD_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static const char* kPixelShaderSrc = R"(
sampler2D colorTex : register(s0);
sampler2D depthTex : register(s1);

// x = shiftStrength (separation), y = reverseZ, z = convergence, w unused.
// Packed into one float4/register so the C++ side only needs a single
// SetPixelShaderConstantF call instead of one per scalar.
float4 params : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float shiftStrength = params.x;
    float reverseZ = params.y;
    float convergence = params.z;

    bool leftHalf = uv.x < 0.5;
    float2 srcUV = leftHalf ? float2(uv.x * 2.0, uv.y) : float2((uv.x - 0.5) * 2.0, uv.y);

    float depth = tex2D(depthTex, srcUV).r;
    // Defend against uninitialized/garbage depth values (e.g. UI-only
    // frames that never write the depth buffer, right after the texture
    // is (re)created): NaN != NaN, so this also catches NaN bit patterns
    // that saturate() alone would not reliably clamp. Treat anything
    // invalid as "far" (1.0), which yields zero disparity - i.e. no
    // shift, which is the safe/neutral fallback.
    if (!(depth >= 0.0 && depth <= 1.0)) depth = 1.0;
    if (reverseZ > 0.5) depth = 1.0 - depth;

    // convergence is the depth plane that gets zero disparity (appears at
    // screen distance); nearer surfaces shift one way (pop out), farther
    // surfaces shift the other way (recede into the screen).
    float disparity = shiftStrength * (convergence - depth);
    float2 shiftedUV = leftHalf ? srcUV + float2(disparity, 0) : srcUV - float2(disparity, 0);
    shiftedUV = saturate(shiftedUV);

    return tex2D(colorTex, shiftedUV);
}
)";

static bool EnsureDepthTarget(IDirect3DDevice9* dev, UINT width, UINT height) {
    if (g_depthTex && g_depthW == width && g_depthH == height) {
        return true;
    }
    if (g_depthSurf) { g_depthSurf->Release(); g_depthSurf = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }

    HRESULT hr = dev->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_INTZ,
                                     D3DPOOL_DEFAULT, &g_depthTex, nullptr);
    if (FAILED(hr)) {
        LogF("EnsureDepthTarget: CreateTexture(INTZ) failed hr=0x%08X - GPU may not support INTZ", (unsigned)hr);
        return false;
    }
    g_depthTex->GetSurfaceLevel(0, &g_depthSurf);

    HRESULT hr2 = dev->SetDepthStencilSurface(g_depthSurf);
    if (FAILED(hr2)) {
        LogF("EnsureDepthTarget: SetDepthStencilSurface failed hr=0x%08X", (unsigned)hr2);
        g_depthSurf->Release(); g_depthSurf = nullptr;
        g_depthTex->Release(); g_depthTex = nullptr;
        return false;
    }
    g_depthW = width; g_depthH = height;
    // Freshly-allocated D3DPOOL_DEFAULT memory is not guaranteed to be
    // zeroed, and a UI-only frame (no 3D geometry, Z-test/write usually
    // off) may never write real depth values before our EndScene hook
    // reads this texture back. Force a deterministic "far" baseline so
    // the very first read after (re)creation is never raw GPU garbage.
    dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    LogF("depth target (re)created %ux%u (INTZ) and installed as active depth-stencil", width, height);
    return true;
}

static bool CreatePipeline(IDirect3DDevice9* dev) {
    ID3DBlob* psBlob = nullptr, * errBlob = nullptr;
    HRESULT hr = D3DCompile(kPixelShaderSrc, strlen(kPixelShaderSrc), nullptr, nullptr, nullptr,
                             "main", "ps_3_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) LogF("PS compile error: %s", (char*)errBlob->GetBufferPointer());
        return false;
    }
    hr = dev->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &g_ps);
    psBlob->Release();
    if (FAILED(hr)) { LogF("CreatePixelShader failed hr=0x%08X", (unsigned)hr); return false; }

    hr = dev->CreateVertexBuffer(4 * sizeof(QuadVertex), D3DUSAGE_WRITEONLY, QUAD_FVF,
                                  D3DPOOL_DEFAULT, &g_quadVB, nullptr);
    if (FAILED(hr)) { LogF("CreateVertexBuffer failed hr=0x%08X", (unsigned)hr); return false; }

    // Dynamic (re-locked every frame) small quad used to draw the cursor
    // marker - its contents change every frame (cursor moves), so it's
    // D3DUSAGE_DYNAMIC rather than the static WRITEONLY quad VB above.
    hr = dev->CreateVertexBuffer(4 * sizeof(ColorVertex), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC,
                                  CURSOR_FVF, D3DPOOL_DEFAULT, &g_cursorVB, nullptr);
    if (FAILED(hr)) { LogF("CreateVertexBuffer(cursor) failed hr=0x%08X", (unsigned)hr); return false; }

    Log("D3D9 stereo pipeline created (pixel shader + quad VB + cursor VB OK)");
    return true;
}

static void DrawCursorMarker(IDirect3DDevice9* dev, float cx, float cy, float halfSize, D3DCOLOR color) {
    ColorVertex verts[4] = {
        { cx - halfSize, cy - halfSize, 0, 1, color },
        { cx + halfSize, cy - halfSize, 0, 1, color },
        { cx - halfSize, cy + halfSize, 0, 1, color },
        { cx + halfSize, cy + halfSize, 0, 1, color },
    };
    void* p = nullptr;
    if (SUCCEEDED(g_cursorVB->Lock(0, sizeof(verts), &p, D3DLOCK_DISCARD))) {
        memcpy(p, verts, sizeof(verts));
        g_cursorVB->Unlock();
        dev->SetFVF(CURSOR_FVF);
        dev->SetStreamSource(0, g_cursorVB, 0, sizeof(ColorVertex));
        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    }
}

static void FillQuad(UINT width, UINT height) {
    // D3D9 pretransformed (XYZRHW) full-screen quad, with the classic
    // half-pixel offset so texel centers line up with pixel centers.
    QuadVertex verts[4] = {
        { -0.5f,           -0.5f,            0, 1, 0, 0 },
        { (float)width-0.5f,-0.5f,           0, 1, 1, 0 },
        { -0.5f,           (float)height-0.5f,0, 1, 0, 1 },
        { (float)width-0.5f,(float)height-0.5f,0, 1, 1, 1 },
    };
    void* p = nullptr;
    if (SUCCEEDED(g_quadVB->Lock(0, sizeof(verts), &p, 0))) {
        memcpy(p, verts, sizeof(verts));
        g_quadVB->Unlock();
    }
}

static void EnsureColorCopy(IDirect3DDevice9* dev, IDirect3DSurface9* backBuffer) {
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    if (g_colorCopyTex && g_colorW == desc.Width && g_colorH == desc.Height && g_colorFmt == desc.Format) {
        return;
    }
    if (g_colorCopyTex) { g_colorCopyTex->Release(); g_colorCopyTex = nullptr; }

    HRESULT hr = dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
                                     desc.Format, D3DPOOL_DEFAULT, &g_colorCopyTex, nullptr);
    if (FAILED(hr)) {
        LogF("EnsureColorCopy: CreateTexture failed hr=0x%08X", (unsigned)hr);
        return;
    }
    g_colorW = desc.Width; g_colorH = desc.Height; g_colorFmt = desc.Format;
    FillQuad(desc.Width, desc.Height);
    LogF("color copy texture (re)created %ux%u fmt=%d", g_colorW, g_colorH, (int)g_colorFmt);
}

// -------------------------------------------------------------- compose ---
// Runs the actual Half-SBS composite. Called from HookPresent (see below)
// rather than HookEndScene: EndScene can fire more than once per displayed
// frame in some engines (an offscreen-RT pass, or - as observed in The Cave
// - a separate final pass that draws the game's own cursor sprite directly
// onto the backbuffer). Compositing in EndScene meant whichever of those
// passes ran AFTER ours (e.g. the cursor draw) landed on the backbuffer
// un-split, at its original single/"real" position. Present() is
// guaranteed to fire exactly once per displayed frame, after ALL of a
// frame's rendering (including any such late pass) has already happened,
// so doing our capture+composite there instead sees the final, complete
// frame content and there's nothing left afterward to silently bypass us.
static int g_diagFrameCount = 0; // reset to 0 after every Reset() too
static void DoComposite(IDirect3DDevice9* dev) {
    bool diag = g_diagFrameCount < 5;
    if (diag) g_diagFrameCount++;

    if (!g_pipelineReady) {
        g_pipelineReady = CreatePipeline(dev);
    }

    if (diag) LogF("diag frame %d: pipelineReady=%d", g_diagFrameCount, (int)g_pipelineReady);

    if (g_pipelineReady) {
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hrGetRT = dev->GetRenderTarget(0, &backBuffer);
        if (diag) LogF("diag: GetRenderTarget hr=0x%08X backBuffer=%p", (unsigned)hrGetRT, (void*)backBuffer);
        if (SUCCEEDED(hrGetRT)) {
            D3DSURFACE_DESC bbDesc;
            backBuffer->GetDesc(&bbDesc);
            if (diag) LogF("diag: backbuffer %ux%u fmt=%d", bbDesc.Width, bbDesc.Height, (int)bbDesc.Format);
            EnsureColorCopy(dev, backBuffer);
            bool haveDepth = EnsureDepthTarget(dev, bbDesc.Width, bbDesc.Height);
            if (diag) LogF("diag: colorCopyTex=%p haveDepth=%d", (void*)g_colorCopyTex, (int)haveDepth);

            if (g_colorCopyTex && haveDepth) {
                IDirect3DSurface9* colorCopySurf = nullptr;
                g_colorCopyTex->GetSurfaceLevel(0, &colorCopySurf);
                HRESULT hrStretch = dev->StretchRect(backBuffer, nullptr, colorCopySurf, nullptr, D3DTEXF_NONE);
                if (diag) LogF("diag: StretchRect hr=0x%08X", (unsigned)hrStretch);
                colorCopySurf->Release();

                // Save state we're about to touch.
                DWORD prevFVF = 0; dev->GetFVF(&prevFVF);
                IDirect3DBaseTexture9* prevTex0 = nullptr; dev->GetTexture(0, &prevTex0);
                IDirect3DBaseTexture9* prevTex1 = nullptr; dev->GetTexture(1, &prevTex1);
                IDirect3DPixelShader9* prevPS = nullptr; dev->GetPixelShader(&prevPS);
                IDirect3DVertexShader9* prevVS = nullptr; dev->GetVertexShader(&prevVS);
                DWORD prevZEnable = 0; dev->GetRenderState(D3DRS_ZENABLE, &prevZEnable);
                DWORD prevAlphaBlend = 0; dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &prevAlphaBlend);
                DWORD prevCullMode = 0; dev->GetRenderState(D3DRS_CULLMODE, &prevCullMode);
                DWORD prevScissor = 0; dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &prevScissor);
                DWORD prevStencil = 0; dev->GetRenderState(D3DRS_STENCILENABLE, &prevStencil);
                DWORD prevAlphaTest = 0; dev->GetRenderState(D3DRS_ALPHATESTENABLE, &prevAlphaTest);
                DWORD prevColorWrite = 0; dev->GetRenderState(D3DRS_COLORWRITEENABLE, &prevColorWrite);

                dev->SetRenderTarget(0, backBuffer);
                // A vertex shader left bound from the game's own last draw
                // call overrides FVF entirely (D3D9 ignores FVF whenever a
                // vertex shader is set), so our pretransformed quad would
                // get run through whatever arbitrary shader was last active
                // and typically end up clipped/invisible - DrawPrimitive
                // still reports success either way. Must force fixed
                // function here.
                dev->SetVertexShader(nullptr);
                dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE,
                    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                    D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
                dev->SetFVF(QUAD_FVF);
                dev->SetStreamSource(0, g_quadVB, 0, sizeof(QuadVertex));
                dev->SetTexture(0, g_colorCopyTex);
                dev->SetTexture(1, g_depthTex);
                dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                dev->SetPixelShader(g_ps);
                float consts[4] = { g_shiftStrength, g_reverseZ, g_convergence, 0 };
                dev->SetPixelShaderConstantF(0, consts, 1);

                HRESULT hrDraw = dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
                if (diag) LogF("diag: DrawPrimitive hr=0x%08X", (unsigned)hrDraw);

                // The OS/hardware cursor is composited outside the D3D9
                // present chain entirely, so it only ever appears once, at
                // its real (unsplit) position - it doesn't know our
                // backbuffer is now Half-SBS. Hide it and draw our own
                // marker into BOTH halves at the squished-and-shifted
                // position matching how everything else in the frame was
                // remapped, so the cursor is visible/usable in each eye.
                // Exclusive-fullscreen D3D9 apps commonly draw their cursor
                // via the DEVICE's own cursor API (IDirect3DDevice9::
                // ShowCursor/SetCursorProperties) rather than the Win32
                // hardware cursor, since the OS cursor plane is unreliable
                // in exclusive fullscreen - that's a completely separate
                // on/off switch from the Win32 ShowCursor() below, so both
                // must be called to cover either case.
                ShowCursor(FALSE);
                dev->ShowCursor(FALSE);
                if (g_hwnd) {
                    POINT pt;
                    if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) &&
                        pt.x >= 0 && pt.x <= (LONG)bbDesc.Width &&
                        pt.y >= 0 && pt.y <= (LONG)bbDesc.Height) {
                        float halfW = (float)bbDesc.Width / 2.0f;
                        float nx = (float)pt.x / (float)bbDesc.Width;
                        float leftX = nx * halfW;
                        float rightX = halfW + nx * halfW;
                        float cy = (float)pt.y;

                        dev->SetPixelShader(nullptr);
                        dev->SetVertexShader(nullptr);
                        dev->SetTexture(0, nullptr);
                        dev->SetTexture(1, nullptr);
                        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                        // black outline, then white fill, for visibility on any background
                        DrawCursorMarker(dev, leftX, cy, 7.0f, D3DCOLOR_ARGB(255, 0, 0, 0));
                        DrawCursorMarker(dev, leftX, cy, 4.0f, D3DCOLOR_ARGB(255, 255, 255, 255));
                        DrawCursorMarker(dev, rightX, cy, 7.0f, D3DCOLOR_ARGB(255, 0, 0, 0));
                        DrawCursorMarker(dev, rightX, cy, 4.0f, D3DCOLOR_ARGB(255, 255, 255, 255));
                    }
                }

                // Restore.
                dev->SetPixelShader(prevPS); if (prevPS) prevPS->Release();
                dev->SetVertexShader(prevVS); if (prevVS) prevVS->Release();
                dev->SetTexture(0, prevTex0); if (prevTex0) prevTex0->Release();
                dev->SetTexture(1, prevTex1); if (prevTex1) prevTex1->Release();
                dev->SetFVF(prevFVF);
                dev->SetRenderState(D3DRS_ZENABLE, prevZEnable);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, prevAlphaBlend);
                dev->SetRenderState(D3DRS_CULLMODE, prevCullMode);
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, prevScissor);
                dev->SetRenderState(D3DRS_STENCILENABLE, prevStencil);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, prevAlphaTest);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, prevColorWrite);
            }
            backBuffer->Release();
        }
    }
}

// ------------------------------------------------------------- EndScene ---
static HRESULT __stdcall HookEndScene(IDirect3DDevice9* dev) {
    static bool loggedOnce = false;
    if (!loggedOnce) { Log("HookEndScene: first call"); loggedOnce = true; }
    return oEndScene(dev);
}

// ------------------------------------------------------------- Present ----
static HRESULT __stdcall HookPresent(IDirect3DDevice9* dev, const RECT* pSrc, const RECT* pDest,
                                      HWND hDest, const RGNDATA* pDirty) {
    // Our composite draws need an open scene (DrawPrimitive requires being
    // between BeginScene/EndScene); the game has already called its own
    // EndScene by the time Present() runs, so we bracket our own pass with
    // a fresh BeginScene/EndScene pair here. This is safe to nest after the
    // game's own scene is closed and before Present actually flips.
    HRESULT hrBegin = dev->BeginScene();
    DoComposite(dev);
    if (SUCCEEDED(hrBegin)) dev->EndScene();
    return oPresent(dev, pSrc, pDest, hDest, pDirty);
}

// -------------------------------------------------------------- Reset -----
// Every resource we allocate (g_depthTex/g_depthSurf, g_colorCopyTex,
// g_quadVB) is D3DPOOL_DEFAULT, and we additionally leave g_depthSurf
// bound as the device's active depth-stencil surface via
// SetDepthStencilSurface(). D3D9 requires ALL D3DPOOL_DEFAULT resources -
// especially ones currently bound as a render target or depth-stencil
// surface - to be released/unbound before Reset() is called, or Reset()
// fails with D3DERR_INVALIDCALL and the device stays lost. If that
// happens, every subsequent EndScene-routed draw (menus, HUD, loading
// screens - anything going through the normal D3D9 pipeline) silently
// stops rendering, while content that bypasses the device entirely (e.g.
// intro movies blitted straight to the backbuffer) keeps working - which
// looks exactly like "only the video plays, no UI at all".
static void ReleaseOurResources(IDirect3DDevice9* dev) {
    if (dev) dev->SetDepthStencilSurface(nullptr);
    if (g_depthSurf) { g_depthSurf->Release(); g_depthSurf = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
    if (g_colorCopyTex) { g_colorCopyTex->Release(); g_colorCopyTex = nullptr; }
    if (g_quadVB) { g_quadVB->Release(); g_quadVB = nullptr; }
    if (g_cursorVB) { g_cursorVB->Release(); g_cursorVB = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    g_depthW = g_depthH = 0;
    g_colorW = g_colorH = 0;
    g_colorFmt = D3DFMT_UNKNOWN;
    g_pipelineReady = false;
}

static HRESULT __stdcall HookReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pParams) {
    Log("HookReset: releasing our D3DPOOL_DEFAULT resources before real Reset()");
    ReleaseOurResources(dev);
    HRESULT hr = oReset(dev, pParams);
    LogF("HookReset: real Reset() returned hr=0x%08X", (unsigned)hr);
    g_diagFrameCount = 0;
    // Our resources are lazily recreated on the next EndScene call via
    // EnsureDepthTarget/EnsureColorCopy/CreatePipeline - nothing else to
    // do here even on success.
    return hr;
}

// ----------------------------------------------------------- CreateDevice -
static HRESULT __stdcall HookCreateDevice(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface) {
    Log("HookCreateDevice called");

    // We don't need to touch the game's own depth-stencil setup at all:
    // EnsureDepthTarget() (called from the EndScene hook) creates our own
    // INTZ-format depth texture and swaps it in as the active
    // depth-stencil surface via SetDepthStencilSurface, which is enough
    // to redirect all subsequent depth writes into something we can read
    // back as a texture - regardless of whatever format/mode the game
    // itself asked CreateDevice for.
    HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                pPresentationParameters, ppReturnedDeviceInterface);

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface && !g_deviceHookInstalled) {
        // hFocusWindow (from GetCreationParameters) is the window used for
        // input/activation handling, but some games render into a
        // *different* window given as hDeviceWindow in
        // pPresentationParameters - and it's hDeviceWindow that actually
        // displays the frame and is the one the OS asks WM_SETCURSOR about.
        // Prefer hDeviceWindow when the game supplied one.
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED((*ppReturnedDeviceInterface)->GetCreationParameters(&cp))) {
            g_hwnd = (pPresentationParameters && pPresentationParameters->hDeviceWindow)
                         ? pPresentationParameters->hDeviceWindow
                         : cp.hFocusWindow;
            LogF("hFocusWindow=%p hDeviceWindow=%p -> using %p",
                 (void*)cp.hFocusWindow,
                 (void*)(pPresentationParameters ? pPresentationParameters->hDeviceWindow : nullptr),
                 (void*)g_hwnd);
            if (g_hwnd && !g_origWndProc) {
                SetLastError(0);
                LONG_PTR prev = SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc));
                if (prev == 0 && GetLastError() != 0) {
                    LogF("subclass FAILED for window %p, GetLastError=%lu", (void*)g_hwnd, GetLastError());
                } else {
                    g_origWndProc = reinterpret_cast<WNDPROC>(prev);
                    LogF("subclassed window %p for cursor suppression, orig wndproc=%p",
                         (void*)g_hwnd, (void*)g_origWndProc);
                }
            }
        }

        void** vtbl = *reinterpret_cast<void***>(*ppReturnedDeviceInterface);
        DWORD oldProtect;

        VirtualProtect(&vtbl[42], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect); // EndScene = slot 42
        oEndScene = reinterpret_cast<EndScene_t>(vtbl[42]);
        vtbl[42] = reinterpret_cast<void*>(HookEndScene);
        VirtualProtect(&vtbl[42], sizeof(void*), oldProtect, &oldProtect);

        VirtualProtect(&vtbl[17], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect); // Present = slot 17
        oPresent = reinterpret_cast<Present_t>(vtbl[17]);
        vtbl[17] = reinterpret_cast<void*>(HookPresent);
        VirtualProtect(&vtbl[17], sizeof(void*), oldProtect, &oldProtect);

        VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect); // Reset = slot 16
        oReset = reinterpret_cast<Reset_t>(vtbl[16]);
        vtbl[16] = reinterpret_cast<void*>(HookReset);
        VirtualProtect(&vtbl[16], sizeof(void*), oldProtect, &oldProtect);

        g_deviceHookInstalled = true;
        LogF("real device @ %p created, EndScene+Present+Reset vtable patched", (void*)*ppReturnedDeviceInterface);

        HideSystemCursorGlobally();
    }

    return hr;
}

// --------------------------------------------------------- hook install ---
static void** GetVTable(void* pInstance) { return *reinterpret_cast<void***>(pInstance); }

static bool InstallHook() {
    Log("InstallHook: creating dummy IDirect3D9 to read vtable");

    typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);
    HMODULE hD3D9 = LoadLibraryA("d3d9.dll");
    if (!hD3D9) { Log("LoadLibrary d3d9.dll failed"); return false; }
    auto pCreate9 = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(hD3D9, "Direct3DCreate9"));
    if (!pCreate9) { Log("GetProcAddress Direct3DCreate9 failed"); return false; }

    IDirect3D9* d3d9 = pCreate9(D3D_SDK_VERSION);
    if (!d3d9) { Log("Direct3DCreate9 failed"); return false; }
    Log("dummy IDirect3D9 created OK");

    void** vtbl = GetVTable(d3d9);
    DWORD oldProtect;
    VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect); // CreateDevice = slot 16
    oCreateDevice = reinterpret_cast<CreateDevice_t>(vtbl[16]);
    vtbl[16] = reinterpret_cast<void*>(HookCreateDevice);
    VirtualProtect(&vtbl[16], sizeof(void*), oldProtect, &oldProtect);
    Log("IDirect3D9::CreateDevice vtable patched");

    d3d9->Release();
    return true;
}

// ------------------------------------------------------------ hotkeys -----
// Runtime tuning without recompiling, using the same key scheme as TriDef's
// own Ignition driver: Ctrl+F3/Ctrl+F4 step separation (shiftStrength)
// down/up, Ctrl+F5/Ctrl+F6 step convergence down/up. Edge-triggered (only
// fires on the key-down transition) so holding a key doesn't spam
// adjustments faster than intended; polled at a modest rate since this is
// not latency-critical.
static void StepClamped(float* value, float delta, float lo, float hi) {
    *value += delta;
    if (*value < lo) *value = lo;
    if (*value > hi) *value = hi;
}

DWORD WINAPI HotkeyThread(LPVOID) {
    bool prevF3 = false, prevF4 = false, prevF5 = false, prevF6 = false;
    for (;;) {
        Sleep(50);
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool f3 = ctrl && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        bool f4 = ctrl && (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        bool f5 = ctrl && (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        bool f6 = ctrl && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;

        if (f3 && !prevF3) {
            StepClamped(&g_shiftStrength, -0.005f, 0.0f, 0.15f);
            LogF("hotkey Ctrl+F3: separation (shiftStrength) = %.4f", g_shiftStrength);
            SaveConfig();
        }
        if (f4 && !prevF4) {
            StepClamped(&g_shiftStrength, 0.005f, 0.0f, 0.15f);
            LogF("hotkey Ctrl+F4: separation (shiftStrength) = %.4f", g_shiftStrength);
            SaveConfig();
        }
        if (f5 && !prevF5) {
            StepClamped(&g_convergence, -0.02f, 0.0f, 1.0f);
            LogF("hotkey Ctrl+F5: convergence = %.4f", g_convergence);
            SaveConfig();
        }
        if (f6 && !prevF6) {
            StepClamped(&g_convergence, 0.02f, 0.0f, 1.0f);
            LogF("hotkey Ctrl+F6: convergence = %.4f", g_convergence);
            SaveConfig();
        }

        prevF3 = f3; prevF4 = f4; prevF5 = f5; prevF6 = f6;
    }
    return 0;
}

DWORD WINAPI InitThread(LPVOID) {
    Log("=== TriDef custom D3D9 stereo hook DLL loaded ===");
    LoadConfig();
    InstallHook();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Only fires on a clean process exit (ExitProcess/FreeLibrary), not
        // on an abrupt TerminateProcess/taskkill - if the process is killed
        // forcefully, the replaced system cursor stays blank desktop-wide
        // until something else calls SystemParametersInfo(SPI_SETCURSORS)
        // (e.g. Windows itself does this on the next sign-in, or it can be
        // restored manually).
        RestoreSystemCursor();
    }
    return TRUE;
}
