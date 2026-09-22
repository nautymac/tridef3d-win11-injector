#include "common.h"

#include <delayimp.h>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <mutex>

#include "sr/management/srcontext.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/utility/exception.h"

namespace srw {

Config g_cfg;
HMODULE g_self = nullptr;
SrCore g_sr;

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;

std::wstring SelfDir()
{
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    size_t i = s.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : s.substr(0, i);
}

std::wstring SelfFileName()
{
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(g_self, p, MAX_PATH);
    std::wstring s(p);
    size_t i = s.find_last_of(L"\\/");
    if (i != std::wstring::npos) s = s.substr(i + 1);
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

void Init(HMODULE self, const char* product)
{
    g_self = self;
    std::wstring ini = SelfDir() + L"\\SRWeave.ini";
    auto b = [&](const wchar_t* k, bool d) { return GetPrivateProfileIntW(L"SRWeave", k, d ? 1 : 0, ini.c_str()) != 0; };
    g_cfg.weave = b(L"weave", true);
    g_cfg.swapEyes = b(L"swap_eyes", false);
    g_cfg.srgb = b(L"srgb", false);
    g_cfg.test = b(L"test", false);
    g_cfg.lens = b(L"lens", true);
    g_cfg.log = b(L"log", true);
    g_cfg.latencyFrames = (int)GetPrivateProfileIntW(L"SRWeave", L"latency_frames", 1, ini.c_str());
    g_cfg.hookDelayMs = (int)GetPrivateProfileIntW(L"SRWeave", L"hook_delay_ms", 0, ini.c_str());
    if (g_cfg.log) {
        std::wstring lp = SelfDir() + L"\\SRWeave.log";
        g_logFile = _wfopen(lp.c_str(), L"a");
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("=== %s (%s) loaded as %ls into %ls (pid %lu) ===", product, SRW_ARCH, SelfFileName().c_str(), exe, GetCurrentProcessId());
    Log("config: weave=%d swap_eyes=%d srgb=%d test=%d lens=%d latency_frames=%d hook_delay_ms=%d",
        g_cfg.weave, g_cfg.swapEyes, g_cfg.srgb, g_cfg.test, g_cfg.lens, g_cfg.latencyFrames, g_cfg.hookDelayMs);
}

void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), buf);
    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) { fputs(line, g_logFile); fflush(g_logFile); }
}

HMODULE LoadSystemDll(const wchar_t* name)
{
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);   // SysWOW64 for a 32-bit process
    std::wstring p = std::wstring(dir) + L"\\" + name;
    // Full path: even if a same-named proxy (possibly us) is already loaded from the game folder,
    // the loader treats a different full path as a different module.
    return LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

std::string ModuleOf(const void* addr)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) || !m)
        return "?";
    char p[MAX_PATH] = {};
    GetModuleFileNameA(m, p, MAX_PATH);
    std::string s(p);
    size_t i = s.find_last_of("\\/");
    return i == std::string::npos ? s : s.substr(i + 1);
}

bool AddrInModule(const void* addr, HMODULE m)
{
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)addr, &owner);
    return owner == m;
}

static bool ReadPtr(const void* at, void** out)
{
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), at, out, sizeof(void*), &n) && n == sizeof(void*);
}

static bool VtableInModule(void** vt, HMODULE mod, int mustHaveIndex)
{
    if (!vt || !AddrInModule(vt, mod)) return false;
    for (int i = 0; i <= mustHaveIndex; i++) {
        void* f = nullptr;
        if (!ReadPtr(&vt[i], &f) || !f || !AddrInModule(f, mod)) return false;
    }
    return true;
}

void** UnwrapVtable(void* obj, HMODULE mod, int mustHaveIndex, void** innerObj)
{
    if (innerObj) *innerObj = nullptr;
    if (!obj || !mod) return nullptr;
    void** vt = nullptr;
    if (!ReadPtr(obj, (void**)&vt)) return nullptr;
    if (VtableInModule(vt, mod, mustHaveIndex)) { if (innerObj) *innerObj = obj; return vt; }
    // wrapper: look for the wrapped object among its first fields
    for (int i = 1; i < 128; i++) {
        void* cand = nullptr;
        if (!ReadPtr((void**)obj + i, &cand) || !cand) continue;
        if (((ULONG_PTR)cand & (sizeof(void*) - 1)) != 0) continue;
        void** cvt = nullptr;
        if (!ReadPtr(cand, (void**)&cvt)) continue;
        if (VtableInModule(cvt, mod, mustHaveIndex)) {
            Log("unwrapped: field %d of %p (vtable in %s) -> inner object %p", i, obj, ModuleOf(vt).c_str(), cand);
            if (innerObj) *innerObj = cand;
            return cvt;
        }
    }
    return nullptr;
}

