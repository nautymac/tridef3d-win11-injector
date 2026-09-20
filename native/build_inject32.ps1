# Builds Inject32.exe (native 32-bit LoadLibraryW injector) into the repo root.
#
# The source is header-free and CRT-free, so all this needs is MSVC's x86
# cross compiler plus the x86 import libraries kernel32.lib and shell32.lib
# (from any Windows SDK, or the Microsoft.Windows.SDK.CPP.x86 nuget package).
# Point the two paths below at your install.
$ErrorActionPreference = "Stop"

$MSVC   = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$SDKLIB = "$env:TEMP\claude\C--Users-nauty\159b1d72-5d66-4b5d-8d65-7840c427e7b2\scratchpad\sdk_nuget\nuget_packages\microsoft.windows.sdk.cpp.x86\10.0.22621.3233\c\um\x86"

$cl  = "$MSVC\bin\Hostx64\x86\cl.exe"
$src = "$PSScriptRoot\inject32.cpp"
$out = (Resolve-Path "$PSScriptRoot\..").Path + "\Inject32.exe"
$obj = "$env:TEMP\inject32.obj"

$env:PATH = "$MSVC\bin\Hostx64\x86;$MSVC\bin\Hostx64\x64;$env:PATH"

$argsList = @(
  "/nologo", "/O1", "/GS-", "/GR-", "/EHs-c-", "/Zl", "/W3", "/Fo$obj", $src,
  "/Fe:$out", "/link", "/NODEFAULTLIB", "/ENTRY:EntryMain", "/SUBSYSTEM:CONSOLE", "/MACHINE:X86",
  "/LIBPATH:$SDKLIB", "kernel32.lib", "shell32.lib"
)

Write-Output "=== Compiling Inject32.exe (x86, no CRT) ==="
& $cl @argsList
if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with $LASTEXITCODE" }
Remove-Item $obj -ErrorAction SilentlyContinue
Get-Item $out | Select-Object FullName, Length, LastWriteTime
