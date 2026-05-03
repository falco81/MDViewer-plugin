@echo off
setlocal enabledelayedexpansion

REM =============================================================================
REM build-windows.bat
REM
REM One-shot build script for MDViewer on Windows 10 / 11.
REM
REM What it does:
REM   1. Locates MSYS2 (default install path: "C:\Program Files\msys64").
REM      If MSYS2 is missing it is installed via winget.
REM      Override with:  set MSYS2_HOME=D:\path\to\msys64
REM   2. Installs MinGW-w64 32-bit and 64-bit toolchains, make, and zip
REM      into MSYS2 via pacman (if not already present).
REM   3. Compiles MDViewer.wlx (32-bit) and MDViewer.wlx64 (64-bit).
REM   4. Packages MDViewer-plugin.zip - the Total Commander install bundle.
REM
REM Run from a Command Prompt:  build-windows.bat
REM Run as Administrator the FIRST time so winget / MSYS2 install can write
REM into "C:\Program Files".
REM =============================================================================

cd /d "%~dp0"

echo.
echo ========================================================================
echo   MDViewer build script (Windows)
echo ========================================================================
echo.

REM --- 1. MSYS2 location -------------------------------------------------------
REM    Discovery order:
REM      a) MSYS2_HOME env var (explicit override)
REM      b) "C:\Program Files\msys64" (preferred default)
REM      c) "C:\msys64" (legacy default that older docs / installers used)
REM    The first folder that actually contains a usable mingw64\bin\g++.exe
REM    wins.
set "MSYS2_PREFERRED=C:\Program Files\msys64"
set "MSYS2_LEGACY=C:\msys64"

set "MSYS2_ROOT="
if defined MSYS2_HOME (
    if exist "%MSYS2_HOME%\mingw64\bin\g++.exe"      set "MSYS2_ROOT=%MSYS2_HOME%"
    if not defined MSYS2_ROOT if exist "%MSYS2_HOME%\usr\bin\bash.exe" set "MSYS2_ROOT=%MSYS2_HOME%"
)
if not defined MSYS2_ROOT if exist "%MSYS2_PREFERRED%\usr\bin\bash.exe" set "MSYS2_ROOT=%MSYS2_PREFERRED%"
if not defined MSYS2_ROOT if exist "%MSYS2_LEGACY%\usr\bin\bash.exe"    set "MSYS2_ROOT=%MSYS2_LEGACY%"

REM --- 2. Install MSYS2 if absent ---------------------------------------------
if not defined MSYS2_ROOT (
    echo [*] MSYS2 not found, attempting install via winget to:
    echo       %MSYS2_PREFERRED%
    where winget >nul 2>&1
    if errorlevel 1 (
        echo [X] winget is not available on this system.
        echo     Install MSYS2 manually from https://www.msys2.org/
        echo     to "%MSYS2_PREFERRED%" and re-run this script.
        exit /b 1
    )

    REM "--location" makes Inno-Setup-based installers (which MSYS2 uses)
    REM honour /DIR=. Some packages ignore it; if so the install will still
    REM succeed but at the package's default path and we fall back below.
    winget install --id MSYS2.MSYS2 ^
        --accept-package-agreements --accept-source-agreements ^
        --silent --location "%MSYS2_PREFERRED%"
    if errorlevel 1 (
        echo [X] winget install of MSYS2.MSYS2 failed.
        echo     Install MSYS2 manually from https://www.msys2.org/ and re-run.
        exit /b 1
    )

    REM Re-run discovery after install.
    if exist "%MSYS2_PREFERRED%\usr\bin\bash.exe" set "MSYS2_ROOT=%MSYS2_PREFERRED%"
    if not defined MSYS2_ROOT if exist "%MSYS2_LEGACY%\usr\bin\bash.exe" set "MSYS2_ROOT=%MSYS2_LEGACY%"

    if not defined MSYS2_ROOT (
        echo [X] MSYS2 was installed but neither
        echo       "%MSYS2_PREFERRED%"
        echo       "%MSYS2_LEGACY%"
        echo     contain bash.exe. Set MSYS2_HOME to the install path and re-run.
        exit /b 1
    )
    echo [OK] MSYS2 installed at "%MSYS2_ROOT%"
) else (
    echo [OK] MSYS2 found at "%MSYS2_ROOT%"
)

set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "CXX32=%MSYS2_ROOT%\mingw32\bin\g++.exe"
set "CXX64=%MSYS2_ROOT%\mingw64\bin\g++.exe"
set "ZIP=%MSYS2_ROOT%\usr\bin\zip.exe"

