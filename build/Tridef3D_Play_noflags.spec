# -*- mode: python ; coding: utf-8 -*-
# Same as Tridef3D_Play.spec but built from play3d_noflags.py: launches via
# the plain steam:// handler with no -no-browser -no-cef-sandbox flags.


a = Analysis(
    ['../play3d_noflags.py'],
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
    name='Tridef3D_Play_noflags',
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
