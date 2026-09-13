$ErrorActionPreference = "Stop"

$MSVC = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231"
$SDKBASE = "$env:TEMP\claude\C--Users-nauty\159b1d72-5d66-4b5d-8d65-7840c427e7b2\scratchpad\sdk_nuget\nuget_packages"
$SDKINC = "$SDKBASE\microsoft.windows.sdk.cpp\10.0.22621.3233\c\Include\10.0.22621.0"
$SDKLIB = "$SDKBASE\microsoft.windows.sdk.cpp.x64\10.0.22621.3233\c"

$cl = "$MSVC\bin\Hostx64\x64\cl.exe"
$srcDir = $PSScriptRoot
$outDll = "$srcDir\tridef_stereo_hook.dll"

$includes = @(
  "$MSVC\include",
  "$SDKINC\shared",
  "$SDKINC\um",
  "$SDKINC\ucrt",
  "$SDKINC\winrt"
) | ForEach-Object { "/I`"$_`"" }

$libpaths = @(
  "$MSVC\lib\x64",
  "$SDKLIB\um\x64",
  "$SDKLIB\ucrt\x64"
) | ForEach-Object { "/LIBPATH:`"$_`"" }

$env:PATH = "$MSVC\bin\Hostx64\x64;$env:PATH"

$argsList = @(
  "/nologo", "/EHsc", "/LD", "/std:c++17", "/MT"
) + $includes + @(
  "$srcDir\hook.cpp",
  "/Fe:$outDll",
  "/link"
) + $libpaths + @(
  "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "user32.lib", "gdi32.lib", "kernel32.lib"
)

Write-Output "=== Compiling ==="
Write-Output "$cl $($argsList -join ' ')"
& $cl @argsList
Write-Output "Exit code: $LASTEXITCODE"
