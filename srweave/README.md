# SRWeave — weaving with the SR SDK directly inside DX9/DX11 games

**English** · [한국어](README.ko.md)

Takes a game image that is already side-by-side (SBS) and weaves it for a lenticular panel with the SR (Simulated Reality / Acer SpatialLabs)
runtime, **inside the game process, right before `Present`**. It does not matter what made the SBS image — TriDef 3D, Geo-11/3DMigoto and ReShade SuperDepth3D all work.
It is one frame faster than capturing the screen and weaving that ([SRCapture3D](https://github.com/nautymac/SRCapture3D)),
and exclusive fullscreen still works.

Requirements: a lenticular 3D panel with the SR/SpatialLabs runtime installed and SR Service running.

| File | Target | Hook points |
|---|---|---|
| `SRWeaveDX9.dll` (x86/x64) | DirectX 9 | `IDirect3DDevice9::Present` / `Reset`, `IDirect3DSwapChain9::Present` |
| `SRWeaveDX11.dll` (x86/x64) | DirectX 11 | `IDXGISwapChain::Present` / `Present1` / `ResizeBuffers` |

Build output goes to `bin\x64\` and `bin\x86\`. Use the x86 DLL for a 32-bit game and the x64 DLL for a 64-bit game.

## How to use

**A. Injection (default)** — `Tridef3D_Play_SR.exe` (or `Tridef3D_Play.exe --sr`) from [tridef3d-win11-injector](https://github.com/nautymac/tridef3d-win11-injector)
injects this DLL after the TriDef DLLs, in order.
Put `sr\x86\SRWeaveDX9.dll`, `sr\x64\SRWeaveDX11.dll` and so on in the launcher folder. 32-bit game → DX9 DLL, 64-bit → DX11 DLL.

**B. Proxy (file copy)** — Copy `SRWeaveDX9.dll` into the game folder as `d3d9.dll`, or `SRWeaveDX11.dll` as `dxgi.dll` (or `d3d11.dll`).
When the game loads it, it passes calls on to the original in System32 and installs its hooks. Inject TriDef as usual.

## Settings and log

`SRWeave.ini` next to the DLL (`[SRWeave]` section). The log is `SRWeave.log` in the same folder.

| Key | Default | Meaning |
|---|---|---|
| `weave` | 1 | Turns weaving itself on or off |
| `swap_eyes` | 0 | Set to 1 when TriDef outputs the eyes in right\|left order |
| `srgb` | 0 | Treat the back buffer as sRGB |
| `test` | 0 | Left red / right blue instead of the game image (to check weaving and eye order) |
| `lens` | 1 | Turn the lenticular lens on while weaving |
| `latency_frames` | 1 | `setLatencyInFrames` |
| `hook_delay_ms` | 0 | Delays the moment the hooks are installed in injection mode |

Hotkeys (game window focused): Ctrl+Alt+W weaving, Ctrl+Alt+S swap left/right, Ctrl+Alt+T test pattern, Ctrl+Alt+L lens.

## How it gets underneath TriDef (the key part)

TriDef hooks even the device creation inside `d3d9.dll` and returns **every device as a complete wrapper object** (the whole vtable is in
`TriDefD3D9.dll`). It also replaces **the vtable pointer of the real device object with a heap copy** and patches only `Present`/`Reset` in it
to point to its own functions. So:

1. Create a temporary device (a wrapper). In the members of its swap chain wrapper, find the **real swap chain**, whose vtable is inside `d3d9.dll`.
2. Get the **real device** with the real swap chain's `GetDevice()` (its vtable is the heap copy).
3. Use the unpatched entries in that copy as a fingerprint to find the original vtable in the original `d3d9.dll` image on disk,
   and get the original `Present`/`Reset` addresses from there (`CleanVtableEntryByFingerprint`).
4. **Inline-hook those real functions with MinHook**. When the TriDef wrapper has finished drawing the SBS image and calls the original, we weave inside that call.

The temporary objects are not released — releasing the TriDef wrapper destabilized TriDef and the game crashed.
`IDirect3DDevice9Ex::PresentEx` is not hooked yet (creating an Ex device also gets it wrapped, and that is unstable).

Weaving steps (DX9): back buffer → `StretchRect` → SBS texture, `setInputViewTexture(tex, width/2, height, format)`,
capture a state block → RT = back buffer, no depth, Z off → `BeginScene`/`weave()`/`EndScene` → restore state → original `Present`.
`invalidateDeviceObjects`/`restoreDeviceObjects` before and after `Reset`.

## Build

```
powershell -ExecutionPolicy Bypass -File setup-deps.ps1 -SrSdkInstaller <win64.exe> -SrSdk32Installer <win32.exe>
powershell -ExecutionPolicy Bypass -File build.ps1
```

All you need is MSVC (x86 and x64 cross tools) and the Windows 10 SDK. `build.ps1` finds MSVC and the SDK by itself and builds both architectures
with Ninja (if it can't find them, set `$env:SRWEAVE_MSVC` and `$env:SRWEAVE_SDK`). The CRT is linked statically, the SR runtime DLLs are
delay-loaded (they are looked up in the SpatialLabs install folder), and MinHook (BSD-2) is included in `third_party\minhook`.

The SR SDK is not redistributed — `setup-deps.ps1` unpacks it from a LeiaSR/SpatialLabs runtime installer. To build the x86 DLL
you also need the win32 SDK. It is inside the `temp\` folder you get when you unpack the win64 installer.

## Verification (2026-09-23)

Left 4 Dead (DX9, 32-bit) + TriDef 3D: hooks on the real `d3d9.dll` functions, SR context and weaver created, `first weaved frame`,
two in-game resolution changes (`Reset`) passed, the game kept running.

Binary Domain (DX9, 32-bit) + TriDef 3D: `GetSwapChain(0)` on the temporary device wrapper failed with `0x8876086c` (D3DERR_INVALIDCALL),
so at first not a single hook was installed. A fallback path was added — instead of going through the swap chain, **find the real device directly in the members of the device wrapper**
(`FindInnerBySlots`: an object where at least 80 of the 119 vtable slots point into `d3d9.dll`, confirmed with `QueryInterface(IDirect3DDevice9)`).
The fingerprint is taken from that heap vtable to find the original `Present`/`Reset`. Result: hook → SR context → weaver → `first weaved frame: 3840x2160`,
weaving confirmed by eye on the panel (2026-09-23). Injecting with `Tridef3D_Play_SR.exe` without administrator rights also works (because Steam starts the game with normal rights).

Gone Home (DX11, 64-bit) + TriDef 3D: TriDef's DXGI layer (`TriDefDXGI64.dll`) also returns the swap chain as a complete wrapper, so at first
0 hooks were installed. `Present`/`Present1`/`ResizeBuffers` are hooked through the same fallback path as for the DX9 device (`FindInnerBySlots` finds the real swap chain with a dxgi.dll vtable in field 1 of the wrapper,
confirmed with `QueryInterface(IDXGISwapChain)`). Result: weaver created →
`first weaved frame: 3840x2160`, two `ResizeBuffers` calls passed, the game kept running (2026-09-23).
Left 4 Dead was rechecked for regressions with the same build (the swap chain path still works).

## License

MIT — `LICENSE` at the repository root. The included [MinHook](https://github.com/TsudaKageyu/minhook) is BSD-2
(`third_party\minhook\LICENSE.txt`). The SR SDK and runtime are not included — they belong to Leia/Acer.

## Where this folder lives

It was originally `weave\` in [SRCapture3D](https://github.com/nautymac/SRCapture3D), and on 2026-09-27 it moved to `srweave\` in this repository
(`tridef3d-win11-injector`) — because it is always used together with the launcher that injects this DLL with `--sr`.
The history is preserved.
