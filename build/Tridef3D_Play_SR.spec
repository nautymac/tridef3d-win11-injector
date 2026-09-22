# -*- mode: python ; coding: utf-8 -*-
# SR panel variant: same as Tridef3D_Play.spec but built from play3d_sr.py,
# which injects SRWeaveDX9/DX11.dll (sr\x86, sr\x64 next to the exe) after
# TriDef's DLLs. Equivalent to Tridef3D_Play.exe --sr.


a = Analysis(
    ['../play3d_sr.py'],
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
    name='Tridef3D_Play_SR',
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
