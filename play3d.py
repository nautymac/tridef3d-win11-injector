"""
One-command TriDef 3D launcher.

Usage:
    python play3d.py "Gone Home"
    python play3d.py "Gone Home" --dry-run     (just show what it would do)

What it does automatically:
    1. Reads HKCU\\SOFTWARE\\DDD\\TriDefIgnition\\Games\\<name> to get the
       steam://rungameid/<appid> the game was registered with in TriDef
       Ignition.
    2. Finds the Steam library (via HKCU\\SOFTWARE\\Valve\\Steam) and parses
       libraryfolders.vdf + appmanifest_<appid>.acf to locate the game's
       install folder and guess its main .exe.
    3. Launches the game via the proper steam:// protocol (NOT by running
       the exe directly - direct launches make Steamworks games detect
       "not launched by Steam" and silently relaunch themselves via the
       already-running steam.exe, which then falls outside any process
       tree we could otherwise catch).
    4. Polls for that new process appearing (sub-millisecond loop, no
       thread suspension - suspending would deadlock LoadLibraryW on the
       new process's own loader lock) and, the instant it's seen, injects
       TriDef's real D3D11/DXGI/orchestrator DLLs before the game's own
       D3D device gets created.

See injector.py for the underlying primitives and why each design choice
here avoids the failure modes that make TriDefInjector64.exe itself (and
several earlier, simpler approaches) unreliable on modern Windows/games.
"""
import sys
import os
import re
import glob
import time
import winreg

try:
    sys.stdout.reconfigure(encoding='utf-8')
except Exception:
    pass

def get_app_dir():
    """Folder to treat as 'where this program lives' - the real exe's
    folder when frozen by PyInstaller (sys.executable), not the temp
    extraction dir onefile mode unpacks into (which is what __file__
    would point to and gets wiped after every run)."""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


sys.path.insert(0, get_app_dir())
import injector

def get_tridef_driver_dir():
    """TriDef's own install path, read from the registry instead of
    hardcoded - so this works on any PC with TriDef installed, regardless
    of where its installer put it."""
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\DDD") as k:
        install_apps = winreg.QueryValueEx(k, "InstallApps")[0]
    return os.path.join(install_apps, "TriDef", "TriDefIgnition", "Driver")


def get_standard_dlls(is_64bit):
    """Exactly the two DLL sets that were confirmed working live, and
    nothing more:

    - 32-bit: TriDefIgnition.dll + TriDefD3D9.dll (Left 4 Dead, DX9).
      ORDER MATTERS: Ignition first, then D3D9, loaded one at a time.
      TriDefD3D9.dll initialised before its Ignition counterpart stays
      inert - that (via a parallel-loading helper) was the real reason
      DX9 looked flaky for so long, not the Steam flags in play3d().
    - 64-bit: TriDefIgnition64.dll + TriDefD3D1164.dll + TriDefDXGI64.dll
      (Gone Home, DX11).

    Do NOT inject the D3D9 and D3D11 sets together: TriDefIgnition(64).dll
    is shared state for both, and loading both hook DLLs at once made
    TriDef throw its "this game didn't use Direct3D" warning and render
    nothing - reproduced on Gone Home. One API set per process."""
    driver_dir = get_tridef_driver_dir()
    if is_64bit:
        return [
            os.path.join(driver_dir, "TriDefIgnition64.dll"),
            os.path.join(driver_dir, "TriDefD3D1164.dll"),
            os.path.join(driver_dir, "TriDefDXGI64.dll"),
        ]
    return [
        os.path.join(driver_dir, "TriDefIgnition.dll"),
        os.path.join(driver_dir, "TriDefD3D9.dll"),
    ]


IMAGE_FILE_MACHINE_I386 = 0x014c
IMAGE_FILE_MACHINE_AMD64 = 0x8664


def is_64bit_exe(exe_path):
    """Read just enough of the PE header to get Machine type - avoids
    depending on the third-party 'pefile' package so the frozen exe
    doesn't need it bundled."""
    with open(exe_path, "rb") as f:
        f.seek(0x3C)
        pe_offset = int.from_bytes(f.read(4), "little")
        f.seek(pe_offset + 4)  # skip "PE\0\0"
        machine = int.from_bytes(f.read(2), "little")
    if machine == IMAGE_FILE_MACHINE_AMD64:
        return True
    if machine == IMAGE_FILE_MACHINE_I386:
        return False
    raise ValueError(f"unrecognized Machine type 0x{machine:x} in {exe_path}")

