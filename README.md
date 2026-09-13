# TriDef 3D Injector (Windows 11 fix)

A replacement launcher/injector for **TriDef 3D Ignition** — the stereoscopic-3D
driver by DDD (defunct since ~2017) — that works reliably on modern Windows 11
and modern games, without touching or redistributing any of TriDef's own
binaries.

## Why this exists

TriDef 3D shipped with ~800 hand-tuned per-game profiles, but the bundled
`TriDefInjector64.exe` is fragile on today's systems for two unrelated
reasons:

1. **Wrong process.** When a game is registered as a Steam title, Ignition
   launches it via `steam.exe steam://rungameid/<id>`. Steam is already
   running as a long-lived process, so that call doesn't spawn the game —
   it just signals the existing Steam client, which then spawns the real
   game as *its own* child. Ignition never sees that child; it hands the
   injector the PID of the throwaway `steam.exe` launch instead, so the
   3D hook ends up injected into Steam itself, not the game.
2. **Naive memory search.** The injector allocates memory for its payload
   by linearly scanning *upward only* from the target module's base
   address. Games in 2016 (when this was last updated) loaded a few dozen
   DLLs; a modern game can load 100+, often filling the address space all
   the way to the top — so the search comes up empty and injection fails
   silently (exit code 4).

Reverse-engineering both of these (see the commit history / write-up) led
to a small, from-scratch injector that sidesteps both problems entirely
instead of patching the 2016-era binary.

## How it works

- **Launch via the real `steam://` protocol**, never the game's `.exe`
  directly. This is what makes Steamworks games initialize normally
  instead of detecting "not launched by Steam" and silently killing +
  relaunching themselves through the persistent Steam client (which
  would again land outside any process tree we could otherwise watch).
- **Poll for the new process at native speed** (a tight loop over
  `CreateToolhelp32Snapshot`, no `Sleep` throttling worth mentioning) and
  inject the instant it appears — racing to get there before the game
  creates its Direct3D device. No thread suspension is used: freezing a
  brand-new process's only thread is very likely to freeze it *while it
  holds its own loader lock*, which would deadlock the injection thread
  forever. Plain speed avoids that trap entirely.
- **Standard `LoadLibraryW` + `CreateRemoteThread` injection** — the same
  well-understood technique used by tools like ReShade and 3DMigoto.
  - **64-bit / DirectX 11 games**: loads TriDef's own real
    `TriDefIgnition64.dll`, `TriDefD3D1164.dll` and `TriDefDXGI64.dll`
    from wherever the user's existing TriDef 3D install already has them.
  - **32-bit / DirectX 9 games**: TriDef's own D3D9 activation entry point
    turned out to be fundamentally uncallable from outside code (see
    `hook_dll/bridge_d3d9.cpp` for the investigation), so these instead
    get `hook_dll/tridef_d3d9_hook.dll` — a from-scratch depth-based
    Half-SBS stereo hook, built entirely on public D3D9 APIs, with no
    TriDef code involved. See the [`hook_dll/`](#hook_dll--a-from-scratch-d3d9-stereo-hook)
    section below.

  Either way, the launcher picks the right one automatically — running a
  DX9 or DX11 game looks identical from the command line.

This repo contains **no TriDef binaries** and never copies or bundles
them — it only calls `LoadLibraryW` on paths already present from the
user's own licensed install (read from the registry TriDef itself
writes at `HKLM\SOFTWARE\WOW6432Node\DDD`). You need TriDef 3D installed
yourself for any of this to do anything.

## Requirements

- Windows 10/11, 64-bit
- [TriDef 3D](https://en.wikipedia.org/wiki/TriDef_3D) installed (abandonware —
  you'll need your own installer/license; this project doesn't provide one)
- The target game added once in TriDef 3D Ignition's own UI (so it's
  registered under `HKCU\SOFTWARE\DDD\TriDefIgnition\Games\<name>`) —
  you never need to launch it *from* Ignition, just add it there once
- Steam, with the game installed
- Administrator privileges (required for cross-process injection)
- Python 3.9+ if running from source; no Python needed if using the
  prebuilt `Play3D.exe` from [Releases](../../releases)

## Usage

```
Play3D.exe
```

Run it (as Administrator) with no arguments and it lists every Steam
game you've registered in TriDef 3D Ignition — pick a number, or just
press Enter to relaunch whatever you ran last time.

```
Play3D.exe "Gone Home"
```

Or name the game directly. `Play3D.bat` is a double-click-friendly
wrapper that keeps the console window open afterward. Works identically
whether the game turns out to be 64-bit/DX11 or 32-bit/DX9 — the DLLs to
inject are picked automatically once the game's architecture is detected.

