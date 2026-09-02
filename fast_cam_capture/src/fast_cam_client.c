#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include "fast_cam_ipc.h"
#include "fd_passing.h"

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

int main(int argc, char* argv[]) {
    int duration_sec = 10;
    const char* sock_path = FAST_CAM_IPC_SOCKET_PATH;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  --time <sec>   Duration in seconds [default: 10]\n");
            return 0;
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================\n");
    printf(" Fast Cam Zero-Copy Consumer Client (SCM_RIGHTS)\n");
    printf(" Connecting to: %s\n", sock_path);
    printf("====================================================\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    int retries = 0;
    while (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (!g_running) { close(sock); return 0; }
        if (++retries > 50) {
            fprintf(stderr, "[-] Connection timeout to %s: %s\n", sock_path, strerror(errno));
            close(sock);
            return 1;
        }
        usleep(100000); // 100ms
    }
    printf("[+] Connected to fast_cam_capture server!\n");

    // 1. Receive handshake with shared memory buffer FDs
    fast_cam_handshake_resp_t hs;
    int fds[FAST_CAM_MAX_BUFS];
    int num_fds = 0;

    int r = recv_fds(sock, fds, FAST_CAM_MAX_BUFS, &num_fds, &hs, sizeof(hs));
    if (r <= 0 || hs.magic != FAST_CAM_MAGIC || hs.msg_type != FAST_CAM_MSG_HANDSHAKE_RESP) {
        fprintf(stderr, "[-] Handshake failed (r=%d, magic=0x%x, fds=%d)\n", r, hs.magic, num_fds);
        close(sock);
        return 1;
    }

    printf("[+] Received %d shared ION file descriptors via SCM_RIGHTS:\n", num_fds);
    printf("    Stream Resolution : %ux%u (stride=%u)\n", hs.width, hs.height, hs.stride);
    printf("    Buffer Size       : %u bytes per slot (Zero-Copy mapped)\n", hs.buffer_bytes);

    // 2. Mmap all shared ION buffers into client address space
    void* mapped_ptrs[FAST_CAM_MAX_BUFS];
    for (int i = 0; i < num_fds; i++) {
        mapped_ptrs[i] = mmap(NULL, hs.buffer_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (mapped_ptrs[i] == MAP_FAILED) {
            fprintf(stderr, "[-] mmap failed for fd=%d: %s\n", fds[i], strerror(errno));
            close(sock);
            return 1;
        }
        printf("    Slot [%d]: fd=%d mapped at %p\n", i, fds[i], mapped_ptrs[i]);
    }

    printf("\n[+] Starting Zero-Copy Frame Ingestion (Target: 30 FPS)...\n");
    printf("%-12s %-10s %-12s %-12s %-14s\n", "Elapsed", "Frames", "Inst. FPS", "Avg FPS", "First Byte");
    printf("--------------------------------------------------------------------\n");

    uint64_t start_ns = get_time_ns();
    uint64_t last_report_ns = start_ns;
    uint32_t total_frames = 0;
    uint32_t window_frames = 0;

    while (g_running) {
        fast_cam_frame_msg_t msg;
        ssize_t n = recv(sock, &msg, sizeof(msg), MSG_WAITALL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            printf("[-] Connection closed by server\n");
            break;
        }

        if (msg.magic != FAST_CAM_MAGIC || msg.msg_type != FAST_CAM_MSG_FRAME_READY) {
            continue;
        }

        uint32_t idx = msg.buf_index;
        uint8_t sample_byte = 0;
        if (idx < (uint32_t)num_fds && mapped_ptrs[idx]) {
            // Read directly from mapped physical RAM with 0 copies!
            sample_byte = ((uint8_t*)mapped_ptrs[idx])[0];
        }

        total_frames++;
        window_frames++;

        uint64_t now_ns = get_time_ns();
        if (now_ns - last_report_ns >= 1000000000ULL) { // 1 sec
            double elapsed = (double)(now_ns - start_ns) / 1000000000.0;
            double window_sec = (double)(now_ns - last_report_ns) / 1000000000.0;
            double inst_fps = (double)window_frames / window_sec;
            double avg_fps = (double)total_frames / elapsed;

            printf("[%5.1fs]     %-10u %-12.2f %-12.2f 0x%02x\n",
                   elapsed, total_frames, inst_fps, avg_fps, sample_byte);

            window_frames = 0;
            last_report_ns = now_ns;

            if (duration_sec > 0 && elapsed >= duration_sec) {
                break;
            }
        }
    }

    uint64_t end_ns = get_time_ns();
    double total_time = (double)(end_ns - start_ns) / 1000000000.0;
    double final_fps = total_time > 0 ? ((double)total_frames / total_time) : 0;

    printf("\n====================================================\n");
    printf(" Consumer Performance Summary:\n");
    printf(" Total Delivered Frames : %u\n", total_frames);
    printf(" Total Test Time        : %.2f seconds\n", total_time);
    printf(" Effective Delivery FPS : %.2f FPS (Target 30 FPS)\n", final_fps);
    printf(" Mode                   : SCM_RIGHTS Zero-Copy\n");
    printf("====================================================\n");

    for (int i = 0; i < num_fds; i++) {
        munmap(mapped_ptrs[i], hs.buffer_bytes);
        close(fds[i]);
    }
    close(sock);
    return 0;
}
