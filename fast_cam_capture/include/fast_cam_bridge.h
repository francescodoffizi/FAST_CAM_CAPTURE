#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t cam_id;        // 0: Front, 1: Right, 2: Rear, 3: Left
    uint32_t width;         // 1920
    uint32_t height;        // 1300
    uint32_t stride;        // 3840 (bytes per row)
    uint64_t timestamp_ns;  // Hardware capture timestamp in nanoseconds
    const uint8_t* pixels;  // Direct Zero-Copy mapped pointer in RAM (no memcpy)
} FastCamFrame;

// Client handle opaque structure
typedef struct FastCamClientCtx FastCamClientCtx;

FastCamClientCtx* fast_cam_client_create(void);
void fast_cam_client_destroy(FastCamClientCtx* ctx);

bool fast_cam_client_connect(FastCamClientCtx* ctx, const char* sock_path);
void fast_cam_client_disconnect(FastCamClientCtx* ctx);

// Waits for the next hardware frame from any active camera (timeout in milliseconds)
bool fast_cam_client_wait_frame(FastCamClientCtx* ctx, FastCamFrame* out_frame, int timeout_ms);

#ifdef __cplusplus
}

// Convenient C++ RAII Wrapper for Android / NDK integration
class FastCamClient {
public:
    FastCamClient() : m_ctx(fast_cam_client_create()) {}
    ~FastCamClient() { fast_cam_client_destroy(m_ctx); }

    bool connect(const char* sock_path = "/data/local/tmp/fast_cam.sock") {
        return fast_cam_client_connect(m_ctx, sock_path);
    }

    void disconnect() {
        fast_cam_client_disconnect(m_ctx);
    }

    bool waitForFrame(FastCamFrame* out_frame, int timeout_ms = 100) {
        return fast_cam_client_wait_frame(m_ctx, out_frame, timeout_ms);
    }

private:
    FastCamClientCtx* m_ctx;
};
#endif