void* CleanVtableEntryByFingerprint(HMODULE realMod, const wchar_t* sysDllName, void** liveVt, int nMatch, int index)
{
    if (!realMod || !liveVt) return nullptr;
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring p = std::wstring(dir) + L"\\" + sysDllName;
    HMODULE img = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    if (!img) { Log("clean mapping of %ls failed (%lu)", sysDllName, GetLastError()); return nullptr; }
    BYTE* base = (BYTE*)((ULONG_PTR)img & ~(ULONG_PTR)0xFFFF);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const ULONG_PTR imageBase = (ULONG_PTR)nt->OptionalHeader.ImageBase;
    const DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    const ULONG_PTR realBase = (ULONG_PTR)realMod;

    // anchors: slots of the live copy that still point into the real module
    struct Anchor { int k; ULONG_PTR fileValue; };
    Anchor anchors[64];
    int na = 0;
    for (int k = 0; k < nMatch && na < 64; k++) {
        void* f = nullptr;
        if (ReadPtr(&liveVt[k], &f) && f && AddrInModule(f, realMod))
            anchors[na++] = { k, (ULONG_PTR)f - realBase + imageBase };
    }
    void* result = nullptr;
    if (na < 4) {
        Log("fingerprint of %ls vtable: only %d unpatched slots, not enough", sysDllName, na);
    } else {
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections && !result; s++, sec++) {
            if (!(sec->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)) continue;
            DWORD start = sec->VirtualAddress, end = sec->VirtualAddress + sec->Misc.VirtualSize;
            if (end > sizeOfImage) end = sizeOfImage;
            for (DWORD off = start & ~(DWORD)(sizeof(void*) - 1); off + sizeof(void*) <= end; off += sizeof(void*)) {
                if (*(ULONG_PTR*)(base + off) != anchors[0].fileValue) continue;
                // candidate vtable start
                LONG_PTR vtRva = (LONG_PTR)off - (LONG_PTR)anchors[0].k * (LONG_PTR)sizeof(void*);
                if (vtRva < 0) continue;
                bool ok = true;
                for (int a = 1; a < na && ok; a++) {
                    DWORD o = (DWORD)vtRva + (DWORD)anchors[a].k * sizeof(void*);
                    ok = (o + sizeof(void*) <= sizeOfImage) && *(ULONG_PTR*)(base + o) == anchors[a].fileValue;
                }
                if (!ok) continue;
                DWORD o = (DWORD)vtRva + (DWORD)index * sizeof(void*);
                if (o + sizeof(void*) > sizeOfImage) break;
                ULONG_PTR entry = *(ULONG_PTR*)(base + o);
                ULONG_PTR real = entry - imageBase + realBase;
                if (real - realBase < sizeOfImage) {
                    result = (void*)real;
                    Log("fingerprint (%d anchors) matched %ls vtable at RVA 0x%lx; slot %d -> %p", na, sysDllName, (unsigned long)vtRva, index, result);
                }
                break;
            }
        }
        if (!result) Log("fingerprint (%d anchors) found no matching vtable in %ls", na, sysDllName);
    }
    FreeLibrary(img);
    return result;
}

void* CleanVtableEntry(HMODULE realMod, const wchar_t* sysDllName, void** vtable, int index)
{
    if (!realMod || !vtable) return nullptr;
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring p = std::wstring(dir) + L"\\" + sysDllName;
    // Image-resource mapping: sections laid out at their RVAs, nothing relocated, nothing executed.
    HMODULE img = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    if (!img) { Log("clean mapping of %ls failed (%lu)", sysDllName, GetLastError()); return nullptr; }
    BYTE* base = (BYTE*)((ULONG_PTR)img & ~(ULONG_PTR)0xFFFF);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    ULONG_PTR imageBase = (ULONG_PTR)nt->OptionalHeader.ImageBase;
    DWORD sizeOfImage = nt->OptionalHeader.SizeOfImage;
    ULONG_PTR realBase = (ULONG_PTR)realMod;
    ULONG_PTR vtRva = (ULONG_PTR)vtable - realBase;
    void* result = nullptr;
    if (vtRva < sizeOfImage) {
        ULONG_PTR fileEntry = *(ULONG_PTR*)(base + vtRva + (ULONG_PTR)index * sizeof(void*));
        ULONG_PTR real = fileEntry - imageBase + realBase;
        if (real - realBase < sizeOfImage) result = (void*)real;
        else Log("clean vtable[%d] of %ls out of range (file value %p)", index, sysDllName, (void*)fileEntry);
    } else {
        Log("vtable %p is not inside %ls (someone replaced the object's vtable pointer)", (void*)vtable, sysDllName);
    }
    FreeLibrary(img);
    return result;
}

// ---- hotkeys -------------------------------------------------------------------------------

bool Hotkey(int vk)
{
    static bool prev[256] = {};
    bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)
                && (GetAsyncKeyState(vk) & 0x8000);
    bool edge = down && !prev[vk & 0xff];
    prev[vk & 0xff] = down;
    return edge;
}

