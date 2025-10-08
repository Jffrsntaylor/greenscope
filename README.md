# GreenScope

Green-on-black Windows console system monitor written in C. Minimal, fast, retro.

![icon](icons/greenscope.png)

## Quick download / releases

- Ensure you're on the `main` branch, then run from Windows PowerShell 5.1 or PowerShell 7+ (`pwsh`).
- The bundled `scripts/prepare_release.ps1` script now verifies the branch, compiles `GreenScope.exe`, gathers the icon, README, and license, and creates `dist/GreenScope-<version>.zip` with the original artwork ready for the GitHub release download button.
- Supply `-Publish` (and optionally `-ReleaseNotesPath` or `-ReleaseNotes`) to automatically push a GitHub release via the GitHub CLI with the correct thumbnail.
- On the GitHub release page, the hero image/download button should use `icons/greenscope.png`, which the script bundles alongside the executable.

```powershell
# Prepare artifacts only
pwsh -ExecutionPolicy Bypass -File scripts/prepare_release.ps1 -Version 0.6.0

# Prepare and publish via GitHub CLI (gh)
pwsh -ExecutionPolicy Bypass -File scripts/prepare_release.ps1 -Version 0.6.0 -Publish
```

The script stores release ZIPs and generated notes in `dist/`. When using `-Publish`, ensure you are authenticated with `gh auth login` first. Release notes are taken from `CHANGELOG.md` unless an override is provided.

## Features
- CPU usage (overall and per-core) with animated sparkline history
- Memory usage with history bar and committed vs. available breakdown
- Disk usage by drive with cached refresh intervals
- Network throughput by adapter with ring-buffer history
- Battery, AC status, estimated runtime, and uptime
- Thermal zone temperature sampling (where supported)
- Reduced flicker console with colorized sections and ASCII/Unicode toggle (`A`)
- Press `Q` to quit

## Build (MSYS2 MinGW UCRT64)

1. Install MSYS2 and open **MSYS2 MinGW UCRT64**.
2. Install toolchain (first time only):
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils
   ```
3. Clone and build:
   ```bash
   git clone https://github.com/<yourname>/greenscope.git
   cd greenscope
   ./build.bat
   ```
   - If `windres` is available, the EXE will embed the icon.
   - If not, it still builds; you can set an icon on a desktop shortcut.

**Manual compile** (no icon):
```bash
gcc -O2 -s -o GreenScope.exe src/greenscope.c -lIphlpapi
```

**Manual compile with icon**:
```bash
windres greenscope.rc greenscope_res.o
gcc -O2 -s -o GreenScope.exe src/greenscope.c greenscope_res.o -lIphlpapi
```

## Run
- Double‑click `GreenScope.exe` or run it from a terminal.  
- (Optional) Create a Desktop **shortcut** → **Properties** → **Change Icon…** and select `icons/greenscope.ico`.

## Notes
- Tested on Windows 10/11 terminals. Toggle ASCII with `A` if you see border glitches.
- No admin privileges required. No network calls, no file writes.

## License
MIT
