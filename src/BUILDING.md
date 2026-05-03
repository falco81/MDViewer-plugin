# Building MDViewer from source

This document describes how to compile `MDViewer.wlx` (32-bit) and
`MDViewer.wlx64` (64-bit) from the source code, and how to package the
Total Commander install ZIP.

You have two options on each platform:

* **Scripted build** — run a single script (`build-linux.sh` or
  `build-windows.bat`); it installs all dependencies and compiles.
* **Manual build** — install dependencies yourself, then run `make` (or
  invoke the compiler directly).

Final outputs in both cases:

| File                    | Purpose                                  |
|-------------------------|------------------------------------------|
| `MDViewer.wlx`          | 32-bit Lister plugin DLL                 |
| `MDViewer.wlx64`        | 64-bit Lister plugin DLL                 |
| `MDViewer-plugin.zip`   | Install bundle for Total Commander       |

---

## Sources and dependencies

| Source            | What it does                                                   |
|-------------------|----------------------------------------------------------------|
| `parser.cpp/.h`   | Markdown → block AST (paragraphs, headings, tables, lists, …) |
| `renderer.cpp/.h` | Layout + paint commands using GDI (text) and GDI+ (images)    |
| `mdviewer.cpp`    | Plugin entry points + native viewer window                     |

Linker dependencies (Win32 system libraries — already present on every
Windows install):

```
comctl32  shlwapi  user32  kernel32  gdi32  advapi32  gdiplus  shell32
```

No third-party libraries are used. The DLL is statically linked against
libgcc/libstdc++ so it runs without any extra MinGW runtime.

---

## Linux (AlmaLinux 9 — primary target)

The script also handles AlmaLinux 8, RHEL 8/9, Rocky 8/9, CentOS Stream 9,
Fedora 38+, Ubuntu 20.04+ and Debian 11+. Below the AlmaLinux 9 path is
shown as canonical.

### A. Scripted build (recommended)

```bash
# 1. Extract the source archive
unzip MDViewer-source.zip
cd MDViewer

# 2. Make the script executable and run it
chmod +x build-linux.sh
./build-linux.sh
```

The script will:

1. Detect AlmaLinux.
2. Run `sudo dnf install -y epel-release` and enable the **CRB** repository
   (CodeReady Builder), needed for `mingw*` build dependencies.
3. Install `mingw32-gcc-c++`, `mingw64-gcc-c++`,
   `mingw32-winpthreads-static`, `mingw64-winpthreads-static`, `make`, and
   `zip`.
4. Run `make all`, producing the three artifacts above.

You will be prompted for `sudo` once.

### B. Manual build on AlmaLinux 9

Step by step:

```bash
# 1. EPEL (provides the mingw* packages)
sudo dnf install -y epel-release

# 2. Enable CRB (CodeReady Builder), required for mingw* deps on Alma 9.
#    On Alma/RHEL 8 the equivalent repo is called "powertools".
sudo dnf install -y dnf-plugins-core
sudo dnf config-manager --set-enabled crb

# 3. Install the cross-compilers and supporting tools.
#    The mingw32-gcc-c++ / mingw64-gcc-c++ packages bring everything we
#    need — including the gdiplus headers and import libraries that ship
#    with mingw-w64.
sudo dnf install -y \
    mingw32-gcc-c++ mingw64-gcc-c++ \
    mingw32-winpthreads-static mingw64-winpthreads-static \
    make zip

# 4. Verify the compilers are on PATH
i686-w64-mingw32-g++   --version
x86_64-w64-mingw32-g++ --version

# 5. Compile and package
cd MDViewer        # source directory
make clean
make all
```

Output: `MDViewer.wlx`, `MDViewer.wlx64`, and `MDViewer-plugin.zip`.

#### What `make` actually runs

If you prefer not to use `make`, the Makefile is equivalent to:

```bash
CXXFLAGS="-O2 -s -std=c++17 -Wall -Wextra -Wno-unused-parameter \
          -static-libgcc -static-libstdc++ \
          -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600"

LDFLAGS="-shared -Wl,--enable-stdcall-fixup -Wl,--kill-at \
         -static -static-libgcc -static-libstdc++"

LIBS="-lcomctl32 -lshlwapi -luser32 -lkernel32 \
      -lgdi32 -ladvapi32 -lgdiplus -lshell32 -lcomdlg32 -lwinspool -ld2d1 -ldwrite -lwinhttp"

# 32-bit
i686-w64-mingw32-g++   $CXXFLAGS \
    mdviewer.cpp parser.cpp renderer.cpp mdviewer.def \
    -o MDViewer.wlx   $LDFLAGS $LIBS

# 64-bit
x86_64-w64-mingw32-g++ $CXXFLAGS \
    mdviewer.cpp parser.cpp renderer.cpp mdviewer.def \
    -o MDViewer.wlx64 $LDFLAGS $LIBS

# Install bundle
zip -j MDViewer-plugin.zip pluginst.inf MDViewer.wlx MDViewer.wlx64
```

