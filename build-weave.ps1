# Build SRWeaveDX9.dll / SRWeaveDX11.dll for x64 and x86.
# Output: bin\weave\x64\*.dll and bin\weave\x86\*.dll
#
# Sets up the MSVC + Windows SDK environment by hand (no vswhere/vcvars: they fail on machines
# where the VS instance isn't registered) and drives the CMake/Ninja bundled with Visual Studio.
# Override auto-detection with $env:SRWEAVE_MSVC (…\VC\Tools\MSVC\<ver>) and $env:SRWEAVE_SDK
# (…\Windows Kits\10) / $env:SRWEAVE_SDKVER (10.0.xxxxx.0) if needed.
param([string]$Config = 'Release', [string[]]$Archs = @('x64', 'x86'))
$ErrorActionPreference = 'Stop'

$msvc = $env:SRWEAVE_MSVC
if (-not $msvc) {
    $msvc = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*" -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $msvc) { throw 'MSVC toolset not found (set $env:SRWEAVE_MSVC)' }
$vsRoot = ($msvc -replace '\\VC\\Tools\\MSVC\\[^\\]+$', '')
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $cmake)) { throw "cmake not found at $cmake" }

$sdk = $env:SRWEAVE_SDK
if (-not $sdk) {
    $sdk = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
}
if (-not $sdk) { throw 'Windows 10 SDK not found (set $env:SRWEAVE_SDK)' }
$sdk = $sdk.TrimEnd('\')
$sdkVer = $env:SRWEAVE_SDKVER
if (-not $sdkVer) {
    $sdkVer = Get-ChildItem "$sdk\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\um\windows.h" } |
              Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
}
if (-not $sdkVer) { throw "no usable SDK version under $sdk\Include" }
Write-Host "MSVC: $msvc"
Write-Host "SDK:  $sdk ($sdkVer)"

$root = $PSScriptRoot
foreach ($arch in $Archs) {
    $hostBin = Join-Path $msvc "bin\Hostx64\$arch"
    if (-not (Test-Path (Join-Path $hostBin 'cl.exe'))) { $hostBin = Join-Path $msvc "bin\Hostx86\$arch" }
    # rc.exe / mt.exe come from the SDK's host (x64) bin folder
    $sdkBin = "$sdk\bin\$sdkVer\x64"
    $env:PATH = "$hostBin;" + (Join-Path $msvc 'bin\Hostx64\x64') + ";$sdkBin;" + (Split-Path $cmake) + ";" + (Split-Path $ninja) + ";" + $env:PATH
    $env:INCLUDE = "$msvc\include;$sdk\Include\$sdkVer\ucrt;$sdk\Include\$sdkVer\um;$sdk\Include\$sdkVer\shared;$sdk\Include\$sdkVer\winrt"
    $env:LIB = "$msvc\lib\$arch;$sdk\Lib\$sdkVer\ucrt\$arch;$sdk\Lib\$sdkVer\um\$arch"
    $env:CC = 'cl.exe'; $env:CXX = 'cl.exe'
    $b = Join-Path $root "build-$arch"
    Write-Host "=== $arch ==="
    & $cmake -G Ninja -S $root -B $b "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_MAKE_PROGRAM=$ninja"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $cmake --build $b
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Get-ChildItem (Join-Path $root '..\bin\weave') -Recurse -Filter *.dll | Select-Object FullName, Length, LastWriteTime
