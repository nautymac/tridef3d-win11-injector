// Custom depth-based stereo (Half-SBS) DXGI/D3D11 hook.
// Injected via injector.py (LoadLibraryW + CreateRemoteThread).
//
// Technique:
//  1. Hook ID3D11Device::CreateTexture2D - any depth/stencil texture gets forced
//     to a TYPELESS format + D3D11_BIND_SHADER_RESOURCE so we can read it later.
//  2. Hook ID3D11Device::CreateDepthStencilView - fix up Format=UNKNOWN requests
//     that would otherwise fail on a now-typeless resource.
//  3. Hook ID3D11DeviceContext::OMSetRenderTargets - remember which depth
//     resource the game is currently rendering the main scene into.
//  4. Hook IDXGISwapChain::Present - copy the back buffer to a readable
//     intermediate texture, look up an SRV for the tracked depth resource,
//     and run a full-screen pixel shader that does simple depth-weighted
//     horizontal reprojection into a Half-SBS composite, writing the result
//     directly into the real back buffer before presenting it.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <unordered_map>

#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------- logging --
static void Log(const char* msg) {
    OutputDebugStringA(msg);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\nauty\\tridef_hook_log.txt", "a") == 0 && f) {
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

// -------------------------------------------------------------- vtables ---
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
typedef HRESULT(__stdcall* CreateDepthStencilView_t)(ID3D11Device*, ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);
typedef void(__stdcall* OMSetRenderTargets_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

static Present_t oPresent = nullptr;
static CreateTexture2D_t oCreateTexture2D = nullptr;
static CreateDepthStencilView_t oCreateDepthStencilView = nullptr;
static OMSetRenderTargets_t oOMSetRenderTargets = nullptr;

// -------------------------------------------------------- depth tracking --
struct DepthFormatInfo { DXGI_FORMAT typeless; DXGI_FORMAT dsvFmt; DXGI_FORMAT srvFmt; };

static DepthFormatInfo GetDepthFormatInfo(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_D32_FLOAT:
            return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT };
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS };
        case DXGI_FORMAT_D16_UNORM:
            return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM };
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS };
        default:
            return { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    }
}

static std::unordered_map<ID3D11Resource*, DXGI_FORMAT> g_depthOriginalFormat;
static ID3D11Resource* g_trackedDepthResource = nullptr;

// ---------------------------------------------------------- render state --
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11SamplerState* g_sampler = nullptr;
static ID3D11Buffer* g_cbuf = nullptr;

static ID3D11Texture2D* g_colorCopyTex = nullptr;
static ID3D11ShaderResourceView* g_colorCopySRV = nullptr;
static UINT g_colorCopyW = 0, g_colorCopyH = 0;
static DXGI_FORMAT g_colorCopyFmt = DXGI_FORMAT_UNKNOWN;

static ID3D11Resource* g_cachedDepthResourceForSRV = nullptr;
static ID3D11ShaderResourceView* g_cachedDepthSRV = nullptr;

struct StereoParams { float shiftStrength; float reverseZ; float pad0; float pad1; };

static const char* kShaderSrc = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
    return o;
}

Texture2D colorTex : register(t0);
Texture2D depthTex : register(t1);
SamplerState samp0 : register(s0);

cbuffer Params : register(b0) {
    float shiftStrength;
    float reverseZ;
    float pad0;
    float pad1;
};

float4 PSMain(VSOut input) : SV_TARGET {
    float2 uv = input.uv;
    bool leftHalf = uv.x < 0.5;
    float2 srcUV = leftHalf ? float2(uv.x * 2.0, uv.y) : float2((uv.x - 0.5) * 2.0, uv.y);

    float depth = depthTex.Sample(samp0, srcUV).r;
    if (reverseZ > 0.5) depth = 1.0 - depth;

    float disparity = shiftStrength * (1.0 - depth);
    float2 shiftedUV = leftHalf ? srcUV + float2(disparity, 0) : srcUV - float2(disparity, 0);
    shiftedUV = saturate(shiftedUV);

    return colorTex.Sample(samp0, shiftedUV);
}
)";

static bool CreatePipeline() {
    ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * errBlob = nullptr;

    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), nullptr, nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) LogF("VS compile error: %s", (char*)errBlob->GetBufferPointer());
        return false;
    }
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), nullptr, nullptr, nullptr,
                     "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) LogF("PS compile error: %s", (char*)errBlob->GetBufferPointer());
        return false;
    }

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    vsBlob->Release();
    psBlob->Release();

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    g_device->CreateSamplerState(&sampDesc, &g_sampler);

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(StereoParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cbDesc, nullptr, &g_cbuf);

    Log("stereo pipeline created (shaders/sampler/cbuffer OK)");
    return true;
}