### C. Other Linux distros

The same script works without changes on Debian/Ubuntu (uses `apt-get`)
and on Fedora (uses `dnf`). For manual builds on these systems:

**Ubuntu / Debian:**

```bash
sudo apt-get update
sudo apt-get install -y mingw-w64 g++-mingw-w64-i686 g++-mingw-w64-x86-64 make zip
make all
```

**Fedora:**

```bash
sudo dnf install -y mingw32-gcc-c++ mingw64-gcc-c++ \
                    mingw32-winpthreads-static mingw64-winpthreads-static \
                    make zip
make all
```

---

## Windows 10 / 11 (native build)

Native build on Windows uses **MSYS2** as the package manager and
**MinGW-w64** as the compiler.

### A. Scripted build (recommended)

```cmd
REM 1. Extract MDViewer-source.zip somewhere, e.g. C:\src\MDViewer
REM 2. Open Command Prompt in that folder, then:

build-windows.bat
```

Run the script as **Administrator** the first time so that `winget` /
MSYS2 install can write into `C:\Program Files`. The script will:

1. Look for MSYS2 in this order:
   - `%MSYS2_HOME%` (env-var override, if set)
   - `C:\Program Files\msys64` (preferred default)
   - `C:\msys64` (legacy default that older docs used)

   If none exist, MSYS2 is installed via
   `winget install MSYS2.MSYS2 --location "C:\Program Files\msys64"`.
2. Run `pacman -Sy` and install:
   `mingw-w64-i686-gcc`, `mingw-w64-x86_64-gcc`, `make`, `zip`.
3. Compile both DLLs by invoking the discovered g++.exe directly.
   The script prepends the matching `mingw32\bin` (or `mingw64\bin`)
   to `PATH` for the duration of each compiler call so that the
   internal `cc1plus.exe` (which lives under `libexec\gcc\...`)
   can locate its companion DLLs (`libisl-23.dll`, `libgmp-10.dll`,
   `libgcc_s_dw2-1.dll`, `libmpc-3.dll`, etc.).
4. Package `MDViewer-plugin.zip`.

If your MSYS2 lives somewhere else, set the environment variable
`MSYS2_HOME` first:

```cmd
set MSYS2_HOME=D:\tools\msys64
build-windows.bat
```

### B. Manual build on Windows 10

1. **Install MSYS2.**

   Easiest: open an elevated Command Prompt and run

   ```cmd
   winget install --id MSYS2.MSYS2 --location "C:\Program Files\msys64"
   ```

   Or download the installer from <https://www.msys2.org/> and install
   to `C:\Program Files\msys64`.

2. **Open the MSYS2 shell** (Start Menu → "MSYS2 MSYS").

3. **Install the toolchains and tools:**

   ```bash
   pacman -Syu --noconfirm
   pacman -S --noconfirm --needed \
       mingw-w64-i686-gcc \
       mingw-w64-x86_64-gcc \
       make zip
   ```

4. **Build using `make`** from the generic MSYS2 shell, with explicit
   compiler paths so that **both** architectures get built:

   ```bash
   cd /c/src/MDViewer        # adjust to your source folder

   make CXX32=/mingw32/bin/g++.exe \
        CXX64=/mingw64/bin/g++.exe \
        all
   ```

   Or run two separate commands from the matching shells:

   ```bash
   # In MSYS2 MINGW32 shell:
   g++ -O2 -s -std=c++17 -static-libgcc -static-libstdc++ \
       -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 \
       mdviewer.cpp parser.cpp renderer.cpp mdviewer.def \
       -o MDViewer.wlx \
       -shared -Wl,--enable-stdcall-fixup -Wl,--kill-at \
       -static -static-libgcc -static-libstdc++ \
       -lcomctl32 -lshlwapi -luser32 -lkernel32 \
       -lgdi32 -ladvapi32 -lgdiplus -lshell32 \
       -lcomdlg32 -lwinspool -ld2d1 -ldwrite -lwinhttp

   # In MSYS2 MINGW64 shell:
   g++ -O2 -s -std=c++17 -static-libgcc -static-libstdc++ \
       -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600 \
       mdviewer.cpp parser.cpp renderer.cpp mdviewer.def \
       -o MDViewer.wlx64 \
       -shared -Wl,--enable-stdcall-fixup -Wl,--kill-at \
       -static -static-libgcc -static-libstdc++ \
       -lcomctl32 -lshlwapi -luser32 -lkernel32 \
       -lgdi32 -ladvapi32 -lgdiplus -lshell32 \
       -lcomdlg32 -lwinspool -ld2d1 -ldwrite -lwinhttp
   ```

5. **Package the install bundle:**

   ```bash
   zip -j MDViewer-plugin.zip pluginst.inf MDViewer.wlx MDViewer.wlx64
   ```

