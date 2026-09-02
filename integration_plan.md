# Integration Guide: Replacing the Camera Daemon in Overdrive (DiLink 5.0 / SA8155P)

This guide describes how to replace the existing camera capture pipeline (`qcarcam_test` + `libhook_qcarcam.so`) with the new precompiled `fast_cam_capture` daemon and the `libfast_cam_client.so` client library.

---

## 1. Distribution Package Contents

```
overdrive_camera_pack/
├── bin/
│   └── fast_cam_capture              # Standalone precompiled native daemon (AArch64, Android API 30)
├── lib/
│   └── libfast_cam_client.so         # Precompiled client shared library for Android JNI (AArch64)
└── include/
    ├── fast_cam_bridge.h             # C/C++ header to include in the JNI module
    └── fast_cam_ipc.h                # IPC protocol header definitions
```

---

## 2. Replacing the Daemon on the Target Device

In the Android shell scripts or startup code running on the vehicle head unit:

### 2.1. Files to Remove from the Target
The following files are no longer needed on `/data/local/tmp/`:
* `qcarcam_test`
* `libhook_qcarcam.so`
* `4cam.xml` (and associated XML test configurations)

### 2.2. New Startup Command
Deploy `fast_cam_capture` to `/data/local/tmp/fast_cam_capture` (ensure executable permissions with `chmod 755`).

Update the startup command executed by the app or service:

```bash
#!/system/bin/sh
# Terminate any obsolete processes
killall -9 fast_cam_capture 2>/dev/null
killall -9 qcarcam_test 2>/dev/null

# Launch the new native daemon in the background
export LD_LIBRARY_PATH=/vendor/lib64:/system/lib64:/data/local/tmp
exec /data/local/tmp/fast_cam_capture --all
```

#### Available CLI Options:
* `--all`: Opens and streams all 4 cameras simultaneously (Front, Right, Rear, Left).
* `--cam <id>`: Opens a specific camera (`0`: Front, `1`: Right, `2`: Rear, `3`: Left).

---

## 3. Android Project Setup (NDK / JNI)

### 3.1. Library Placement
Copy `libfast_cam_client.so` into your project's native libraries directory:
* Path: `app/src/main/jniLibs/arm64-v8a/libfast_cam_client.so`

Copy the headers to:
* `app/src/main/cpp/include/fast_cam_bridge.h`
* `app/src/main/cpp/include/fast_cam_ipc.h`

### 3.2. Update `app/src/main/cpp/CMakeLists.txt`
Declare the imported shared library and link it against `qcarcam_bridge`:

```cmake
# Import the precompiled client library
add_library(fast_cam_client SHARED IMPORTED)
set_target_properties(fast_cam_client PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libfast_cam_client.so"
)

# Link fast_cam_client
target_link_libraries(qcarcam_bridge
    fast_cam_client
    android
    log
)
```

---

## 4. Updating `qcarcam_bridge.cpp`

In `app/src/main/cpp/camera/qcarcam_bridge.cpp`, replace the socket stream reading loop with the `FastCamClient` API:

```cpp
#include "fast_cam_bridge.h"

// Inside the stream receiver thread:
void* stream_thread_func(void* arg) {
    FastCamClient client;
    
    // Connect to the local daemon socket
    while (g_streaming.load() && !client.connect("/data/local/tmp/fast_cam.sock")) {
        usleep(300000); // Retry connection
    }

    FastCamFrame frame;
    while (g_streaming.load()) {
        // Wait for the next available hardware frame (timeout in milliseconds)
        if (!client.waitForFrame(&frame, 100)) {
            continue;
        }

        // Filter by requested camera ID (or process all active cameras)
        int desired_cam = g_active_camera.load();
        if (desired_cam >= 0 && frame.cam_id != (uint32_t)desired_cam) {
            continue;
        }

        // frame.pixels points directly to the 1920x1300 UYVY buffer ready for rendering
        std::lock_guard<std::mutex> lock(g_winMutex);
        if (g_nativeWindow) {
            ANativeWindow_Buffer winBuffer;
            if (ANativeWindow_lock(g_nativeWindow, &winBuffer, nullptr) == 0) {
                // Color conversion to Android Surface
                convert_uyvy_to_rgba(frame.pixels, frame.width, frame.height, 
                                     (uint32_t*)winBuffer.bits, winBuffer.stride);
                ANativeWindow_unlockAndPost(g_nativeWindow);
            }
        }
    }

    client.disconnect();
    return nullptr;
}
```

---

## 5. `FastCamFrame` Field Reference

The `FastCamFrame` structure populated by `client.waitForFrame(&frame)` provides the following fields:

| Field | Type | Description |
| :--- | :--- | :--- |
| `frame.cam_id` | `uint32_t` | Camera channel index (`0`=Front, `1`=Right, `2`=Rear, `3`=Left) |
| `frame.width` | `uint32_t` | Frame width in pixels (`1920`) |
| `frame.height` | `uint32_t` | Frame height in pixels (`1300`) |
| `frame.stride` | `uint32_t` | Row stride in bytes (`3840`) |
| `frame.timestamp_ns` | `uint64_t` | Hardware capture timestamp in nanoseconds |
| `frame.pixels` | `const uint8_t*` | Direct pointer to the raw **UYVY** pixel buffer |
