CXX      = i686-w64-mingw32-g++
CXXFLAGS = -m32 -O2 -std=c++17 -Wall -Wextra -Isrc
# --no-insert-timestamp zeroes the PE header timestamp so identical source
# and toolchain produce a bit-identical DLL (reproducible builds).
LDFLAGS  = -shared -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -static-libgcc -static-libstdc++ -lkernel32 -luser32 -lgdi32
TARGET   = d3d9.dll
DEF      = d3d9.def

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
       src/patches/shiftRecruit.cpp \
       src/patches/siegeCampHotkey.cpp \
       src/patches/autoMarket/autoMarket.cpp \
       src/patches/autoMarket/overlay.cpp \
       src/patches/settingsOverlay.cpp

HDRS = $(wildcard src/*.h src/core/*.h src/proxy/*.h src/patches/*.h src/patches/endgameStats/*.h src/patches/autoMarket/*.h)

DEPLOY_PATH = /mnt/c/Games/Steam/steamapps/common/Stronghold\ 2/

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS) $(DEF)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(DEF) $(LDFLAGS)

debug: CXXFLAGS += -DDEBUG -g
debug: $(TARGET)

deploy: $(TARGET)
	cp $(TARGET) $(DEPLOY_PATH)

clean:
	rm -f $(TARGET)

.PHONY: all debug deploy clean
