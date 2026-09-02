#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "qcarcam_types.h"
#include "fast_cam_ipc.h"
#include "fd_passing.h"
#include "obfuscate.h"

#define FRAME_W       1920
#define FRAME_H       1300
#define FRAME_STRIDE  (FRAME_W * 2)
#define FRAME_BYTES   (FRAME_W * FRAME_H * 2) // 4,992,000 bytes
#define NUM_BUFS      5
#define MAX_CAMS      4

// ION definitions
struct IonAllocData {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};
#define ION_IOC_MAGIC   'I'
#define ION_IOC_ALLOC   _IOWR(ION_IOC_MAGIC, 0, struct IonAllocData)


static volatile bool g_running = true;

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int ion_open(void) {
    char s_dev[16];
    int fd = open(DECRYPT_STR(ENC__dev_ion, s_dev), O_RDONLY | O_CLOEXEC);
    memset(s_dev, 0, sizeof(s_dev));
    return fd;
}

static int ion_alloc_buf(int ion_fd, size_t len, void** out_ptr) {
    if (ion_fd < 0) {
        void* p = malloc(len);
        if (!p) return -1;
        memset(p, 0x80, len);
        *out_ptr = p;
        return -1;
    }

    uint32_t masks[2];
    masks[0] = op_ion_heap_system();
    masks[1] = 1u << 1;

    for (size_t i = 0; i < 2; i++) {
        struct IonAllocData ad;
        memset(&ad, 0, sizeof(ad));
        ad.len          = (uint64_t)len;
        ad.heap_id_mask = masks[i];
        ad.flags        = 1; // ION_FLAG_CACHED

        if (ioctl(ion_fd, ION_IOC_ALLOC, &ad) < 0) {
            continue;
        }

        void* ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)ad.fd, 0);
        if (ptr == MAP_FAILED) {
            close((int)ad.fd);
            continue;
        }

        memset(ptr, 0x80, len);
        *out_ptr = ptr;
        return (int)ad.fd;
    }

    void* p = malloc(len);
    if (!p) return -1;
    memset(p, 0x80, len);
    *out_ptr = p;
    return -1;
}

static void dummy_event_cb(void* hndl, unsigned int event_id, void* payload) {
    (void)payload;
    (void)hndl;
    (void)event_id;
}

// Function pointers to libais_client.so
static pfn_qcarcam_initialize    qcarcam_initialize;
static pfn_qcarcam_uninitialize  qcarcam_uninitialize;
static pfn_qcarcam_open          qcarcam_open;
static pfn_qcarcam_close         qcarcam_close;
static pfn_qcarcam_s_buffers     qcarcam_s_buffers;
static pfn_qcarcam_s_param       qcarcam_s_param;
static pfn_qcarcam_start         qcarcam_start;
static pfn_qcarcam_stop          qcarcam_stop;
static pfn_qcarcam_get_frame     qcarcam_get_frame;
static pfn_qcarcam_release_frame qcarcam_release_frame;

typedef struct {
    int cam_id;
    qcarcam_hndl_t hndl;
    int buf_fds[NUM_BUFS];
    void* buf_ptrs[NUM_BUFS];
    qcarcam_buffer_t descriptors[NUM_BUFS];
    uint32_t latest_buf_idx;
    uint32_t frames_count;
    bool active;
    bool first_frame_ok;
} camera_channel_t;

