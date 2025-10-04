@echo off
setlocal
cd /d %~dp0

REM Detect MSYS2 UCRT64 gcc
where gcc >nul 2>nul
if errorlevel 1 (
  echo [!] gcc not found on PATH. Open "MSYS2 MinGW UCRT64" and run:
  echo     pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils
  pause
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
  pause
  exit /b 1
)
echo [OK] Built GreenScope.exe
pause
