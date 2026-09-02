NDK_PATH ?= $(HOME)/Library/Android/sdk/ndk/26.1.10909125
TOOLCHAIN ?= $(NDK_PATH)/toolchains/llvm/prebuilt/darwin-x86_64
CC := $(TOOLCHAIN)/bin/aarch64-linux-android30-clang
CXX := $(TOOLCHAIN)/bin/aarch64-linux-android30-clang++

CFLAGS := -O3 -Wall -Wextra -Iinclude -D_GNU_SOURCE
CXXFLAGS := -O3 -Wall -Wextra -Iinclude -D_GNU_SOURCE -fPIC
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
	$(CXX) $(CXXFLAGS) -shared $^ -o $@
	@echo "[+] Built shared library: $@"

clean:
	rm -rf build

.PHONY: all clean