// Ultra-fast 2x2 UYVY Compositor
// Top-Left: Cam 0 (Front), Top-Right: Cam 1 (Right)
// Bottom-Left: Cam 3 (Left), Bottom-Right: Cam 2 (Rear)
static void compose_2x2_uyvy(
    const uint8_t* cam0, const uint8_t* cam1,
    const uint8_t* cam3, const uint8_t* cam2,
    uint8_t* out_grid
) {
    const int half_w = FRAME_W / 2; // 960
    const int half_h = FRAME_H / 2; // 650
    const int out_stride = FRAME_W * 2; // 3840 bytes per row
    const int in_stride  = FRAME_STRIDE; // 3840 bytes per row

    for (int y = 0; y < half_h; y++) {
        int src_y = y * 2;
        const uint32_t* src0 = (const uint32_t*)(cam0 + src_y * in_stride);
        const uint32_t* src1 = (const uint32_t*)(cam1 + src_y * in_stride);
        uint32_t* dst_top = (uint32_t*)(out_grid + y * out_stride);

        // Top-Left: Cam 0
        for (int x = 0; x < half_w / 2; x++) {
            dst_top[x] = src0[x * 2];
        }
        // Top-Right: Cam 1
        for (int x = 0; x < half_w / 2; x++) {
            dst_top[half_w / 2 + x] = src1[x * 2];
        }

        // Bottom-Left: Cam 3
        const uint32_t* src3 = (const uint32_t*)(cam3 + src_y * in_stride);
        const uint32_t* src2 = (const uint32_t*)(cam2 + src_y * in_stride);
        uint32_t* dst_bot = (uint32_t*)(out_grid + (half_h + y) * out_stride);

        for (int x = 0; x < half_w / 2; x++) {
            dst_bot[x] = src3[x * 2];
        }
        // Bottom-Right: Cam 2
        for (int x = 0; x < half_w / 2; x++) {
            dst_bot[half_w / 2 + x] = src2[x * 2];
        }
    }
}