For 32-bit/DX9 games, once running: `Ctrl+F3`/`Ctrl+F4` adjust
separation, `Ctrl+F5`/`Ctrl+F6` adjust convergence (same keys as TriDef's
own Ignition driver) — see [`hook_dll/`](#hook_dll--a-from-scratch-d3d9-stereo-hook) below.

Running from source instead of the prebuilt exe:

```
python play3d.py "Gone Home"
python play3d.py "Gone Home" --dry-run   # show what it would do, don't launch
```

## Building it yourself

```
pip install pyinstaller
pyinstaller build/Play3D.spec --distpath .
```

`hook_dll/tridef_d3d9_hook.dll` (needed for 32-bit/DX9 games) is a
separate native build — requires the MSVC x86 toolchain and Windows SDK:

```
powershell -ExecutionPolicy Bypass -File hook_dll/build_d3d9_32.ps1
```

## `hook_dll/` — a from-scratch D3D9 stereo hook

`hook_dll/hook_d3d9.cpp` is what actually runs for every 32-bit/DirectX 9
game — TriDef's own D3D9 activation path (`TriDef3DSDKFunc`) depends on an
undocumented register value its internal dispatch sets up, which no
standard external call can replicate, so this replaces that dependency
entirely with an independent implementation:

- **Real depth, not estimated.** Hooks `IDirect3D9::CreateDevice` (shared
  vtable trick) to reach the real device, then swaps in its own
  `D3DFMT_INTZ`-format depth texture via `SetDepthStencilSurface` — the
  same depth-buffer-as-texture trick ReShade and countless D3D9 mods have
  used for over a decade — so the stereo shift is computed from the
  game's actual per-pixel depth.
- **Composites in `Present`, not `EndScene`.** Some engines call
  `EndScene` more than once per displayed frame (an offscreen pass, or a
  separate late pass some games use just to draw their own cursor
  sprite); compositing there meant whatever ran after us — including a
  game's own cursor — landed on the backbuffer un-split. `Present` fires
  exactly once per displayed frame, after everything else, so that's
  where the actual capture + Half-SBS composite happens now (wrapped in
  its own `BeginScene`/`EndScene` pair).
- **Handles `Reset()`.** Releases/rebinds its own D3DPOOL_DEFAULT
  resources around the game's device `Reset()` calls (window size/mode
  changes) — otherwise a still-bound custom depth surface makes `Reset()`
  fail outright, silently breaking every subsequent D3D9 draw call
  (menus, HUD, loading screens) while anything that bypasses the device
  (like an intro video) keeps working, which looks exactly like "only
  the video plays, no UI at all."
- **Forces fixed-function state for its own draw.** A vertex shader left
  bound from the game's last draw call silently overrides FVF for any
  pretransformed quad — `DrawPrimitive` still reports success either
  way, it just draws nothing visible. Explicitly clears the vertex
  shader (and cull/scissor/stencil/alpha-test state) before drawing.
- **Cursor handling.** The real mouse cursor is a single, unsplit overlay
  that has no idea the frame is now Half-SBS. Every known way to hide it
  turned out to be a no-op for at least one tested game — Win32
  `ShowCursor`, the D3D9 device's own `ShowCursor`, even subclassing the
  window to force `SetCursor(NULL)` on `WM_SETCURSOR` — so it falls back
  to replacing the shared system cursor resource itself
  (`SetSystemCursor` on `OCR_NORMAL`), which works regardless of which
  API/thread/window the game uses, and is restored on clean exit. A
  small marker is drawn into both halves at the mouse's real,
  squished-and-shifted position so the cursor stays usable.
- **Runtime tuning.** `Ctrl+F3`/`Ctrl+F4` step separation down/up,
  `Ctrl+F5`/`Ctrl+F6` step convergence down/up — the same key scheme as
  TriDef's own Ignition driver. Values persist across restarts in
  `hook_dll/tridef_d3d9_hook_config.ini`.

This repo also has a matching from-scratch DXGI/D3D11 `Present` hook
(`hook_dll/hook.cpp`) that proves out the same vtable-hooking + depth
capture approach for D3D11, as a profile-free alternative for games with
no TriDef profile — it's an earlier, less complete proof of concept, not
what actually runs for the 64-bit path (that still uses TriDef's own,
already-correct D3D11 rendering via its real DLLs).

## Legal note

This is an interoperability tool for software you already own: it loads
DLLs from your own existing, licensed TriDef 3D installation into
processes on your own machine. It does not crack, patch, or redistribute
TriDef's binaries, and does not circumvent any license/activation check.
Standard disclaimer applies: use at your own risk, on software you're
entitled to run.

## License

MIT for the code in this repository (see [LICENSE](LICENSE)). This does
not extend to TriDef 3D itself, which remains the property of its
original owner and is not included here.