bool PollHotkeys()
{
    bool lensChanged = false;
    if (Hotkey('W')) { g_cfg.weave = !g_cfg.weave; Log("hotkey: weave %s", g_cfg.weave ? "ON" : "OFF"); lensChanged = true; }
    if (Hotkey('S')) { g_cfg.swapEyes = !g_cfg.swapEyes; Log("hotkey: swap_eyes %d", g_cfg.swapEyes); }
    if (Hotkey('T')) { g_cfg.test = !g_cfg.test; Log("hotkey: test pattern %d", g_cfg.test); }
    if (Hotkey('L')) { g_cfg.lens = !g_cfg.lens; Log("hotkey: lens %d", g_cfg.lens); lensChanged = true; }
    return lensChanged;
}

// ---- SR context ----------------------------------------------------------------------------

namespace {

// Delay-load failures (SR runtime DLL missing) arrive as SEH exceptions; C++ exceptions pass through.
int DelayLoadFilter(unsigned code)
{
    return (code == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND)
            || code == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND))
           ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// No C++ objects with destructors inside a __try body, hence the split.
void DoCreate(SR::SRContext** out) { *out = SR::SRContext::create(); }

bool DoCreateSEH(SR::SRContext** out, bool* dllMissing)
{
    __try {
        DoCreate(out);
        return true;
    } __except (DelayLoadFilter(GetExceptionCode())) {
        *dllMissing = true;
        return false;
    }
}

// Where the SR runtime lives. The 64-bit runtime folder is normally on PATH (SpatialLabs adds it),
// the 32-bit one usually isn't, so the delay-load hook below looks here first.
const wchar_t* const kRuntimeDirs[] = {
#ifdef _WIN64
    L"C:\\Program Files\\Acer\\SpatialLabs\\Platform\\bin",
    L"C:\\Program Files\\Simulated Reality\\Platform\\bin",
#else
    L"C:\\Program Files (x86)\\Simulated Reality\\Platform\\bin",
    L"C:\\Program Files (x86)\\Acer\\SpatialLabs\\Platform\\bin",
#endif
};

FARPROC WINAPI DliHook(unsigned reason, DelayLoadInfo* info)
{
    if (reason != dliNotePreLoadLibrary || !info || !info->szDll) return nullptr;
    wchar_t name[MAX_PATH] = {};
    MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, name, MAX_PATH);
    // next to us first (lets a user drop the runtime DLLs beside the weaver), then the known folders
    std::wstring cand = SelfDir() + L"\\" + name;
    if (HMODULE m = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return (FARPROC)m;
    for (const wchar_t* dir : kRuntimeDirs) {
        cand = std::wstring(dir) + L"\\" + name;
        if (HMODULE m = LoadLibraryExW(cand.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return (FARPROC)m;
    }
    return nullptr;   // fall back to the normal search (PATH)
}

}   // namespace

}   // namespace srw

extern "C" const PfnDliHook __pfnDliNotifyHook2 = srw::DliHook;

namespace srw {

bool SrEnsureContext()
{
    if (g_sr.ctx) return true;
    if (g_sr.dead) return false;
    ULONGLONG now = GetTickCount64();
    if (now - g_sr.lastTry < 2000) return false;   // don't hammer the service from the render thread
    g_sr.lastTry = now;
    g_sr.tries++;
    bool dllMissing = false;
    try {
        if (!DoCreateSEH(&g_sr.ctx, &dllMissing)) {
            Log("SR runtime DLL not found (SimulatedRealityCore%s/DirectX%s.dll) - weaving disabled", SRW_SR_SUFFIX, SRW_SR_SUFFIX);
            g_sr.dead = true;
            return false;
        }
    } catch (const SR::ServerNotAvailableException&) {
        g_sr.ctx = nullptr;
        if (g_sr.tries == 1 || g_sr.tries % 10 == 0)
            Log("SR Service not available (try %d) - is the SR/SpatialLabs service running?", g_sr.tries);
        if (g_sr.tries >= 60) { Log("giving up on SR Service"); g_sr.dead = true; }
        return false;
    } catch (const std::exception& e) {
        Log("SRContext::create failed: %s", e.what());
        g_sr.ctx = nullptr;
        g_sr.dead = true;
        return false;
    } catch (...) {
        Log("SRContext::create failed (unknown exception)");
        g_sr.ctx = nullptr;
        g_sr.dead = true;
        return false;
    }
    Log("SR context created (try %d)", g_sr.tries);
    return g_sr.ctx != nullptr;
}

void SrFinishInit()
{
    if (!g_sr.ctx || g_sr.initialized) return;
    try {
        g_sr.lens = SR::SwitchableLensHint::create(*g_sr.ctx);   // owned by the context
        g_sr.ctx->initialize();
        g_sr.initialized = true;
        Log("SR context initialized");
    } catch (const std::exception& e) {
        Log("SR initialize failed: %s", e.what());
        g_sr.dead = true;
    } catch (...) {
        Log("SR initialize failed (unknown exception)");
        g_sr.dead = true;
    }
}

void SrSetLens(bool on)
{
    if (!g_sr.lens || g_sr.lensOn == on) return;
    try {
        if (on) g_sr.lens->enable(); else g_sr.lens->disable();
        g_sr.lensOn = on;
        Log("lens %s", on ? "ON" : "OFF");
    } catch (...) {
        Log("lens hint call failed");
    }
}

}   // namespace srw
