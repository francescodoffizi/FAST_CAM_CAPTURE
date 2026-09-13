NDK_PATH ?= $(HOME)/Library/Android/sdk/ndk/26.1.10909125
TOOLCHAIN ?= $(NDK_PATH)/toolchains/llvm/prebuilt/darwin-x86_64
CC := $(TOOLCHAIN)/bin/aarch64-linux-android30-clang
CXX := $(TOOLCHAIN)/bin/aarch64-linux-android30-clang++
STRIP := $(TOOLCHAIN)/bin/llvm-strip

CFLAGS := -O3 -Wall -Wextra -Iinclude -D_GNU_SOURCE -fvisibility=hidden
CXXFLAGS := -O3 -Wall -Wextra -Iinclude -D_GNU_SOURCE -fPIC -fvisibility=hidden
LDFLAGS := -ldl -llog -Wl,-rpath,/vendor/lib64 -Wl,-rpath,/system/lib64

SERVER_TARGET := build/fast_cam_capture
CLIENT_TARGET := build/fast_cam_client
LIB_TARGET    := build/libfast_cam_client.so

all: $(SERVER_TARGET) $(CLIENT_TARGET) $(LIB_TARGET)

$(SERVER_TARGET): src/main.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "[+] Built server target: $@"

$(CLIENT_TARGET): src/fast_cam_client.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "[+] Built client target: $@"

$(LIB_TARGET): src/fast_cam_bridge.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -shared -Wl,-soname,libfast_cam_client.so $^ -o $@
	@echo "[+] Built shared library with SONAME: $@"

# Hardened release build: aggressive stripping of all symbols, debug sections, and symbol tables
release: all
	@echo "[*] Hardening & stripping ELF binaries..."
	$(STRIP) --strip-all --discard-all $(SERVER_TARGET)
	$(STRIP) --strip-all --discard-all $(CLIENT_TARGET)
	$(STRIP) --strip-unneeded $(LIB_TARGET)
	@echo "[+] Stripping complete. Binaries hardened against reverse engineering."

clean:
	rm -rf build

.PHONY: all release clean