# Helper/launcher processes to never treat as "the game" when guessing
# which .exe in an install folder is the real one.
EXE_EXCLUDE_PATTERNS = [
    "crashpad_handler", "unitycrashhandler", "crashreport", "crashhandler",
    "installer", "uninstall", "unins", "redist", "vcredist", "dxsetup",
    "helper", "launcher", "updater", "battleye", "easyanticheat", "eac",
    # Pre-game option/launcher windows. Binary Domain ships
    # BinaryDomainConfiguration.exe, which Steam starts first; the real
    # BinaryDomain.exe is only spawned when you press Start in it.
    "configuration", "config", "settings", "options", "setup",
]


def _norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


def get_steam_path():
    for hive, subkey in [(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam"),
                          (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Valve\Steam")]:
        try:
            with winreg.OpenKey(hive, subkey) as k:
                for val_name in ("InstallPath", "SteamPath"):
                    try:
                        return winreg.QueryValueEx(k, val_name)[0]
                    except FileNotFoundError:
                        continue
        except FileNotFoundError:
            continue
    return None


def list_all_library_folders(steam_path):
    """Every Steam library folder (main install + any added via Steam's
    'Storage Manager'), needed to search for a game's appmanifest when we
    don't already know which library it's in."""
    folders = [steam_path]
    vdf_path = os.path.join(steam_path, "steamapps", "libraryfolders.vdf")
    try:
        with open(vdf_path, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
        for m in re.finditer(r'"path"\s*"([^"]+)"', content):
            lib_path = m.group(1).replace("\\\\", "\\")
            if lib_path not in folders:
                folders.append(lib_path)
    except FileNotFoundError:
        pass
    return folders


def find_appid_by_installdir(steam_path, installdir_name):
    """Reverse-lookup a Steam appid from an install folder name by scanning
    every library's appmanifest_*.acf files for a matching "installdir"."""
    for lib in list_all_library_folders(steam_path):
        for manifest_path in glob.glob(os.path.join(lib, "steamapps", "appmanifest_*.acf")):
            try:
                with open(manifest_path, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
            except OSError:
                continue
            m_dir = re.search(r'"installdir"\s*"([^"]+)"', content)
            if m_dir and m_dir.group(1) == installdir_name:
                m_id = re.search(r'"appid"\s*"(\d+)"', content)
                if m_id:
                    return m_id.group(1)
    return None


def get_tridef_game_appid(game_name):
    key_path = rf"SOFTWARE\DDD\TriDefIgnition\Games\{game_name}"
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as k:
        argument = winreg.QueryValueEx(k, "Argument")[0]
        m = re.search(r"rungameid/(\d+)", argument)
        if m:
            return m.group(1)
        # Ignition doesn't always record a steam:// launch URI - some
        # profiles just store a direct Path to the exe, even for actual
        # Steam games (their install folder ends up under
        # steamapps\common\<name> regardless). Fall back to reverse-
        # looking-up the appid from that folder name via Steam's own
        # appmanifest files, so those games work too instead of just
        # being invisible/unlaunchable.
        try:
            exe_path = winreg.QueryValueEx(k, "Path")[0]
        except FileNotFoundError:
            exe_path = None
    if not exe_path:
        raise ValueError(f"no rungameid in Argument={argument!r} and no Path to fall back to")
    installdir_name = os.path.basename(os.path.dirname(exe_path))
    steam_path = get_steam_path()
    if not steam_path:
        raise ValueError("could not find Steam install path for installdir fallback")
    appid = find_appid_by_installdir(steam_path, installdir_name)
    if not appid:
        raise ValueError(
            f"couldn't find a steam appid for {game_name!r}: no rungameid in "
            f"Argument={argument!r}, and no appmanifest matches installdir "
            f"{installdir_name!r} (from Path={exe_path!r})")
    return appid


def sync_ignition_argument_with_fix(game_name, appid):
    """Writes -no-browser -no-cef-sandbox steam://rungameid/<appid> back
    into Ignition's own Argument value for this game, so TriDef 3D
    Ignition's own "Play" button also launches with the fix applied - not
    just runs launched through this tool. Best-effort: this is a nice-to-
    have, not something play3d() should fail over if the registry key is
    missing/unwritable for some reason."""
    fixed_argument = f"-no-browser -no-cef-sandbox steam://rungameid/{appid}"
    key_path = rf"SOFTWARE\DDD\TriDefIgnition\Games\{game_name}"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path, 0, winreg.KEY_SET_VALUE | winreg.KEY_QUERY_VALUE) as k:
            try:
                current = winreg.QueryValueEx(k, "Argument")[0]
            except FileNotFoundError:
                current = None
            if current != fixed_argument:
                winreg.SetValueEx(k, "Argument", 0, winreg.REG_SZ, fixed_argument)
                print(f"synced Ignition's own Argument for {game_name!r} -> {fixed_argument}")
    except OSError as e:
        print(f"(couldn't sync Ignition's Argument for {game_name!r}: {e})")
    set_ignition_last_game(game_name)


def set_ignition_last_game(game_name):
    """TriDefIgnition.dll, once inside the game, finds its per-game profile
    by reading Games\\<LastGameName>\\Profile - it has no other way of
    knowing which game it was injected into. Ignition's UI writes
    LastGameName whenever a game is selected there; we're launching
    without the UI, so write it ourselves or the game gets whichever
    profile was last clicked in Ignition."""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"SOFTWARE\DDD\TriDefIgnition\Games",
                            0, winreg.KEY_SET_VALUE) as k:
            winreg.SetValueEx(k, "LastGameName", 0, winreg.REG_SZ, game_name)
    except OSError as e:
        print(f"(couldn't set LastGameName to {game_name!r}: {e})")


def find_library_folder_for_app(steam_path, appid):
    vdf_path = os.path.join(steam_path, "steamapps", "libraryfolders.vdf")
    with open(vdf_path, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()

    # naive per-block scan: each numbered block has a "path" and an "apps" map
    blocks = re.split(r'"\d+"\s*\n\s*{', content)[1:]
    for block in blocks:
        path_m = re.search(r'"path"\s*"([^"]+)"', block)
        if not path_m:
            continue
        lib_path = path_m.group(1).replace("\\\\", "\\")
        if re.search(rf'"{appid}"\s*"', block):
            return lib_path
    return steam_path  # fall back to the main steam install


def guess_main_exe(install_dir):
    """Pick the real game executable out of the install folder.

    Order of preference:
      1. an exe whose name matches the install folder's name once both are
         stripped to [a-z0-9] ("Binary Domain" -> BinaryDomain.exe), and
         which isn't on the exclude list;
      2. the largest exe not on the exclude list;
      3. the largest exe of any kind (fallback if the list ate everything).

    Excluded-but-present exes are printed so a wrong guess is visible in
    the log rather than silent."""
    exes = glob.glob(os.path.join(install_dir, "*.exe"))
    if not exes:
        return None
    folder_key = _norm(os.path.basename(install_dir.rstrip("\\/")))

    candidates, skipped = [], []
    for exe in exes:
        base = os.path.basename(exe).lower()
        (skipped if any(pat in base for pat in EXE_EXCLUDE_PATTERNS) else candidates).append(exe)
    if skipped:
        print("Ignoring launcher/helper exes: " + ", ".join(os.path.basename(s) for s in skipped))
    if not candidates:
        candidates = exes

    by_name = [c for c in candidates
               if folder_key and _norm(os.path.splitext(os.path.basename(c))[0]) == folder_key]
    if by_name:
        return by_name[0]
    # otherwise prefer the largest exe (helper/updater stubs are usually tiny)
    candidates.sort(key=lambda p: os.path.getsize(p), reverse=True)
    return candidates[0]


def play3d(game_name, dry_run=False, steam_flags=True):
    print(f"=== TriDef 3D launch: {game_name} ===")

    appid = get_tridef_game_appid(game_name)
    print(f"Steam AppID: {appid}")
    if steam_flags:
        print("(steam-flags mode: launching steam.exe with -no-browser -no-cef-sandbox)")
        sync_ignition_argument_with_fix(game_name, appid)
    else:
        set_ignition_last_game(game_name)

    steam_path = get_steam_path()
    if not steam_path:
        print("ERROR: could not find Steam install path")
        return False
    print(f"Steam path: {steam_path}")

    lib_path = find_library_folder_for_app(steam_path, appid)
    manifest_path = os.path.join(lib_path, "steamapps", f"appmanifest_{appid}.acf")
    installdir_name = None
    if os.path.exists(manifest_path):
        # Steam rewrites the manifest right after a game exits and holds it
        # exclusively for a moment; relaunching immediately used to die with
        # PermissionError here. Retry briefly instead.
        for attempt in range(10):
            try:
                with open(manifest_path, "r", encoding="utf-8", errors="ignore") as f:
                    m = re.search(r'"installdir"\s*"([^"]+)"', f.read())
                    if m:
                        installdir_name = m.group(1)
                break
            except PermissionError:
                time.sleep(0.3)
    if not installdir_name:
        print(f"ERROR: could not read installdir from {manifest_path}")
        return False

    install_dir = os.path.join(lib_path, "steamapps", "common", installdir_name)
    print(f"Install dir: {install_dir}")

    exe_path = guess_main_exe(install_dir)
    if not exe_path:
        print(f"ERROR: no .exe found in {install_dir}")
        return False
    image_name = os.path.basename(exe_path)
    print(f"Detected main exe: {image_name}")

    is_64bit = is_64bit_exe(exe_path)
    print(f"Architecture: {'64-bit' if is_64bit else '32-bit'}")

    steam_exe = os.path.join(steam_path, "steam.exe")
    launch_uri = f"steam://rungameid/{appid}"

    standard_dlls = get_standard_dlls(is_64bit)
    print(f"DLLs to inject:")
    for d in standard_dlls:
        exists = "OK" if os.path.exists(d) else "MISSING!"
        print(f"  [{exists}] {d}")

    injector_python = None
    if not is_64bit:
        # A 64-bit process can't correctly compute LoadLibraryW's address
        # inside a 32-bit (WOW64) target - the bitness of the injecting
        # process has to match. Hand the actual injection step off to the
        # bundled 32-bit helper.
        helper = os.path.join(get_app_dir(), "Inject32.exe")
        if not os.path.exists(helper):
            print(f"ERROR: {image_name} is 32-bit but Inject32.exe is missing from {get_app_dir()}")
            return False
        injector_python = helper
        print(f"32-bit target -> delegating injection to: {helper}")

    # Launch Steam directly with these two flags ahead of the steam://
    # URI, instead of going through the shell's steam:// protocol handler
    # (which can't pass flags through to steam.exe at all). The flags
    # disable Steam's embedded CEF browser; they came from a forum tip
    # picked up while chasing the D3D9 problem. The actual D3D9 fix turned
    # out to be DLL load order (see get_standard_dlls) and a controlled
    # run without the flags works too. Default is therefore the plain
    # steam:// URI through the shell handler; --steam-flags (or the
    # Tridef3D_Play_steamflags build) is kept as a backup that launches
    # steam.exe directly with the flags, exactly as every earlier verified
    # run did.
    if steam_flags:
        launch_cmd = [steam_exe, "-no-browser", "-no-cef-sandbox", launch_uri]
        launch_uri_for_shell = None
        print(f"Launch: {' '.join(launch_cmd)}")
    else:
        launch_cmd = None
        launch_uri_for_shell = launch_uri
        print(f"Launch: start {launch_uri}")

    if dry_run:
        print("(dry run - not actually launching)")
        return True

    # Long timeout on purpose: games with a pre-game configuration/launcher
    # window (Binary Domain) don't start the real exe until the user clicks
    # Start in it, and that can take a while.
    pid = injector.poll_launch_and_inject(image_name, launch_uri_for_shell, standard_dlls,
                                           timeout_s=180,
                                           injector_python=injector_python,
                                           launch_cmd=launch_cmd)
    if pid:
        print(f"\nSUCCESS: {game_name} running as PID {pid} with TriDef 3D injected.")
        return True
    else:
        print(f"\nFAILED: never saw {image_name} appear.")
        return False


LAST_USED_PATH = os.path.join(get_app_dir(), "last_game.txt")


def list_registered_games():
    """Every game TriDef 3D Ignition knows about (HKCU\\...\\Games\\<name>),
    filtered to ones that look like Steam games - either a rungameid
    Argument, or (some Ignition profiles only record this) a Path that
    lands under a Steam library's steamapps\\common - so we don't list
    stale/non-Steam entries we can't launch. get_tridef_game_appid()
    handles resolving the appid for either case."""
    games = []
    key_path = r"SOFTWARE\DDD\TriDefIgnition\Games"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as parent:
            i = 0
            while True:
                try:
                    name = winreg.EnumKey(parent, i)
                except OSError:
                    break
                i += 1
                if name == "LastGameName":  # a value, not a subkey, but just in case
                    continue
                try:
                    with winreg.OpenKey(parent, name) as gk:
                        argument = ""
                        try:
                            argument = winreg.QueryValueEx(gk, "Argument")[0]
                        except FileNotFoundError:
                            pass
                        path = ""
                        try:
                            path = winreg.QueryValueEx(gk, "Path")[0]
                        except FileNotFoundError:
                            pass
                    if "rungameid" in argument or "steamapps" in path.lower():
                        games.append(name)
                except OSError:
                    continue
    except FileNotFoundError:
        pass
    return games


def get_ignition_last_game_name():
    """TriDef Ignition writes its own "last selected game" value
    (HKCU\\...\\Games\\LastGameName) every time you add or select a game
    in its own UI - so right after registering a new game there, this
    already points at it. Used to skip straight to launching that game
    instead of making the user separately pick it again from our list."""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"SOFTWARE\DDD\TriDefIgnition\Games") as k:
            name = winreg.QueryValueEx(k, "LastGameName")[0]
    except (FileNotFoundError, OSError):
        return None
    return name if name in list_registered_games() else None


def load_last_used():
    if os.path.exists(LAST_USED_PATH):
        with open(LAST_USED_PATH, "r", encoding="utf-8") as f:
            return f.read().strip()
    return None


def save_last_used(game_name):
    with open(LAST_USED_PATH, "w", encoding="utf-8") as f:
        f.write(game_name)


def prompt_for_game_name():
    games = list_registered_games()
    if not games:
        return input('TriDef 3D Ignition에 등록된 게임이 없습니다. 게임 이름을 직접 입력하세요: ').strip()

    # Prefer Ignition's own "last selected" value as the suggested
    # default, falling back to whatever this tool itself last launched.
    # Only a *suggestion*, never auto-launched without confirmation - just
    # opening a game's Properties dialog in Ignition updates its
    # LastGameName as a side effect, so trusting it blindly used to
    # launch the wrong game after nothing more than glancing at settings.
    default_name = get_ignition_last_game_name() or load_last_used()
    if default_name and default_name in games:
        games.remove(default_name)
        games.insert(0, default_name)

    print("TriDef에 등록된 게임:")
    for i, name in enumerate(games, 1):
        marker = " (최근 선택됨)" if i == 1 and default_name == name else ""
        print(f"  {i}) {name}{marker}")
    print()
    choice = input(f"번호 선택 (엔터 = 1): ").strip()
    if not choice:
        return games[0]
    if choice.isdigit() and 1 <= int(choice) <= len(games):
        return games[int(choice) - 1]
    return choice  # allow typing a name not in the list too


def main(default_steam_flags=False):
    dry_run = '--dry-run' in sys.argv
    if '--steam-flags' in sys.argv:
        steam_flags = True
    elif '--no-steam-flags' in sys.argv:
        steam_flags = False
    else:
        steam_flags = default_steam_flags
    positional = [a for a in sys.argv[1:] if a not in ('--dry-run', '--steam-flags', '--no-steam-flags')]

    if positional:
        game_name = positional[0]
    else:
        game_name = prompt_for_game_name()

    if not game_name:
        print('게임 이름이 필요합니다. 예: python play3d.py "Gone Home"')
        sys.exit(1)

    ok = play3d(game_name, dry_run=dry_run, steam_flags=steam_flags)
    if ok and not dry_run:
        save_last_used(game_name)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
