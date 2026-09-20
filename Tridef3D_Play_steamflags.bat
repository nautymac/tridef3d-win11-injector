@echo off
chcp 65001 >nul
if "%~1"=="" (
    "%~dp0Tridef3D_Play_steamflags.exe"
) else (
    "%~dp0Tridef3D_Play_steamflags.exe" "%~1"
)
echo.
pause
