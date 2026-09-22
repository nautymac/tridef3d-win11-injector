@echo off
chcp 65001 >nul
if "%~1"=="" (
    "%~dp0Tridef3D_Play_SR.exe"
) else (
    "%~dp0Tridef3D_Play_SR.exe" "%~1"
)
echo.
pause
