# SRWeave 빌드에 필요한 SR SDK 를 풀어 둔다.
#   third_party\sr-sdk     <- simulatedreality-<ver>-win64-Release.exe   (x64 DLL 빌드용)
#   third_party\sr-sdk32   <- simulatedreality-<ver>-win32-Release.exe   (x86 DLL 빌드용)
#
# 설치 파일은 LeiaSR / SpatialLabs 런타임 설치본 안에 들어 있다. win32 판은 win64 설치 파일을
# 풀면 나오는 temp\ 폴더 안에 있다 (예: LeiaSR-Runtime-1.34.10-win64\temp\simulatedreality-1.34.10-win32-Release.exe).
# 재배포하지 않는다 — 각자 설치본에서 가져온다.
param(
    [string]$SrSdkInstaller = '',    # win64 Release 설치 파일 경로
    [string]$SrSdk32Installer = ''   # win32 Release 설치 파일 경로
)
$ErrorActionPreference = 'Stop'
$tp = Join-Path $PSScriptRoot 'third_party'
New-Item -ItemType Directory -Force $tp | Out-Null

function Expand-SrSdk($installer, $dest, $label) {
    if (-not (Test-Path $installer)) { throw "설치 파일이 없습니다: $installer" }
    $sevenZip = @('C:\Program Files\7-Zip\7z.exe', 'C:\Program Files (x86)\7-Zip\7z.exe') |
                Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $sevenZip) { throw '7-Zip 이 필요합니다 (C:\Program Files\7-Zip\7z.exe)' }
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    New-Item -ItemType Directory -Force $dest | Out-Null
    Write-Host "extract SR SDK ($label) -> $dest"
    & $sevenZip x -tNsis -o"$dest" $installer -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "SR SDK 추출 실패 ($LASTEXITCODE)" }
}

$sdk = Join-Path $tp 'sr-sdk'
if ($SrSdkInstaller) {
    Expand-SrSdk $SrSdkInstaller $sdk 'win64'
} elseif (-not (Test-Path (Join-Path $sdk 'include\sr\weaver\dx9weaver.h'))) {
    Write-Host "SR SDK(64) 없음: $sdk  -> -SrSdkInstaller <simulatedreality-*-win64-Release.exe>"
}

$sdk32 = Join-Path $tp 'sr-sdk32'
if ($SrSdk32Installer) {
    Expand-SrSdk $SrSdk32Installer $sdk32 'win32'
} elseif (-not (Test-Path (Join-Path $sdk32 'lib\SimulatedRealityDirectX32.lib'))) {
    Write-Host "SR SDK(32) 없음: $sdk32  -> -SrSdk32Installer <simulatedreality-*-win32-Release.exe>"
}
Write-Host 'done'
