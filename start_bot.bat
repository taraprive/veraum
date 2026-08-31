@echo off
rem ============================================================
rem  HFT Arbitrage Bot - one-click start
rem  Double-click this file. Ctrl+C inside the window stops it.
rem  Optional: pass a config file as first argument to override.
rem ============================================================
cd /d "%~dp0"

set "CFG=%1"
if "%CFG%"=="" set "CFG=config\config.json"

rem MinGW runtime DLLs (needed to run the exe on Windows)
set "PATH=C:\Users\user\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin;%PATH%"

echo [1/3] Building (incremental - fast when nothing changed)...
"C:\Program Files\CMake\bin\cmake.exe" --build build
if errorlevel 1 goto :buildfail

echo [2/3] Starting bot with %CFG% ...  (Ctrl+C to stop)
echo.
build\hft_arbitrage_bot.exe "%CFG%" config\exchanges.json

echo.
echo [3/3] Bot stopped. PnL summary is above. Press any key to close.
pause >nul
exit /b 0

:buildfail
echo.
echo BUILD FAILED - see the red messages above.
pause
exit /b 1