---

## Compile flags — explanation

| Flag                              | Why                                                                    |
|-----------------------------------|------------------------------------------------------------------------|
| `-O2 -s`                          | Optimize and strip debug symbols.                                      |
| `-std=c++17`                      | Code uses `[[maybe_unused]]` and `std::move` in modern form.          |
| `-static-libgcc -static-libstdc++`| Self-contained DLL — no `libstdc++-6.dll` / `libgcc_s_dw2-1.dll` deps. |
| `-DUNICODE -D_UNICODE`            | Win32 wide-char APIs (`CreateFileW`, `CreateWindowExW`, …).            |
| `-DWINVER=0x0600 -D_WIN32_WINNT=0x0600` | Target Windows Vista+ (gives access to `SetWindowSubclass`).        |
| `-shared`                         | Build a DLL.                                                           |
| `-Wl,--enable-stdcall-fixup`      | Linker handles `__stdcall` decoration mismatches gracefully.           |
| `-Wl,--kill-at`                   | Strip `@N` suffixes from exported `__stdcall` names so TC can find them. |
| `mdviewer.def`                    | Module-definition file listing the 8 plugin exports.                   |

Required Windows libraries linked: `comctl32`, `shlwapi`, `user32`,
`kernel32`, `gdi32`, `advapi32`, **`gdiplus`** (image loading + text
rendering), `shell32` (`ShellExecuteW` for link clicks), `comdlg32`
(`PrintDlgW` for Ctrl+E PDF export), `winspool` (printer enumeration).

---

## Verifying the build

After build, check that the DLL has the right architecture and the right
exports.

On Linux:

```bash
file MDViewer.wlx MDViewer.wlx64
# MDViewer.wlx:   PE32 executable (DLL) (console) Intel 80386, for MS Windows
# MDViewer.wlx64: PE32+ executable (DLL) (console) x86-64, for MS Windows

x86_64-w64-mingw32-objdump -p MDViewer.wlx64 | grep -E "List[A-Z]"
# Expected exports (no @N decoration):
#   ListCloseWindow
#   ListGetDetectString
#   ListLoad
#   ListLoadW
#   ListSearchText
#   ListSearchTextW
#   ListSendCommand
#   ListSetDefaultParams
```

On Windows (in MSYS2 shell):

```bash
objdump -p MDViewer.wlx64 | grep -E "List[A-Z]"
```

If you see decorated names like `ListLoad@16`, the `-Wl,--kill-at` flag
was lost; re-add it.

---

## Troubleshooting

**`i686-w64-mingw32-g++: command not found` on AlmaLinux**
- Verify EPEL is enabled: `dnf repolist enabled | grep -i epel`
- Verify CRB is enabled: `dnf repolist enabled | grep -i crb`
- Re-run: `sudo dnf install -y mingw32-gcc-c++ mingw64-gcc-c++`

**`cannot find -lwinpthread` (Alma/RHEL/Fedora)**
- Install: `sudo dnf install -y mingw32-winpthreads-static mingw64-winpthreads-static`

**`cannot find -lgdiplus`**
- The `gdiplus` import library is part of `mingw*-crt` packages; if it’s
  missing, reinstall the toolchain: `sudo dnf reinstall mingw32-gcc-c++ mingw64-gcc-c++`

**`pacman: command not found` (Windows)**
- MSYS2 is not installed at `C:\Program Files\msys64` or `C:\msys64`. Run the `winget` command above
  or install manually from <https://www.msys2.org/>.

**`winget: not recognized as a command` (Windows)**
- Install "App Installer" from the Microsoft Store, or upgrade Windows 10
  to a build that has winget (1809+ with App Installer).

**Plugin loads but page doesn't scroll on long files**
- This was a bug in older HTML-based versions of the plugin. The current
  fully-native version uses a real Win32 vertical scrollbar; if scrolling
  doesn't work, please file an issue with details (file size, viewer
  height, keystrokes you tried).

---

## File overview

| File                | Role                                                       |
|---------------------|------------------------------------------------------------|
| `parser.h/.cpp`     | Markdown → block AST.                                      |
| `renderer.h/.cpp`   | Layout + paint draws (text via GDI, images via GDI+).      |
| `mdviewer.cpp`      | Plugin exports, window proc, scrollbar, search overlay.    |
| `listplug.h`        | TC Lister Plugin API constants.                            |
| `mdviewer.def`      | DLL exports (no name decoration).                          |
| `pluginst.inf`      | TC plugin install manifest.                                |
| `Makefile`          | GNU make recipe for both architectures.                    |
| `build-linux.sh`    | One-shot Linux dependency install + build.                 |
| `build-windows.bat` | One-shot Windows dependency install + build.               |
| `BUILDING.md`       | This document.                                             |
| `README.md`         | User-facing description and install instructions.          |
