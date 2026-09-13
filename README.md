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
  well-understood technique used by tools like ReShade and 3DMigoto —
  loading TriDef's own real `TriDefIgnition64.dll`, `TriDefD3D1164.dll`
  and `TriDefDXGI64.dll` from wherever the user's existing TriDef 3D
  install already has them.

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
wrapper that keeps the console window open afterward.

Running from source instead of the prebuilt exe:

```
python play3d.py "Gone Home"
python play3d.py "Gone Home" --dry-run   # show what it would do, don't launch
```

## Building `Play3D.exe` yourself

```
pip install pyinstaller
pyinstaller --onefile --console --name Play3D play3d.py
```

## `hook_dll/` — bonus: a from-scratch stereo DXGI hook

Before landing on "just fix the delivery of TriDef's own DLLs," this repo
also grew a small experimental DXGI/D3D11 `Present` hook written from
scratch in C++ (`hook_dll/hook.cpp`), built entirely on public, documented
Win32/D3D11 APIs, with no TriDef code involved. It proves out vtable
hooking + depth-buffer capture (forcing `D3D11_BIND_SHADER_RESOURCE` onto
depth textures via a `CreateTexture2D` hook, tracking the active
depth-stencil via an `OMSetRenderTargets` hook) as a generic,
profile-free alternative for games with no TriDef profile. It's not a
finished stereo renderer — see the comments in `hook.cpp` for exactly
how far it got — but it's a working, documented starting point for
anyone who wants to take that route instead.

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
