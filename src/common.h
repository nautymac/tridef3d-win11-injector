// Shared bits for SRWeaveDX9 / SRWeaveDX11: config, log, module helpers,
// hotkeys and the SR context (which is graphics-API independent).
#pragma once
#include <windows.h>
#include <string>

namespace SR { class SRContext; class SwitchableLensHint; }

namespace srw {

struct Config {
    bool weave = true;       // do the weaving at all (Ctrl+Alt+W toggles)
    bool swapEyes = false;   // TriDef emitting R|L instead of L|R (Ctrl+Alt+S)
    bool srgb = false;       // treat the back buffer as sRGB
    bool test = false;       // red/blue halves instead of the game image (Ctrl+Alt+T)
    bool lens = true;        // switch the lenticular lens on while weaving (Ctrl+Alt+L)
    bool log = true;
    int latencyFrames = 1;   // IWeaverBase1::setLatencyInFrames
    int hookDelayMs = 0;     // injected mode: wait before installing the hooks
};
extern Config g_cfg;
extern HMODULE g_self;

// Reads SRWeave.ini next to the DLL, opens SRWeave.log there. `product` names the DLL in the log.
void Init(HMODULE self, const char* product);
void Log(const char* fmt, ...);
std::wstring SelfDir();
std::wstring SelfFileName();                 // lower-case file name of this module, e.g. "d3d9.dll"
HMODULE LoadSystemDll(const wchar_t* name);  // always the copy in System32 / SysWOW64, never a proxy
std::string ModuleOf(const void* addr);      // base name of the module containing addr, or "?"
bool AddrInModule(const void* addr, HMODULE m);
// The *unpatched* value of vtable[index], read from the on-disk System32 copy of the DLL that
// owns the vtable (mapped as an image, relocated to realMod's base). Lets us find the real
// Present even after TriDef has overwritten the live vtable entry with its own function.
// Returns nullptr if the vtable isn't inside realMod or the mapping fails.
void* CleanVtableEntry(HMODULE realMod, const wchar_t* sysDllName, void** vtable, int index);
// TriDef hands out *wrapper* COM objects whose vtables live in its own DLL and which hold the
// real d3d object in a member. Given such an object (or a real one), return the real object's
// vtable: the object's own vtable if it is inside `mod`, otherwise the vtable of the first
// pointer-sized field that points at an object whose vtable (entries 0..mustHaveIndex) is
// inside `mod`. Returns nullptr if nothing plausible is found. Optionally returns the inner object.
void** UnwrapVtable(void* obj, HMODULE mod, int mustHaveIndex, void** innerObj = nullptr);
// Looser variant for objects whose vtable pointer TriDef redirected to a heap copy: find, among
// the wrapper's first fields, an object whose vtable (wherever it lives) has at least `minHits`
// of its first `nSlots` entries inside `mod`. Returns that object; *outVt gets its live vtable.
// Scanning starts at field `*field` and advances it past the match, so a caller can reject a
// candidate and keep looking.
void* FindInnerBySlots(void* obj, HMODULE mod, int nSlots, int minHits, void*** outVt, int* field);
// TriDef copies an object's vtable to the heap and patches a few slots (Present, Reset...).
// Given that live copy, use its *unpatched* slots (the ones still pointing into `realMod`,
// among the first `nMatch`) as a fingerprint to locate the original vtable inside the on-disk
// image of the DLL, and return the original value of slot `index`, relocated to realMod.
void* CleanVtableEntryByFingerprint(HMODULE realMod, const wchar_t* sysDllName, void** liveVt, int nMatch, int index);

// Ctrl+Alt+<vk>, rising edge. Call from the render thread once per frame.
bool Hotkey(int vk);
// Applies the standard hotkeys to g_cfg. Returns true if the lens state should be re-applied.
bool PollHotkeys();

// SR context, shared by whatever weavers the DLL creates.
struct SrCore {
    SR::SRContext* ctx = nullptr;
    SR::SwitchableLensHint* lens = nullptr;
    bool initialized = false;   // ctx->initialize() has run
    bool dead = false;          // gave up (runtime missing / too many failures)
    bool lensOn = false;
    ULONGLONG lastTry = 0;
    int tries = 0;
};
extern SrCore g_sr;
bool SrEnsureContext();     // rate-limited create; true when g_sr.ctx is usable
void SrFinishInit();        // once, after the first weaver exists: lens hint + initialize()
void SrSetLens(bool on);

}   // namespace srw
