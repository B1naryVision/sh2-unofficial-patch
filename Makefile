CXX      = i686-w64-mingw32-g++
CXXFLAGS = -m32 -O2 -std=c++17 -Wall -Isrc
LDFLAGS  = -shared -Wl,--enable-stdcall-fixup -static-libgcc -static-libstdc++ -lkernel32 -luser32 -lgdi32
TARGET   = d3d9.dll
DEF      = d3d9.def

SRCS = src/dllmain.cpp \
       src/core/hook.cpp \
       src/core/log.cpp \
       src/proxy/d3d9Proxy.cpp \
       src/patches/registry.cpp \
       src/patches/knightCatapultCrash.cpp \
       src/patches/mpAiEnable.cpp \
       src/patches/introSkip.cpp \
       src/patches/mpConnectCompleteCrash.cpp \
       src/patches/endgameStats.cpp \
       src/patches/barracksCrash.cpp \
       src/patches/stopTroopsHotkey.cpp \
       src/patches/attackHotkey.cpp

DEPLOY_PATH = /mnt/c/Games/Steam/steamapps/common/Stronghold\ 2/

all: $(TARGET)

$(TARGET): $(SRCS) $(DEF)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(DEF) $(LDFLAGS)

debug: CXXFLAGS += -DDEBUG -g
debug: $(TARGET)

deploy: $(TARGET)
	cp $(TARGET) $(DEPLOY_PATH)

clean:
	rm -f $(TARGET)

.PHONY: all debug deploy clean
