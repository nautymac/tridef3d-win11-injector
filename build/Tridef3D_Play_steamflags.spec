# -*- mode: python ; coding: utf-8 -*-
# Backup variant: same as Tridef3D_Play.spec but built from
# play3d_steamflags.py, which launches steam.exe directly with
# -no-browser -no-cef-sandbox instead of the plain steam:// handler.


a = Analysis(
    ['../play3d_steamflags.py'],
    pathex=['..'],
    binaries=[],
    datas=[],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='Tridef3D_Play_steamflags',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