static void EnsureColorCopy(ID3D11Texture2D* backBuffer) {
    D3D11_TEXTURE2D_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);
    if (g_colorCopyTex && g_colorCopyW == bbDesc.Width && g_colorCopyH == bbDesc.Height && g_colorCopyFmt == bbDesc.Format) {
        return;
    }
    if (g_colorCopySRV) { g_colorCopySRV->Release(); g_colorCopySRV = nullptr; }
    if (g_colorCopyTex) { g_colorCopyTex->Release(); g_colorCopyTex = nullptr; }

    D3D11_TEXTURE2D_DESC desc = bbDesc;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_colorCopyTex);
    if (FAILED(hr)) {
        LogF("EnsureColorCopy: CreateTexture2D failed hr=0x%08X", (unsigned)hr);
        return;
    }
    hr = g_device->CreateShaderResourceView(g_colorCopyTex, nullptr, &g_colorCopySRV);
    if (FAILED(hr)) {
        LogF("EnsureColorCopy: CreateSRV failed hr=0x%08X", (unsigned)hr);
        return;
    }
    g_colorCopyW = bbDesc.Width; g_colorCopyH = bbDesc.Height; g_colorCopyFmt = bbDesc.Format;
    LogF("color copy texture (re)created %ux%u fmt=%d", g_colorCopyW, g_colorCopyH, (int)g_colorCopyFmt);
}

static ID3D11ShaderResourceView* GetDepthSRV() {
    if (!g_trackedDepthResource) return nullptr;
    if (g_cachedDepthResourceForSRV == g_trackedDepthResource && g_cachedDepthSRV) {
        return g_cachedDepthSRV;
    }
    if (g_cachedDepthSRV) { g_cachedDepthSRV->Release(); g_cachedDepthSRV = nullptr; }

    auto it = g_depthOriginalFormat.find(g_trackedDepthResource);
    if (it == g_depthOriginalFormat.end()) {
        return nullptr;
    }
    DepthFormatInfo info = GetDepthFormatInfo(it->second);
    if (info.srvFmt == DXGI_FORMAT_UNKNOWN) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = info.srvFmt;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    HRESULT hr = g_device->CreateShaderResourceView(g_trackedDepthResource, &srvDesc, &g_cachedDepthSRV);
    if (FAILED(hr)) {
        LogF("GetDepthSRV: CreateSRV failed hr=0x%08X", (unsigned)hr);
        return nullptr;
    }
    g_cachedDepthResourceForSRV = g_trackedDepthResource;
    Log("depth SRV (re)created for currently tracked depth resource");
    return g_cachedDepthSRV;
}

// ------------------------------------------------------------- Present ----
static float g_shiftStrength = 0.03f;
static float g_reverseZ = 0.0f;
static bool g_pipelineReady = false;

static HRESULT __stdcall HookPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    static bool loggedOnce = false;
    if (!loggedOnce) { Log("HookPresent: first call"); loggedOnce = true; }

    if (!g_device) {
        swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device);
        if (g_device) {
            g_device->GetImmediateContext(&g_context);
            g_pipelineReady = CreatePipeline();
        }
    }

    if (g_pipelineReady && g_device && g_context) {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
            EnsureColorCopy(backBuffer);
            if (g_colorCopyTex && g_colorCopySRV) {
                g_context->CopyResource(g_colorCopyTex, backBuffer);

                ID3D11ShaderResourceView* depthSRV = GetDepthSRV();
                if (depthSRV) {
                    ID3D11RenderTargetView* rtv = nullptr;
                    HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &rtv);
                    if (SUCCEEDED(hr)) {
                        D3D11_TEXTURE2D_DESC bbDesc{};
                        backBuffer->GetDesc(&bbDesc);
                        D3D11_VIEWPORT vp{ 0, 0, (float)bbDesc.Width, (float)bbDesc.Height, 0, 1 };
                        g_context->RSSetViewports(1, &vp);

                        D3D11_MAPPED_SUBRESOURCE mapped;
                        if (SUCCEEDED(g_context->Map(g_cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                            StereoParams* p = (StereoParams*)mapped.pData;
                            p->shiftStrength = g_shiftStrength;
                            p->reverseZ = g_reverseZ;
                            g_context->Unmap(g_cbuf, 0);
                        }

                        ID3D11RenderTargetView* rtvs[1] = { rtv };
                        g_context->OMSetRenderTargets(1, rtvs, nullptr);
                        g_context->VSSetShader(g_vs, nullptr, 0);
                        g_context->PSSetShader(g_ps, nullptr, 0);
                        ID3D11ShaderResourceView* srvs[2] = { g_colorCopySRV, depthSRV };
                        g_context->PSSetShaderResources(0, 2, srvs);
                        ID3D11SamplerState* samps[1] = { g_sampler };
                        g_context->PSSetSamplers(0, 1, samps);
                        g_context->PSSetConstantBuffers(0, 1, &g_cbuf);
                        g_context->IASetInputLayout(nullptr);
                        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        ID3D11Buffer* nullVB = nullptr;
                        UINT stride = 0, offset = 0;
                        g_context->IASetVertexBuffers(0, 1, &nullVB, &stride, &offset);
                        g_context->Draw(3, 0);

                        rtv->Release();
                    }
                }
            }
            backBuffer->Release();
        }
    }

    return oPresent(swapChain, syncInterval, flags);
}

