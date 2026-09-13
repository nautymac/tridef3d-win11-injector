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


def get_standard_dlls():
    driver_dir = get_tridef_driver_dir()
    return [
        os.path.join(driver_dir, "TriDefIgnition64.dll"),
        os.path.join(driver_dir, "TriDefD3D1164.dll"),
        os.path.join(driver_dir, "TriDefDXGI64.dll"),
    ]

# Helper/launcher processes to never treat as "the game" when guessing
# which .exe in an install folder is the real one.
EXE_EXCLUDE_PATTERNS = [
    "crashpad_handler", "unitycrashhandler", "installer", "uninstall",
    "redist", "vcredist", "dxsetup", "helper", "launcher", "updater",
    "battleye", "easyanticheat", "eac",
]


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


def get_tridef_game_appid(game_name):
    key_path = rf"SOFTWARE\DDD\TriDefIgnition\Games\{game_name}"
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key_path) as k:
        argument = winreg.QueryValueEx(k, "Argument")[0]
    m = re.search(r"rungameid/(\d+)", argument)
    if not m:
        raise ValueError(f"couldn't find a steam appid in Argument={argument!r}")
    return m.group(1)


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
    exes = glob.glob(os.path.join(install_dir, "*.exe"))
    candidates = []
    for exe in exes:
        base = os.path.basename(exe).lower()
        if any(pat in base for pat in EXE_EXCLUDE_PATTERNS):
            continue
        candidates.append(exe)
    if not candidates:
        candidates = exes
    if not candidates:
        return None
    # prefer the largest exe (helper/updater stubs are usually tiny)
    candidates.sort(key=lambda p: os.path.getsize(p), reverse=True)
    return candidates[0]


def play3d(game_name, dry_run=False):
    print(f"=== TriDef 3D launch: {game_name} ===")

    appid = get_tridef_game_appid(game_name)
    print(f"Steam AppID: {appid}")

    steam_path = get_steam_path()
    if not steam_path:
        print("ERROR: could not find Steam install path")
        return False
    print(f"Steam path: {steam_path}")

    lib_path = find_library_folder_for_app(steam_path, appid)
    manifest_path = os.path.join(lib_path, "steamapps", f"appmanifest_{appid}.acf")
    installdir_name = None
    if os.path.exists(manifest_path):
        with open(manifest_path, "r", encoding="utf-8", errors="ignore") as f:
            m = re.search(r'"installdir"\s*"([^"]+)"', f.read())
            if m:
                installdir_name = m.group(1)
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

    steam_exe = os.path.join(steam_path, "steam.exe")
    launch_uri = f"steam://rungameid/{appid}"

    standard_dlls = get_standard_dlls()
    print(f"DLLs to inject:")
    for d in standard_dlls:
        exists = "OK" if os.path.exists(d) else "MISSING!"
        print(f"  [{exists}] {d}")

    if dry_run:
        print("(dry run - not actually launching)")
        return True

    pid = injector.poll_launch_and_inject(image_name, launch_uri, standard_dlls)
    if pid:
        print(f"\nSUCCESS: {game_name} running as PID {pid} with TriDef 3D injected.")
        return True
    else:
        print(f"\nFAILED: never saw {image_name} appear.")
        return False


LAST_USED_PATH = os.path.join(get_app_dir(), "last_game.txt")


def list_registered_games():
    """Every game TriDef 3D Ignition knows about (HKCU\\...\\Games\\<name>),
    filtered to ones that look like Steam games (have a rungameid
    Argument) so we don't list stale/non-Steam entries we can't launch."""
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
                        argument = winreg.QueryValueEx(gk, "Argument")[0]
                    if "rungameid" in argument:
                        games.append(name)
                except OSError:
                    continue
    except FileNotFoundError:
        pass
    return games


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

    last_used = load_last_used()
    if last_used and last_used in games:
        games.remove(last_used)
        games.insert(0, last_used)

    print("TriDef에 등록된 게임:")
    for i, name in enumerate(games, 1):
        marker = " (최근 실행)" if i == 1 and last_used == name else ""
        print(f"  {i}) {name}{marker}")
    print()
    choice = input(f"번호 선택 (엔터 = 1): ").strip()
    if not choice:
        return games[0]
    if choice.isdigit() and 1 <= int(choice) <= len(games):
        return games[int(choice) - 1]
    return choice  # allow typing a name not in the list too


if __name__ == '__main__':
    dry_run = '--dry-run' in sys.argv
    positional = [a for a in sys.argv[1:] if a != '--dry-run']

    if positional:
        game_name = positional[0]
    else:
        game_name = prompt_for_game_name()

    if not game_name:
        print('게임 이름이 필요합니다. 예: python play3d.py "Gone Home"')
        sys.exit(1)

    ok = play3d(game_name, dry_run=dry_run)
    if ok and not dry_run:
        save_last_used(game_name)
    sys.exit(0 if ok else 1)
