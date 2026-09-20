# TriDef 3D Injector (Windows 11 fix, DirectX 11)

A replacement launcher/injector for **TriDef 3D Ignition** — the stereoscopic-3D
driver by DDD (defunct since ~2017) — that works reliably on modern Windows 11
and modern games, without touching or redistributing any of TriDef's own
binaries.

Supports **DirectX 11 games, 32-bit and 64-bit**. DirectX 9 was investigated
and dropped — see [Why no DirectX 9?](#why-no-directx-9) below.

## Quick start

1. Install [TriDef 3D](https://en.wikipedia.org/wiki/TriDef_3D) (your own
   existing install/license — this project doesn't provide one).
2. Add the game once in TriDef 3D Ignition's own UI (you never need to
   actually launch it from there).
3. Run `Tridef3D_Dx11_Play.exe` (as Administrator) — from
   [Releases](../../releases), no Python required.

```
Tridef3D_Dx11_Play.exe
```

With no arguments it lists every Steam game you've registered in TriDef 3D
Ignition — pick a number, or press Enter to relaunch whatever you ran last
time. Or name the game directly:

```
Tridef3D_Dx11_Play.exe "Gone Home"
```

`Tridef3D_Dx11_Play.bat` is a double-click-friendly wrapper. Works
identically for 32-bit or 64-bit games — the right DLL set is picked
automatically.

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
   3D hook ends up injected into Steam itself, not the game. (Confirmed
   directly: logging the actual argument Ignition passes shows exactly
   one PID, and it's steam.exe's, not the game's.)
2. **Naive memory search.** The injector allocates memory for its payload
   by linearly scanning *upward only* from the target module's base
   address. Games in 2016 (when this was last updated) loaded a few dozen
   DLLs; a modern game can load 100+, often filling the address space all
   the way to the top — so the search comes up empty and injection fails
   silently (exit code 4). A follow-up attempt to just binary-patch this
   search logic hit a deeper wall: the offset math is 32-bit-truncated, so
   the allocated memory has to land *near* the target module regardless of
   how the search itself is tuned — not fixable without emitting different
   machine code entirely.

Reverse-engineering both of these led to a small, from-scratch injector
that sidesteps both problems entirely instead of patching the 2016-era
binary.

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
  loading TriDef's own real D3D11/DXGI DLLs from wherever the user's
  existing TriDef 3D install already has them (`TriDefIgnition64.dll` +
  `TriDefD3D1164.dll` + `TriDefDXGI64.dll` for 64-bit games,
  `TriDefIgnition.dll` + `TriDefD3D11.dll` + `TriDefDXGI.dll` for 32-bit).
  For 32-bit targets, a 64-bit process can't correctly resolve
  `LoadLibraryW`'s address inside a 32-bit (WOW64) target, so the actual
  injection step is delegated to the bundled `Inject32.exe` helper, which
  matches the target's bitness.

This repo contains **no TriDef binaries** and never copies or bundles
them — it only calls `LoadLibraryW` on paths already present from the
user's own licensed install (read from the registry TriDef itself
writes at `HKLM\SOFTWARE\WOW6432Node\DDD`). You need TriDef 3D installed
yourself for any of this to do anything.

## Why no DirectX 9?

DirectX 9 support was built and worked as a from-scratch, non-TriDef
depth-based stereo hook, but was dropped from this project: it doesn't use
TriDef's actual rendering, just a substitute for it, which wasn't the
point of reviving TriDef here.

Getting *TriDef's own* D3D9 rendering to activate turned out to be a dead
end. Its activation entry point (`TriDef3DSDKFunc`, in `TriDefD3D9.dll`)
needs an explicit call with an undocumented register value that only
TriDef's own injector stub sets up — a plain `LoadLibraryW` load leaves
the DLL loaded but inert, and calling the entry point directly from
outside code crashes identically regardless of calling convention.
Confirmed this isn't fixable by improving the launch mechanism either:
even TriDef 3D Ignition's own "Play" button, using its own real
`TriDefInjector.exe`/`TriDefInjector64.exe`, fails to activate D3D9 the
same way — the problem is upstream in TriDef's own D3D9 activation path,
not in how any injector (including TriDef's own) calls it.

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
  prebuilt `Tridef3D_Dx11_Play.exe` from [Releases](../../releases)

## Running from source

```
python play3d.py "Gone Home"
python play3d.py "Gone Home" --dry-run   # show what it would do, don't launch
```

## Building it yourself

```
pip install pyinstaller
pyinstaller build/Tridef3D_Dx11_Play.spec --distpath .
```

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
