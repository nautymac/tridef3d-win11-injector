"""Entry point for the Tridef3D_Play_steamflags build - the backup variant.

Identical to play3d.py except Steam is launched as
`steam.exe -no-browser -no-cef-sandbox steam://rungameid/<id>` and those
flags are written into Ignition's per-game Argument value. This is how
every early verified run was done; the flags later turned out not to
matter (DLL load order was the real fix), so the plain build is the
default and this one is kept just in case."""
import play3d

if __name__ == '__main__':
    play3d.main(default_steam_flags=True)
