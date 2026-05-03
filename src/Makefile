# Makefile - build MDViewer.wlx (32-bit) and MDViewer.wlx64 (64-bit)
# Cross-compile from Linux with mingw-w64, or build natively on Windows
# (when CXX32 / CXX64 are pointed at MSYS2 mingw32 / mingw64 g++).

# --- Toolchain -----------------------------------------------------------
# Override on command line if your compilers live elsewhere, e.g.
#   make CXX32=/c/msys64/mingw32/bin/g++.exe CXX64=/c/msys64/mingw64/bin/g++.exe
CXX32 ?= i686-w64-mingw32-g++
CXX64 ?= x86_64-w64-mingw32-g++

# --- Flags ---------------------------------------------------------------
# -static-libgcc/-static-libstdc++ : self-contained DLL, no extra DLLs
# -s   : strip
# -O2  : optimise
CXXFLAGS := -O2 -s -std=c++17 -Wall -Wextra -Wno-unused-parameter \
            -static-libgcc -static-libstdc++ \
            -DUNICODE -D_UNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600

LDFLAGS := -shared \
           -Wl,--enable-stdcall-fixup -Wl,--kill-at \
           -static -static-libgcc -static-libstdc++

LIBS := -lcomctl32 -lshlwapi -luser32 -lkernel32 -lgdi32 -ladvapi32 \
        -lgdiplus -lshell32 -lcomdlg32 -lwinspool -ld2d1 -ldwrite -lwinhttp

SRCS := mdviewer.cpp parser.cpp renderer.cpp
DEF  := mdviewer.def

# --- Targets -------------------------------------------------------------
all: MDViewer.wlx MDViewer.wlx64 MDViewer-plugin.zip

MDViewer.wlx: $(SRCS) $(DEF)
	$(CXX32) $(CXXFLAGS) $(SRCS) $(DEF) -o $@ $(LDFLAGS) $(LIBS)

MDViewer.wlx64: $(SRCS) $(DEF)
	$(CXX64) $(CXXFLAGS) $(SRCS) $(DEF) -o $@ $(LDFLAGS) $(LIBS)

# Clean install package: pluginst.inf + both DLLs only.
# When the user clicks this ZIP inside Total Commander, TC sees pluginst.inf
# at the root and offers to install the plugin.
MDViewer-plugin.zip: MDViewer.wlx MDViewer.wlx64 pluginst.inf
	rm -f MDViewer-plugin.zip
	zip -j MDViewer-plugin.zip pluginst.inf MDViewer.wlx MDViewer.wlx64

clean:
	rm -f MDViewer.wlx MDViewer.wlx64 MDViewer-plugin.zip plugin.zip *.o

.PHONY: all clean
