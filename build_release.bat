@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ============================================
echo   FFXIV MOD Hanhua Tool - Release Package
echo ============================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_release.ps1"
set EXITCODE=%errorlevel%
echo.
if not "%EXITCODE%"=="0" (
    echo [FAILED] Packaging error, exit code %EXITCODE%
    echo.
    pause
    exit /b %EXITCODE%
)
echo [OK] Package ready: release\ and FFXIV_Mod_Hanhua_v1.0_Green.zip
echo.
pause
