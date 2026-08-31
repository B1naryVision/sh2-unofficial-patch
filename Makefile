CXX      = i686-w64-mingw32-g++
WINDRES  = i686-w64-mingw32-windres
CXXFLAGS = -m32 -O2 -std=c++17 -Wall -Wextra -Isrc
# -s strips the ~270 KB of DWARF debug sections a release build has no use for
# (the debug target puts it back). --dynamicbase and --nxcompat set the ASLR and
# DEP bits: a DLL shipping with DllCharacteristics of 0 reads as a legacy or
# hand-built binary to reputation and antivirus heuristics, and this one already
# has enough working against it. --no-insert-timestamp zeroes the PE header
# timestamp so identical source and toolchain produce a bit-identical DLL
# (reproducible builds).
STRIP    = -s
LDFLAGS  = -shared -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp \
           -Wl,--dynamicbase -Wl,--nxcompat $(STRIP) \
           -static-libgcc -static-libstdc++ -lkernel32 -luser32 -lgdi32
TARGET   = d3d9.dll
DEF      = d3d9.def
RC       = src/d3d9.rc
RES      = src/d3d9.res.o

SRCS = src/dllmain.cpp \
       src/core/config.cpp \
       src/core/hotkey.cpp \
       src/core/keybindWidget.cpp \
       src/core/overlayPanel.cpp \
       src/core/hook.cpp \
       src/core/log.cpp \
       src/core/frameTick.cpp \
       src/core/d3dHook.cpp \
       src/proxy/d3d9Proxy.cpp \
       src/patches/registry.cpp \
       src/patches/knightCatapultCrash.cpp \
       src/patches/mpAiEnable.cpp \
       src/patches/introSkip.cpp \
       src/patches/mpConnectCompleteCrash.cpp \
       src/patches/lobbyInProgressFilter.cpp \
       src/patches/pingCommand.cpp \
       src/patches/endgameStats.cpp \
       src/patches/endgameStats/unitTracker.cpp \
       src/patches/endgameStats/session.cpp \
       src/patches/endgameStats/collect.cpp \
       src/patches/endgameStats/overlay.cpp \
       src/patches/endgameStats/debugDump.cpp \
       src/patches/barracksCrash.cpp \
       src/patches/mapEdgeCrash.cpp \
       src/patches/stopTroopsHotkey.cpp \
       src/patches/attackHotkey.cpp \
       src/patches/zoomSpeed.cpp \
       src/patches/panSpeed.cpp \
       src/patches/zoomLimit.cpp \
       src/patches/shiftRecruit.cpp \
       src/patches/siegeCampHotkey.cpp \
       src/patches/autoMarket/autoMarket.cpp \
       src/patches/autoMarket/overlay.cpp \
       src/patches/settingsOverlay.cpp

HDRS = $(wildcard src/*.h src/core/*.h src/proxy/*.h src/patches/*.h src/patches/endgameStats/*.h src/patches/autoMarket/*.h)

DEPLOY_PATH = /mnt/c/Games/Steam/steamapps/common/Stronghold\ 2/

# Standalone map-unlocker tool for end users: a dependency-free Win32 exe,
# built separately from the DLL (`make tool`).
TOOL       = tools/win/sh2-map-unlocker.exe
TOOL_SRC   = tools/win/s2mEditable.cpp
TOOL_RC    = tools/win/s2mEditable.rc
TOOL_RES   = tools/win/s2mEditable.res.o
TOOL_TEST  = tools/win/test-s2m.exe
TOOLFLAGS  = -m32 -Os -std=c++17 -Wall -Wextra -municode -fno-exceptions -fno-rtti \
             -s -Wl,--no-insert-timestamp -static-libgcc -static-libstdc++
TOOLLIBS   = -lcomdlg32 -lshell32 -lole32 -luser32

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) $(DEF) $(RES)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(DEF) $(RES) $(LDFLAGS)

# The version resource is what a scanner and the file properties dialog read to
# find out whose software this is. See the comment in src/d3d9.rc.
$(RES): $(RC)
	$(WINDRES) -i $< -o $@ -O coff

debug: CXXFLAGS += -DDEBUG -g
debug: STRIP =
debug: $(TARGET)

deploy: $(TARGET)
	cp $(TARGET) $(DEPLOY_PATH)

tool: $(TOOL)

# The version resource, manifest and icon are not cosmetic: an exe with no
# metadata at all is what SmartScreen and AV heuristics like least.
$(TOOL_RES): $(TOOL_RC) tools/win/s2mEditable.manifest tools/win/s2mEditable.ico
	$(WINDRES) -i $< -o $@ -O coff

$(TOOL): $(TOOL_SRC) $(TOOL_RES)
	$(CXX) $(TOOLFLAGS) -mwindows -o $@ $(TOOL_SRC) $(TOOL_RES) $(TOOLLIBS)

# Console harness over the same parse/rebuild code, so the file handling can be
# regression-tested headlessly. Not shipped.
$(TOOL_TEST): tools/win/test_s2mEditable.cpp $(TOOL_SRC)
	$(CXX) $(TOOLFLAGS) -Wno-unused-function -mconsole -o $@ $< $(TOOLLIBS)

tool-test: $(TOOL_TEST)

clean:
	rm -f $(TARGET) $(RES) $(TOOL) $(TOOL_RES) $(TOOL_TEST)

.PHONY: all debug deploy tool tool-test clean
