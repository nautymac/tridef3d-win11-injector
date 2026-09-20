@echo off
chcp 65001 >nul
if "%~1"=="" (
    "%~dp0Tridef3D_Dx11_Play.exe"
) else (
    "%~dp0Tridef3D_Dx11_Play.exe" "%~1"
)
echo.
pause