REM --- 3. Install required toolchains via pacman ------------------------------
set "NEED_PACMAN=0"
if not exist "%CXX32%" set "NEED_PACMAN=1"
if not exist "%CXX64%" set "NEED_PACMAN=1"
if not exist "%ZIP%"   set "NEED_PACMAN=1"

if "%NEED_PACMAN%"=="1" (
    echo [*] Installing MinGW-w64 toolchains, make, and zip via pacman...
    "%BASH%" -lc "pacman -Sy --noconfirm" || (echo [X] pacman -Sy failed & exit /b 1)
    "%BASH%" -lc "pacman -S --noconfirm --needed mingw-w64-i686-gcc mingw-w64-x86_64-gcc make zip" || (echo [X] pacman -S failed & exit /b 1)
) else (
    echo [OK] MinGW-w64 toolchains and tools already installed.
)

REM --- 4. Verify -------------------------------------------------------------
if not exist "%CXX32%" (
    echo [X] 32-bit compiler missing: %CXX32%
    exit /b 1
)
if not exist "%CXX64%" (
    echo [X] 64-bit compiler missing: %CXX64%
    exit /b 1
)
if not exist "%ZIP%" (
    echo [X] zip.exe missing: %ZIP%
    exit /b 1
)

echo.
echo [*] Toolchain ready:
echo     32-bit g++ : %CXX32%
echo     64-bit g++ : %CXX64%
echo     zip        : %ZIP%
echo.

REM --- 5. Common compile flags -----------------------------------------------
set "CXXFLAGS=-O2 -s -std=c++17 -Wall -Wextra -Wno-unused-parameter -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600"
set "LDFLAGS=-shared -Wl,--enable-stdcall-fixup -Wl,--kill-at -static -static-libgcc -static-libstdc++"
set "LIBS=-lcomctl32 -lshlwapi -luser32 -lkernel32 -lgdi32 -ladvapi32 -lgdiplus -lshell32 -lcomdlg32 -lwinspool -ld2d1 -ldwrite -lwinhttp"
set "SRCS=mdviewer.cpp parser.cpp renderer.cpp"
set "DEFFILE=mdviewer.def"

REM --- 6. Clean previous artifacts -------------------------------------------
if exist MDViewer.wlx          del /q MDViewer.wlx
if exist MDViewer.wlx64        del /q MDViewer.wlx64
if exist MDViewer-plugin.zip   del /q MDViewer-plugin.zip

REM --- 7. Build 32-bit -------------------------------------------------------
REM    cc1plus.exe lives under mingw32\libexec\..., not under mingw32\bin,
REM    so it cannot find its companion DLLs (libisl, libgmp, libmpc,
REM    libgcc_s_dw2-1, etc.) unless we add the bin folder to PATH for the
REM    duration of the compiler call. Without this the build fails with
REM    "The code execution cannot proceed because libisl-23.dll was not
REM    found" and friends.
set "ORIG_PATH=%PATH%"

echo [*] Building 32-bit DLL (MDViewer.wlx)...
set "PATH=%MSYS2_ROOT%\mingw32\bin;%ORIG_PATH%"
"%CXX32%" %CXXFLAGS% %SRCS% %DEFFILE% -o MDViewer.wlx %LDFLAGS% %LIBS%
set "PATH=%ORIG_PATH%"
if errorlevel 1 (
    echo [X] 32-bit build failed.
    exit /b 1
)
echo [OK] MDViewer.wlx built.

REM --- 8. Build 64-bit -------------------------------------------------------
echo [*] Building 64-bit DLL (MDViewer.wlx64)...
set "PATH=%MSYS2_ROOT%\mingw64\bin;%ORIG_PATH%"
"%CXX64%" %CXXFLAGS% %SRCS% %DEFFILE% -o MDViewer.wlx64 %LDFLAGS% %LIBS%
set "PATH=%ORIG_PATH%"
if errorlevel 1 (
    echo [X] 64-bit build failed.
    exit /b 1
)
echo [OK] MDViewer.wlx64 built.

REM --- 9. Package install zip ------------------------------------------------
echo [*] Creating MDViewer-plugin.zip...
"%ZIP%" -j MDViewer-plugin.zip pluginst.inf MDViewer.wlx MDViewer.wlx64
if errorlevel 1 (
    echo [X] zip packaging failed.
    exit /b 1
)
echo [OK] MDViewer-plugin.zip created.

echo.
echo ========================================================================
echo   Build successful.
echo ========================================================================
echo.
dir /b MDViewer.wlx MDViewer.wlx64 MDViewer-plugin.zip
echo.
echo To install in Total Commander:
echo   - Click MDViewer-plugin.zip from inside TC.
echo   - TC will offer to install the plugin via pluginst.inf.
echo.

endlocal
exit /b 0
