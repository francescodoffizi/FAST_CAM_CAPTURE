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

In `app/src/main/cpp/camera/qcarcam_bridge.cpp`, replace the socket stream reading loop with the `FastCamClient` API.

The client automatically connects to the abstract socket `@fast_cam.sock` (which bypasses Android SELinux `Permission denied` restrictions) and supports both individual cameras (`0..3`) and the **2x2 Mosaic mode** (`4`):

```cpp
#include "fast_cam_bridge.h"

// 2x2 Grid Compositor in UYVY (composes 4 cameras into 1920x1300 at 30 FPS)
static void compose_2x2_uyvy(
    const uint8_t* cam0, const uint8_t* cam1,
    const uint8_t* cam3, const uint8_t* cam2,
    uint8_t* out_grid
) {
    const int half_w = FRAME_WIDTH / 2; // 960
    const int half_h = FRAME_HEIGHT / 2; // 650
    const int out_stride = FRAME_WIDTH * 2; // 3840 bytes
    const int in_stride  = FRAME_WIDTH * 2; // 3840 bytes

    for (int y = 0; y < half_h; y++) {
        int src_y = y * 2;
        const uint32_t* src0 = (const uint32_t*)(cam0 + src_y * in_stride);
        const uint32_t* src1 = (const uint32_t*)(cam1 + src_y * in_stride);
        uint32_t* dst_top = (uint32_t*)(out_grid + y * out_stride);

        // Top-Left: Cam 0 (Front), Top-Right: Cam 1 (Right)
        for (int x = 0; x < half_w / 2; x++) dst_top[x] = src0[x * 2];
        for (int x = 0; x < half_w / 2; x++) dst_top[half_w / 2 + x] = src1[x * 2];

        // Bottom-Left: Cam 3 (Left), Bottom-Right: Cam 2 (Rear)
        const uint32_t* src3 = (const uint32_t*)(cam3 + src_y * in_stride);
        const uint32_t* src2 = (const uint32_t*)(cam2 + src_y * in_stride);
        uint32_t* dst_bot = (uint32_t*)(out_grid + (half_h + y) * out_stride);

        for (int x = 0; x < half_w / 2; x++) dst_bot[x] = src3[x * 2];
        for (int x = 0; x < half_w / 2; x++) dst_bot[half_w / 2 + x] = src2[x * 2];
    }
}

// Inside the stream receiver thread:
void* stream_thread_func(void* arg) {
    FastCamClient client;
    
    // Connect to the abstract socket @fast_cam.sock (SELinux-safe)
    while (g_streaming.load() && !client.connect("@fast_cam.sock")) {
        usleep(300000); // Retry connection
    }

    FastCamFrame frame;
    const uint8_t* cam_ptrs[4] = { nullptr, nullptr, nullptr, nullptr };
    static uint8_t mosaic_buf[FRAME_WIDTH * FRAME_HEIGHT * 2];

    while (g_streaming.load()) {
        if (!client.waitForFrame(&frame, 100)) {
            continue;
        }

        if (frame.cam_id < 4 && frame.pixels) {
            cam_ptrs[frame.cam_id] = frame.pixels;
        }

        int desired_cam = g_active_camera.load();
        const uint8_t* render_pixels = nullptr;

        if (desired_cam == 4) {
            // 2x2 Mosaic Mode (All 4 cameras combined)
            const uint8_t* p0 = cam_ptrs[0] ? cam_ptrs[0] : frame.pixels;
            const uint8_t* p1 = cam_ptrs[1] ? cam_ptrs[1] : p0;
            const uint8_t* p2 = cam_ptrs[2] ? cam_ptrs[2] : p0;
            const uint8_t* p3 = cam_ptrs[3] ? cam_ptrs[3] : p0;

            compose_2x2_uyvy(p0, p1, p3, p2, mosaic_buf);
            render_pixels = mosaic_buf;
        } else if (desired_cam >= 0 && desired_cam < 4) {
            // Specific single camera channel
            if ((int)frame.cam_id == desired_cam) {
                render_pixels = frame.pixels;
            }
        } else {
            render_pixels = frame.pixels;
        }

        if (render_pixels) {
            std::lock_guard<std::mutex> lock(g_winMutex);
            if (g_nativeWindow) {
                ANativeWindow_Buffer winBuffer;
                if (ANativeWindow_lock(g_nativeWindow, &winBuffer, nullptr) == 0) {
                    convert_uyvy_to_rgba(render_pixels, FRAME_WIDTH, FRAME_HEIGHT, 
                                         (uint32_t*)winBuffer.bits, winBuffer.stride);
                    ANativeWindow_unlockAndPost(g_nativeWindow);
                }
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

---

## 6. Dual-Pipeline Support: Live Monitoring vs 4K Ultra-HD Archiving

`libfast_cam_client.so` provides high-performance C/C++ compositing routines directly out-of-the-box:

1. **Standard Decimated 2x2 Mosaic (`FastCamClient::compose2x2`)**:
   * Output: `1920x1300` UYVY canvas (downsampled by $2\times$ per camera).
   * Intended for: Pad UI Live View, Web Remote Streaming, and 720p/1080p encoders (`c2.qti.avc.encoder`).
2. **4K Ultra-HD Native Mosaic (`FastCamClient::compose4K`)**:
   * Output: `3840x2600` UYVY canvas preserving **100% of native sensor pixels** (zero downsampling).
   * Execution time: **~1.1 ms** via contiguous row `memcpy`.
   * Intended for: Local Dashcam & Sentry Storage with Hardware HEVC encoder (`c2.qti.hevc.encoder` at 12 Mbps) for license plate and facial detail clarity.

