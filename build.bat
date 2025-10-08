@echo off
setlocal
cd /d %~dp0

set "SKIP_PAUSE="
if /i "%~1"=="--no-pause" set "SKIP_PAUSE=1"

REM Detect MSYS2 UCRT64 gcc
where gcc >nul 2>nul
if errorlevel 1 (
  echo [!] gcc not found on PATH. Open "MSYS2 MinGW UCRT64" and run:
  echo     pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils
  call :maybe_pause
  exit /b 1
)

REM Try to build resources (icon). If windres missing, continue without icon.
where windres >nul 2>nul
if errorlevel 1 (
  echo [i] windres not found; building without embedded icon...
  gcc -O2 -s -o GreenScope.exe src\greenscope.c -lIphlpapi
  goto :done
)

echo [i] Building with embedded icon...
windres greenscope.rc greenscope_res.o
if errorlevel 1 (
  echo [i] windres failed; continuing without icon...
  gcc -O2 -s -o GreenScope.exe src\greenscope.c -lIphlpapi
) else (
  gcc -O2 -s -o GreenScope.exe src\greenscope.c greenscope_res.o -lIphlpapi
)

:done
if errorlevel 1 (
  echo [!] Build failed.
  call :maybe_pause
  exit /b 1
)
echo [OK] Built GreenScope.exe
call :maybe_pause
exit /b 0

:maybe_pause
if not defined SKIP_PAUSE pause
exit /b 0
