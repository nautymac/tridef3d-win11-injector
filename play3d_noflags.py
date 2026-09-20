"""Entry point for the Tridef3D_Play_noflags build: identical to play3d.py
except Steam is opened through the plain steam:// shell handler with no
-no-browser -no-cef-sandbox flags, and Ignition's per-game Argument is
left untouched. Exists to test whether the flags matter at all."""
import play3d

if __name__ == '__main__':
    play3d.main(default_steam_flags=False)
