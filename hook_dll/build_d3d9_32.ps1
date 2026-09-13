$ErrorActionPreference = "Stop"

$MSVC = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$SDKBASE = "$env:TEMP\claude\C--Users-nauty\159b1d72-5d66-4b5d-8d65-7840c427e7b2\scratchpad\sdk_nuget\nuget_packages"
$SDKINC = "$SDKBASE\microsoft.windows.sdk.cpp\10.0.22621.3233\c\Include\10.0.22621.0"
$SDKLIB86BASE = "$SDKBASE\microsoft.windows.sdk.cpp.x86\10.0.22621.3233\c"

$cl = "$MSVC\bin\Hostx64\x86\cl.exe"
$srcDir = $PSScriptRoot
$outDll = "$srcDir\tridef_d3d9_hook.dll"

$includes = @(
  "$MSVC\include",
  "$SDKINC\shared",
  "$SDKINC\um",
  "$SDKINC\ucrt",
  "$SDKINC\winrt"
) | ForEach-Object { "/I`"$_`"" }

$libpaths = @(
  "$MSVC\lib\x86",
  "$SDKLIB86BASE\um\x86",
  "$SDKLIB86BASE\ucrt\x86"
) | ForEach-Object { "/LIBPATH:`"$_`"" }

$argsList = @(
  "/nologo", "/EHsc", "/LD", "/std:c++17", "/MT"
) + $includes + @(
  "$srcDir\hook_d3d9.cpp",
  "/Fe:$outDll",
  "/link"
) + $libpaths + @(
  "d3d9.lib", "d3dcompiler.lib", "user32.lib", "gdi32.lib", "kernel32.lib"
)

Write-Output "=== Compiling (x86) hook_d3d9.cpp ==="
& $cl @argsList
Write-Output "Exit code: $LASTEXITCODE"