// ----------------------------------------------------- CreateTexture2D ----
static HRESULT __stdcall HookCreateTexture2D(ID3D11Device* device, const D3D11_TEXTURE2D_DESC* pDesc,
                                              const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) {
    D3D11_TEXTURE2D_DESC desc = *pDesc;
    bool isDepth = false;
    DXGI_FORMAT originalFmt = pDesc->Format;

    if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) {
        DepthFormatInfo info = GetDepthFormatInfo(desc.Format);
        if (info.typeless != DXGI_FORMAT_UNKNOWN) {
            desc.Format = info.typeless;
            desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            isDepth = true;
        }
    }

    HRESULT hr = oCreateTexture2D(device, &desc, pInitialData, ppTexture2D);
    if (SUCCEEDED(hr) && isDepth && ppTexture2D && *ppTexture2D) {
        g_depthOriginalFormat[(ID3D11Resource*)*ppTexture2D] = originalFmt;
        LogF("depth texture created %ux%u origFmt=%d -> typeless, SRV-capable", desc.Width, desc.Height, (int)originalFmt);
    }
    return hr;
}

// ------------------------------------------------ CreateDepthStencilView --
static HRESULT __stdcall HookCreateDepthStencilView(ID3D11Device* device, ID3D11Resource* pResource,
                                                      const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                      ID3D11DepthStencilView** ppDepthStencilView) {
    D3D11_DEPTH_STENCIL_VIEW_DESC fixedDesc{};
    const D3D11_DEPTH_STENCIL_VIEW_DESC* descToUse = pDesc;

    if ((!pDesc || pDesc->Format == DXGI_FORMAT_UNKNOWN)) {
        auto it = g_depthOriginalFormat.find(pResource);
        if (it != g_depthOriginalFormat.end()) {
            DepthFormatInfo info = GetDepthFormatInfo(it->second);
            if (pDesc) fixedDesc = *pDesc;
            fixedDesc.Format = info.dsvFmt;
            if (!pDesc) {
                fixedDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            }
            descToUse = &fixedDesc;
        }
    }
    return oCreateDepthStencilView(device, pResource, descToUse, ppDepthStencilView);
}

// -------------------------------------------------- OMSetRenderTargets ----
static void __stdcall HookOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT numViews,
                                              ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (dsv) {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res) {
            g_trackedDepthResource = res;
            res->Release(); // don't hold an extra ref; only used as an identity key
        }
    }
    oOMSetRenderTargets(ctx, numViews, rtvs, dsv);
}

// --------------------------------------------------------- hook install ---
static void** GetVTable(void* pInstance) { return *reinterpret_cast<void***>(pInstance); }

template <typename T>
static void PatchVTableSlot(void** vtbl, int index, T hookFn, T* outOriginal) {
    DWORD oldProtect;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    *outOriginal = reinterpret_cast<T>(vtbl[index]);
    vtbl[index] = reinterpret_cast<void*>(hookFn);
    VirtualProtect(&vtbl[index], sizeof(void*), oldProtect, &oldProtect);
}

static bool InstallHooks() {
    Log("InstallHooks: creating dummy device/swapchain to read vtables");

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "TriDefStereoHookDummyWnd";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "dummy", WS_OVERLAPPEDWINDOW,
                                 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 100;
    scd.BufferDesc.Height = 100;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    D3D_FEATURE_LEVEL fl;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev, &fl, &ctx);

    if (FAILED(hr)) {
        LogF("D3D11CreateDeviceAndSwapChain failed hr=0x%08X", (unsigned)hr);
        DestroyWindow(hwnd);
        return false;
    }
    Log("dummy device/swapchain created OK");

    void** swapVtbl = GetVTable(swap);
    PatchVTableSlot(swapVtbl, 8, HookPresent, &oPresent);

    void** devVtbl = GetVTable(dev);
    PatchVTableSlot(devVtbl, 5, HookCreateTexture2D, &oCreateTexture2D);
    PatchVTableSlot(devVtbl, 10, HookCreateDepthStencilView, &oCreateDepthStencilView);

    void** ctxVtbl = GetVTable(ctx);
    PatchVTableSlot(ctxVtbl, 33, HookOMSetRenderTargets, &oOMSetRenderTargets);

    Log("all vtables patched: Present, CreateTexture2D, CreateDepthStencilView, OMSetRenderTargets");

    swap->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return true;
}

DWORD WINAPI InitThread(LPVOID) {
    Log("=== TriDef custom stereo hook DLL loaded ===");
    InstallHooks();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