int main(int argc, char* argv[]) {
    int single_cam = 0;
    bool all_cams = false;
    int duration_sec = 0; // Default: continuous daemon execution
    int record_fps = 30;
    const char* record_file = NULL;
    const char* grid_file = NULL;
    const char* sock_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cam") == 0 && i + 1 < argc) {
            single_cam = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--all") == 0) {
            all_cams = true;
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            record_fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_file = argv[++i];
        } else if (strcmp(argv[i], "--grid2x2") == 0 && i + 1 < argc) {
            grid_file = argv[++i];
            all_cams = true;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --cam <id>        Camera ID to capture [default: 0]\n");
            printf("  --all             Open all 4 cameras (0:Front, 1:Right, 2:Rear, 3:Left)\n");
            printf("  --grid2x2 <file>  Compose and record 4 cameras into 2x2 grid video (1920x1300)\n");
            printf("  --time <sec>      Duration in seconds (0 = continuous daemon) [default: 0]\n");
            printf("  --fps <fps>       Target recording FPS (1..30) [default: 30]\n");
            printf("  --record <file>   Record stream to raw UYVY video file\n");
            printf("  --socket <path>   IPC socket path or @abstract_name\n");
            return 0;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int active_cams[MAX_CAMS];
    int num_active = 0;

    if (all_cams) {
        active_cams[0] = 0; // Front
        active_cams[1] = 1; // Right
        active_cams[2] = 2; // Rear
        active_cams[3] = 3; // Left
        num_active = 4;
    } else {
        active_cams[0] = single_cam;
        num_active = 1;
    }

    printf("====================================================\n");
    printf(" Fast QCarCam Multi-Stream Engine (SA8155P DiLink 5.0)\n");
    printf(" Active Cameras: %d (", num_active);
    for (int i = 0; i < num_active; i++) printf("%d%s", active_cams[i], (i + 1 < num_active) ? ", " : "");
    printf(")\n");
    printf(" Mode          : %s\n", grid_file ? "2x2 Composite Grid Recording" : "Independent Channels (Zero-Copy)");
    printf(" Duration      : %d seconds\n", duration_sec);
    printf("====================================================\n");

    // Anti-analysis & debugging protection
    check_anti_debug();

    // 1. Load libais_client.so (Obfuscated string decrypt)
    char s_lib[64];
    void* lib = dlopen(DECRYPT_STR(ENC_libais_client_so, s_lib), RTLD_NOW | RTLD_GLOBAL);
    memset(s_lib, 0, sizeof(s_lib));
    if (!lib) {
        lib = dlopen(DECRYPT_STR(ENC__vendor_lib64_libais_client_so, s_lib), RTLD_NOW | RTLD_GLOBAL);
        memset(s_lib, 0, sizeof(s_lib));
    }
    if (!lib) {
        return 1;
    }

    #define RESOLVE_OBF(fn, enc_var) do { \
        char s_sym[32]; \
        fn = (pfn_##fn)dlsym(lib, DECRYPT_STR(enc_var, s_sym)); \
        memset(s_sym, 0, sizeof(s_sym)); \
        if (!fn) { dlclose(lib); return 1; } \
    } while (0)

    RESOLVE_OBF(qcarcam_initialize, ENC_qcarcam_initialize);
    RESOLVE_OBF(qcarcam_uninitialize, ENC_qcarcam_uninitialize);
    RESOLVE_OBF(qcarcam_open, ENC_qcarcam_open);
    RESOLVE_OBF(qcarcam_close, ENC_qcarcam_close);
    RESOLVE_OBF(qcarcam_s_buffers, ENC_qcarcam_s_buffers);
    RESOLVE_OBF(qcarcam_s_param, ENC_qcarcam_s_param);
    RESOLVE_OBF(qcarcam_start, ENC_qcarcam_start);
    RESOLVE_OBF(qcarcam_stop, ENC_qcarcam_stop);
    RESOLVE_OBF(qcarcam_get_frame, ENC_qcarcam_get_frame);
    RESOLVE_OBF(qcarcam_release_frame, ENC_qcarcam_release_frame);

    int rc = (int)(intptr_t)qcarcam_initialize(NULL);
    if (rc != 0) {
        fprintf(stderr, "[-] Init failed: %d\n", rc);
        dlclose(lib);
        return 1;
    }
    printf("[+] Hardware init OK\n");

    int ion_fd = ion_open();
    camera_channel_t channels[MAX_CAMS];
    memset(channels, 0, sizeof(channels));

    int all_fds[FAST_CAM_MAX_TOTAL_BUFS];
    int total_fds = 0;

    // 2. Open each camera channel and allocate dedicated ION buffers
    for (int i = 0; i < num_active; i++) {
        int cid = active_cams[i];
        channels[i].cam_id = cid;
        channels[i].hndl = qcarcam_open((uint32_t)cid);
        if (!channels[i].hndl) {
            fprintf(stderr, "[-] Failed to open camera %d\n", cid);
            continue;
        }

        // Register event callback & subscribe
        struct { void* cb_func; void* cookie; } cb_param = { (void*)dummy_event_cb, NULL };
        qcarcam_s_param(channels[i].hndl, 1, &cb_param);
        uint32_t event_mask = 0xf;
        qcarcam_s_param(channels[i].hndl, 2, &event_mask);

        // Allocate 5 ION buffers
        for (int b = 0; b < NUM_BUFS; b++) {
            channels[i].buf_fds[b] = ion_alloc_buf(ion_fd, FRAME_BYTES, &channels[i].buf_ptrs[b]);
            all_fds[total_fds++] = channels[i].buf_fds[b];

            qcarcam_buffer_t* d = &channels[i].descriptors[b];
            memset(d, 0, sizeof(*d));
            d->width     = FRAME_W;
            d->height    = FRAME_H;
            d->stride    = FRAME_STRIDE;
            d->size      = FRAME_BYTES;
            d->fd        = (int64_t)channels[i].buf_fds[b];
            d->pVirtAddr = channels[i].buf_ptrs[b];
            d->buf_type  = 1;
        }

        qcarcam_buffers_t bufs_desc;
        bufs_desc.color_fmt = (qcarcam_color_fmt_t)op_qcarcam_uyvy(); // UYVY
        bufs_desc.flags     = 0;
        bufs_desc.pBuffers  = channels[i].descriptors;
        bufs_desc.n_buffers = NUM_BUFS;

        rc = qcarcam_s_buffers(channels[i].hndl, &bufs_desc);
        if (rc != 0) {
            fprintf(stderr, "[-] Buffer config failed for cam %d: %d\n", cid, rc);
            continue;
        }

        rc = qcarcam_start(channels[i].hndl);
        if (rc != 0) {
            fprintf(stderr, "[-] Start failed for cam %d: %d\n", cid, rc);
            continue;
        }

        channels[i].active = true;
        printf("[+] Camera %d started OK! (Handle: %p)\n", cid, channels[i].hndl);
    }

    // 3. Setup Zero-Copy IPC Server Socket (with Abstract Socket & SELinux Fallback)
    int ipc_server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    int ipc_client_sock = -1;
    bool is_abstract_sock = false;
    char bound_sock_path[108] = {0};

    if (ipc_server_sock >= 0) {
        struct sockaddr_un saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sun_family = AF_UNIX;
        socklen_t slen = 0;
        bool bound = false;

        char default_sock[64];
        DECRYPT_STR(ENC__data_local_tmp_fast_cam_sock, default_sock);
        const char* target_path = sock_path ? sock_path : default_sock;

        if (target_path[0] == '@') {
            is_abstract_sock = true;
            saddr.sun_path[0] = '\0';
            strncpy(saddr.sun_path + 1, target_path + 1, sizeof(saddr.sun_path) - 2);
            slen = sizeof(sa_family_t) + strlen(target_path);
            if (bind(ipc_server_sock, (struct sockaddr*)&saddr, slen) == 0) {
                bound = true;
                strncpy(bound_sock_path, target_path, sizeof(bound_sock_path) - 1);
            }
        } else {
            strncpy(saddr.sun_path, target_path, sizeof(saddr.sun_path) - 1);
            unlink(target_path);
            slen = sizeof(sa_family_t) + strlen(saddr.sun_path) + 1;
            if (bind(ipc_server_sock, (struct sockaddr*)&saddr, slen) == 0) {
                bound = true;
                strncpy(bound_sock_path, target_path, sizeof(bound_sock_path) - 1);
            }
        }

        // Automatic fallback on abstract socket @fast_cam.sock if filesystem socket failed (e.g. SELinux Enforcing)
        if (!bound) {
            memset(&saddr, 0, sizeof(saddr));
            saddr.sun_family = AF_UNIX;
            const char* abs_name = "fast_cam.sock";
            saddr.sun_path[0] = '\0';
            memcpy(saddr.sun_path + 1, abs_name, strlen(abs_name));
            slen = sizeof(sa_family_t) + 1 + strlen(abs_name);
            if (bind(ipc_server_sock, (struct sockaddr*)&saddr, slen) == 0) {
                bound = true;
                is_abstract_sock = true;
                snprintf(bound_sock_path, sizeof(bound_sock_path), "@%s", abs_name);
            } else {
                fprintf(stderr, "[-] Failed to bind abstract socket @%s: %s\n", abs_name, strerror(errno));
            }
        }

        memset(default_sock, 0, sizeof(default_sock));

        if (bound) {
            listen(ipc_server_sock, 2);
            fcntl(ipc_server_sock, F_SETFL, O_NONBLOCK);
            printf("[+] Zero-Copy Multi-Channel IPC Server initialized (%s)\n", bound_sock_path);
        }
    }

    // Optional Grid 2x2 or Single recording file
    FILE* fp_grid = NULL;
    uint8_t* grid_canvas = NULL;
    if (grid_file) {
        fp_grid = fopen(grid_file, "wb");
        grid_canvas = (uint8_t*)malloc(FRAME_BYTES);
        printf("[+] 2x2 Grid recording enabled: %s (1920x1300 @ %d FPS)\n", grid_file, record_fps);
    }

    FILE* fp_rec = NULL;
    if (record_file) {
        fp_rec = fopen(record_file, "wb");
        printf("[+] Single recording enabled: %s\n", record_file);
    }

    printf("\n%-12s %-10s %-12s %-12s\n", "Timestamp", "Frames", "Inst. FPS", "Avg FPS");
    printf("----------------------------------------------------\n");

    uint64_t start_ns = get_time_ns();
    uint64_t last_fps_report_ns = start_ns;
    uint64_t last_grid_save_ns = start_ns;
    uint32_t total_frames = 0;
    uint32_t fps_window_frames = 0;
    uint32_t grid_saved_frames = 0;

    uint64_t grid_interval_ns = 1000000000ULL / (record_fps > 0 ? record_fps : 30);

    while (g_running) {
        // Accept new IPC client and send all shared ION FDs via SCM_RIGHTS
        if (ipc_server_sock >= 0 && ipc_client_sock < 0) {
            ipc_client_sock = accept(ipc_server_sock, NULL, NULL);
            if (ipc_client_sock >= 0) {
                fast_cam_handshake_multi_resp_t hs;
                memset(&hs, 0, sizeof(hs));
                hs.magic       = op_fcam_magic();
                hs.msg_type    = FAST_CAM_MSG_HANDSHAKE_RESP;
                hs.num_streams = num_active;
                hs.total_fds   = total_fds;

                for (int i = 0; i < num_active; i++) {
                    hs.streams[i].cam_id       = channels[i].cam_id;
                    hs.streams[i].width        = FRAME_W;
                    hs.streams[i].height       = FRAME_H;
                    hs.streams[i].stride       = FRAME_STRIDE;
                    hs.streams[i].buffer_bytes = FRAME_BYTES;
                    hs.streams[i].num_buffers  = NUM_BUFS;
                    hs.streams[i].fd_start_idx = i * NUM_BUFS;
                }

                if (send_fds(ipc_client_sock, all_fds, total_fds, &hs, sizeof(hs)) == 0) {
                    printf("[+] Android / Consumer Client connected! Sent %d shared ION FDs.\n", total_fds);
                } else {
                    close(ipc_client_sock);
                    ipc_client_sock = -1;
                }
            }
        }

        // Poll each active camera
        for (int i = 0; i < num_active; i++) {
            if (!channels[i].active) continue;

            qcarcam_frame_info_t fi;
            memset(&fi, 0, sizeof(fi));

            int ret = qcarcam_get_frame(channels[i].hndl, &fi, 1000000ULL, 0); // 1ms non-blocking poll
            if (ret != QCARCAM_RET_OK) continue;

            uint32_t idx = fi.buf_index;
            channels[i].latest_buf_idx = idx;
            channels[i].frames_count++;
            total_frames++;
            fps_window_frames++;

            // Forward lightweight 32-byte frame notification to IPC client
            if (ipc_client_sock >= 0) {
                fast_cam_frame_msg_t fmsg;
                fmsg.magic        = op_fcam_magic();
                fmsg.msg_type     = FAST_CAM_MSG_FRAME_READY;
                fmsg.cam_id       = channels[i].cam_id;
                fmsg.buf_index    = idx;
                fmsg.sequence_no  = fi.sequence_no;
                fmsg.width        = FRAME_W;
                fmsg.height       = FRAME_H;
                fmsg.timestamp_ns = fi.timestamp_ns;

                ssize_t sent = send(ipc_client_sock, &fmsg, sizeof(fmsg), MSG_NOSIGNAL);
                if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                    printf("[-] Consumer client disconnected.\n");
                    close(ipc_client_sock);
                    ipc_client_sock = -1;
                }
            }

            if (fp_rec && i == 0 && idx < NUM_BUFS) {
                fwrite(channels[i].buf_ptrs[idx], 1, FRAME_BYTES, fp_rec);
            }

            // Immediately release frame to hardware ring buffer
            qcarcam_release_frame(channels[i].hndl, idx);
        }

        // 2x2 Grid Compositor Tick
        uint64_t now_ns = get_time_ns();
        if (fp_grid && grid_canvas && (now_ns - last_grid_save_ns >= grid_interval_ns)) {
            // Need buffers from all 4 channels
            const uint8_t* p0 = (const uint8_t*)channels[0].buf_ptrs[channels[0].latest_buf_idx];
            const uint8_t* p1 = (num_active > 1) ? (const uint8_t*)channels[1].buf_ptrs[channels[1].latest_buf_idx] : p0;
            const uint8_t* p2 = (num_active > 2) ? (const uint8_t*)channels[2].buf_ptrs[channels[2].latest_buf_idx] : p0;
            const uint8_t* p3 = (num_active > 3) ? (const uint8_t*)channels[3].buf_ptrs[channels[3].latest_buf_idx] : p0;

            if (p0 && p1 && p2 && p3) {
                compose_2x2_uyvy(p0, p1, p3, p2, grid_canvas);
                fwrite(grid_canvas, 1, FRAME_BYTES, fp_grid);
                grid_saved_frames++;
                last_grid_save_ns = now_ns;
            }
        }

        // Report Stats Every Second
        if (now_ns - last_fps_report_ns >= 1000000000ULL) {
            double elapsed_sec = (double)(now_ns - start_ns) / 1000000000.0;
            double window_sec  = (double)(now_ns - last_fps_report_ns) / 1000000000.0;
            double inst_fps    = (double)fps_window_frames / window_sec;
            double avg_fps     = (double)total_frames / elapsed_sec;

            printf("[%5.1fs]     %-10u %-12.2f %-12.2f\n", elapsed_sec, total_frames, inst_fps, avg_fps);

            fps_window_frames = 0;
            last_fps_report_ns = now_ns;

            if (duration_sec > 0 && elapsed_sec >= duration_sec) {
                break;
            }
        }
    }

    uint64_t end_ns = get_time_ns();
    double total_time_sec = (double)(end_ns - start_ns) / 1000000000.0;
    double final_fps = total_time_sec > 0 ? ((double)total_frames / total_time_sec) : 0;

    printf("\n====================================================\n");
    printf(" Multi-Stream Benchmark Summary:\n");
    printf(" Active Channels       : %d\n", num_active);
    for (int i = 0; i < num_active; i++) {
        printf("   Camera %d           : %u frames (%.2f FPS)\n",
               channels[i].cam_id, channels[i].frames_count,
               total_time_sec > 0 ? (channels[i].frames_count / total_time_sec) : 0);
    }
    printf(" Total System Frames   : %u\n", total_frames);
    printf(" Aggregate Capture FPS : %.2f FPS (Target: %.0f FPS)\n", final_fps, num_active * 30.0);
    printf("====================================================\n");

    if (fp_grid) {
        fclose(fp_grid);
        free(grid_canvas);
        printf("[+] Saved 2x2 Grid Video: %u frames (%.2f MB) to %s\n",
               grid_saved_frames, (double)(grid_saved_frames * (uint64_t)FRAME_BYTES) / (1024.0 * 1024.0), grid_file);
    }
    if (fp_rec) fclose(fp_rec);

    // Stop streams & clean up
    for (int i = 0; i < num_active; i++) {
        if (channels[i].active) {
            qcarcam_stop(channels[i].hndl);
            qcarcam_close(channels[i].hndl);
        }
        for (int b = 0; b < NUM_BUFS; b++) {
            if (channels[i].buf_fds[b] >= 0) {
                munmap(channels[i].buf_ptrs[b], FRAME_BYTES);
                close(channels[i].buf_fds[b]);
            }
        }
    }

    if (ipc_client_sock >= 0) close(ipc_client_sock);
    if (ipc_server_sock >= 0) {
        close(ipc_server_sock);
        if (!is_abstract_sock && bound_sock_path[0] != '\0') {
            unlink(bound_sock_path);
        }
    }

    qcarcam_uninitialize();
    if (ion_fd >= 0) close(ion_fd);
    dlclose(lib);

    printf("[+] Done.\n");
    return 0;
}
