# TriDef 3D Injector (Windows 11 fix)

A replacement launcher/injector for **TriDef 3D Ignition** — the stereoscopic-3D
driver by DDD (defunct since ~2017) — that works reliably on modern Windows 11
and modern games, without touching or redistributing any of TriDef's own
binaries.

Supports **DirectX 9 (32-bit) and DirectX 11 (64-bit) games**, using TriDef's
own real rendering DLLs for both. See [DirectX 9 support](#directx-9-support)
for the one non-obvious trick that makes D3D9 work.

## What this actually solves: the Ignition pop-up

If you press **Play** in TriDef 3D Ignition on Windows 11, you get a dialog
saying the game *"did not use Direct3D 9, 10 or 11"*, and the game runs in
plain 2D. That pop-up is the whole reason this project exists.

**This tool is a way around that pop-up.** It bypasses Ignition's broken
launch path entirely: you still register the game in Ignition once (so its
per-game 3D profile is available), but you launch through
`Tridef3D_Play.exe` instead of Ignition's Play button. The injection is
done from scratch, against the correct process, so TriDef's real DLLs hook
the game and you get stereo 3D with no dialog at all.

The pop-up isn't a licensing or compatibility check — it's a post-mortem
report. Ignition watched the wrong process (a throwaway `steam.exe`), saw
no Direct3D in it, and told you so. Details in
[Why this exists](#why-this-exists).

## Quick start

1. Install [TriDef 3D](https://en.wikipedia.org/wiki/TriDef_3D) (your own
   existing install/license — this project doesn't provide one).
2. Add the game once in TriDef 3D Ignition's own UI (you never need to
   actually launch it from there — in fact, don't; that's the pop-up).
3. Run `Tridef3D_Play.exe` (as Administrator) — from
   [Releases](../../releases), no Python required.

```
Tridef3D_Play.exe
```

With no arguments it lists every Steam game you've registered in TriDef 3D
Ignition, with the one you most recently selected as the default — pick a
number, or just press Enter. Or name the game directly:

```
Tridef3D_Play.exe "Left 4 Dead"
```

`Tridef3D_Play.exe` is a single self-contained executable — put it anywhere
and run it, nothing to install. `Tridef3D_Play.bat` is just a
double-click-friendly wrapper. Works identically for 32-bit or 64-bit games
— the right DLL set is picked automatically. The only extra file is
`Inject32.exe`, which must sit in the same folder **for 32-bit games only**
(see [How it works](#how-it-works)).

## Game profiles: what you get with and without one

TriDef shipped ~800 hand-tuned per-game profiles, and the profile is what
produces real depth:

- **With a matching profile** — TriDef reconstructs proper stereo geometry.
  Real depth, correct separation and convergence, adjustable with TriDef's
  own hotkeys.
- **Without a profile** — the image is still split side-by-side, but it's a
  flat SBS pair with no actual depth. Both halves are effectively the same
  view. It *looks* like 3D output to a display, but there's nothing to see
  in stereo.

So if a game injects fine yet looks flat, the injection isn't the problem —
that title has no TriDef profile.

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
   3D hook ends up watching Steam itself, not the game. (Confirmed
   directly: logging the actual argument Ignition passes shows exactly
   one PID, and it's steam.exe's. This is the source of the
   "this game did not use Direct3D 9, 10 or 11" pop-up — it watched the
   wrong process, which indeed never used Direct3D.)
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
binary. Ignition's own "Play" button remains broken — always launch
through this tool.

## How it works

- **Launch `steam.exe` directly, with two extra flags:**
  `steam.exe -no-browser -no-cef-sandbox steam://rungameid/<id>`. Going
  through the `steam://` protocol is what makes Steamworks games
  initialize normally instead of detecting "not launched by Steam" and
  silently relaunching themselves. The two flags disable Steam's embedded
  Chromium browser for that launch — required for DirectX 9, harmless for
  DirectX 11 (see below). The shell's `steam://` URI handler can't pass
  flags through to `steam.exe`, so it's invoked directly.
- **Poll for the new process at native speed** (a tight loop over
  `CreateToolhelp32Snapshot`) and inject the instant it appears. No
  thread suspension is used: freezing a brand-new process's only thread
  is very likely to freeze it *while it holds its own loader lock*, which
  would deadlock the injection thread forever. Plain speed avoids that.
- **Standard `LoadLibraryW` + `CreateRemoteThread` injection** — the same
  well-understood technique used by tools like ReShade and 3DMigoto —
  loading TriDef's own real DLLs from wherever the user's existing TriDef
  3D install already has them:
  - **64-bit games:** `TriDefIgnition64.dll` + `TriDefD3D1164.dll` +
    `TriDefDXGI64.dll` (DirectX 11)
  - **32-bit games:** `TriDefIgnition.dll` + `TriDefD3D9.dll` (DirectX 9)

  For 32-bit targets, a 64-bit process can't correctly resolve
  `LoadLibraryW`'s address inside a 32-bit (WOW64) target, so the actual
  injection step is delegated to the bundled `Inject32.exe` helper.
- The tool also writes the same two flags back into Ignition's own
  per-game `Argument` registry value, so the setting is visible in
  Ignition's game Properties dialog and survives if you ever do use
  Ignition directly.

This repo contains **no TriDef binaries** and never copies or bundles
them — it only calls `LoadLibraryW` on paths already present from the
user's own licensed install (read from the registry TriDef itself
writes at `HKLM\SOFTWARE\WOW6432Node\DDD`). You need TriDef 3D installed
yourself for any of this to do anything.

## DirectX 9 support

TriDef's D3D9 DLL, loaded with a plain `LoadLibraryW`, sits inert — it
never hooks anything, and the game runs in plain 2D. That looked like a
dead end for a long time (its activation path seemed to need internal
state only TriDef's own injector stub sets up).

The fix comes from a community tip (the "Getting TriDef working with
Steam games" thread on the MTBS3D forums): launch Steam with
`-no-browser -no-cef-sandbox`. With those flags on the `steam.exe` call
that starts the game, `TriDefIgnition.dll` + `TriDefD3D9.dll` hook
correctly and D3D9 games render in stereo. Confirmed live against
Left 4 Dead (Source engine) and The Cave (different engine) — the
effect isn't engine-specific. Why Steam's embedded browser interferes with TriDef's D3D9
activation isn't understood — but the effect is reproducible, and the
flags are harmless for D3D11 games, so they're always applied.

**Load order matters.** `TriDefIgnition.dll` must finish initialising
before `TriDefD3D9.dll` is loaded; the D3D9 DLL loaded first sits inert
for good. An earlier version of the 32-bit helper fired both
`LoadLibraryW` remote threads at once to save time, which made that order
random — the same command then worked about one launch in three on
Left 4 Dead, and nothing about Steam flags or profiles changed it. The
bundled `Inject32.exe` is now a 4 KB native helper
([native/inject32.cpp](native/inject32.cpp)) that loads the DLLs strictly
in the order given, one at a time. The 64-bit in-process path always did
that, which is why DirectX 11 never showed the problem.

**One API set per process.** Do not inject the D3D9 and D3D11 sets
together. `TriDefIgnition(64).dll` is shared state for both; loading both
hook DLLs at once makes TriDef report "this game did not use Direct3D"
and render nothing — reproduced on Gone Home. That's why the DLL set is
chosen by bitness rather than injecting everything and letting the game
pick.

**Current limitation:** the set is picked by exe bitness — 32-bit gets
the D3D9 set, 64-bit gets D3D11. A 32-bit DirectX 11 game isn't covered
yet (no such title was available to test against). Static detection of
which API a game uses doesn't work: Unity and Source both load their
Direct3D backend dynamically at runtime rather than importing it.

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
  prebuilt `Tridef3D_Play.exe` from [Releases](../../releases)

## Running from source

```
python play3d.py "Left 4 Dead"
python play3d.py "Left 4 Dead" --dry-run   # show what it would do, don't launch
```

## Building it yourself

```
pip install pyinstaller
pyinstaller build/Tridef3D_Play.spec --distpath .
powershell -ExecutionPolicy Bypass -File native/build_inject32.ps1   # Inject32.exe (needs MSVC x86 tools)
```

`Inject32.exe` is header-free and CRT-free C++, so it only needs the
MSVC x86 cross compiler plus `kernel32.lib`/`shell32.lib` import
libraries; edit the two paths at the top of the build script.

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
