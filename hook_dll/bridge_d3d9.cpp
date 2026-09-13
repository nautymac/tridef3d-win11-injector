// Bridge DLL: hooks IDirect3D9::CreateDevice (shared vtable trick, same
// technique as the DXGI/D3D11 marker test) and, once the game's real
// IDirect3DDevice9 exists, calls TriDefD3D9.dll's exported
// TriDef3DSDKFunc(device, 0, 0, 0) - the entry point we found by tracing
// ProduceStereo's callers back through the DLL, which the *original*
// TriDefInjector's synthesized loader stub would normally have called
// after manually mapping the DLL. We supply the missing "find the real
// device and call the real activation entry point" glue ourselves,
// while leaving TriDef's own (already-verified, depth-shift) rendering
// pipeline completely untouched.
//
// Load order matters: TriDefD3D9.dll (and TriDefIgnition.dll) must
// already be loaded in the process before this DLL's DllMain runs, so
// GetModuleHandle("TriDefD3D9.dll") + GetProcAddress finds the real
// function. The injector loads DLLs in the order given on its command
// line, so just list this one last.

#include <windows.h>
#include <d3d9.h>
#include <cstdio>

typedef int (__cdecl *TriDef3DSDKFunc_t)(void*, DWORD, DWORD, DWORD);
static TriDef3DSDKFunc_t pTriDef3DSDKFunc = nullptr;

typedef HRESULT (__stdcall *CreateDevice_t)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static CreateDevice_t oCreateDevice = nullptr;

static void Log(const char* msg) {
    OutputDebugStringA(msg);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\nauty\\tridef_bridge_log.txt", "a") == 0 && f) {
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

static HRESULT __stdcall HookCreateDevice(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface) {
    Log("HookCreateDevice called");
    HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                pPresentationParameters, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface && pTriDef3DSDKFunc) {
        // TriDef3DSDKFunc crashed when called without TriDefIgnition.dll
        // present at all - it apparently reads global state Ignition's
        // own static initializers set up. Rather than race an external
        // CreateRemoteThread injection of Ignition against this engine's
        // (fast) device creation, load it ourselves right here: we're
        // already running inside the target process, so this is just a
        // normal blocking LoadLibrary call - no IPC, no separate thread
        // to race against, and it won't return until Ignition's own
        // DllMain/static init has actually finished.
        if (!GetModuleHandleA("TriDefIgnition.dll")) {
            HMODULE h = LoadLibraryA("C:\\Program Files (x86)\\TriDef\\TriDef\\TriDefIgnition\\Driver\\TriDefIgnition.dll");
            LogF("self-loaded TriDefIgnition.dll -> %p", (void*)h);
        }
        LogF("real device created @ %p, calling TriDef3DSDKFunc (Ignition present: %d)",
             (void*)*ppReturnedDeviceInterface, GetModuleHandleA("TriDefIgnition.dll") != nullptr);

        // The crash is a NULL-pointer *write* deep inside real d3d9.dll:
        // one of TriDef3DSDKFunc's param_2/3/4 gets forwarded to an
        // internal "fill in this caller-supplied struct" style D3D9 call
        // (mov [eax], ebx with eax==0). We don't know which one needs a
        // real buffer, so give all three a valid, zeroed, oversized
        // scratch buffer instead of NULL.
        static BYTE scratch2[256] = {0};
        static BYTE scratch3[256] = {0};
        static BYTE scratch4[256] = {0};
        __try {
            int result = pTriDef3DSDKFunc(*ppReturnedDeviceInterface, (DWORD)scratch2, (DWORD)scratch3, (DWORD)scratch4);
            LogF("TriDef3DSDKFunc returned %d", result);
        }
        __except (
            LogF("EXCEPTION in TriDef3DSDKFunc: code=0x%08X addr=%p eax=0x%08X ebx=0x%08X ecx=0x%08X "
                 "edx=0x%08X esi=0x%08X edi=0x%08X ebp=0x%08X esp=0x%08X eip=0x%08X",
                 (unsigned)GetExceptionCode(),
                 (void*)GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                 GetExceptionInformation()->ContextRecord->Eax,
                 GetExceptionInformation()->ContextRecord->Ebx,
                 GetExceptionInformation()->ContextRecord->Ecx,
                 GetExceptionInformation()->ContextRecord->Edx,
                 GetExceptionInformation()->ContextRecord->Esi,
                 GetExceptionInformation()->ContextRecord->Edi,
                 GetExceptionInformation()->ContextRecord->Ebp,
                 GetExceptionInformation()->ContextRecord->Esp,
                 GetExceptionInformation()->ContextRecord->Eip),
            EXCEPTION_EXECUTE_HANDLER) {
            Log("caught the exception, continuing safely (game will keep running)");
        }
    } else {
        LogF("CreateDevice hr=0x%08X ppDevice=%p pFunc=%p", (unsigned)hr,
             ppReturnedDeviceInterface ? (void*)*ppReturnedDeviceInterface : nullptr,
             (void*)pTriDef3DSDKFunc);
    }
    return hr;
}

static void** GetVTable(void* pInstance) { return *reinterpret_cast<void***>(pInstance); }

static bool InstallHook() {
    HMODULE hTriDefD3D9 = GetModuleHandleA("TriDefD3D9.dll");
    if (!hTriDefD3D9) {
        Log("TriDefD3D9.dll not found in this process - load it before this DLL");
        return false;
    }
    pTriDef3DSDKFunc = reinterpret_cast<TriDef3DSDKFunc_t>(GetProcAddress(hTriDefD3D9, "TriDef3DSDKFunc"));
    LogF("TriDef3DSDKFunc resolved @ %p", (void*)pTriDef3DSDKFunc);
    if (!pTriDef3DSDKFunc) return false;

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

DWORD WINAPI InitThread(LPVOID) {
    Log("=== bridge_d3d9 loaded ===");
    InstallHook();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
