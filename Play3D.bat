@echo off
chcp 65001 >nul
if "%~1"=="" (
    "%~dp0Play3D.exe"
) else (
    "%~dp0Play3D.exe" "%~1"
)
echo.
pause
