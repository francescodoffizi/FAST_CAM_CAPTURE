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
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "fast_cam_ipc.h"
#include "fd_passing.h"
#include "obfuscate.h"

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
    const char* sock_path = FAST_CAM_IPC_SOCKET_PATH;
    int test_duration = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            test_duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        }
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("====================================================\n");
    printf(" Fast QCarCam Zero-Copy Consumer Client (C Test)\n");
    printf(" Connecting to : %s\n", sock_path);
    printf(" Duration      : %d seconds\n", test_duration);
    printf("====================================================\n");

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[-] socket");
        return 1;
    }

    struct sockaddr_un saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    strncpy(saddr.sun_path, sock_path, sizeof(saddr.sun_path) - 1);

    printf("[*] Connecting to server...\n");
    int retry = 0;
    while (connect(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        if (++retry > 30) {
            fprintf(stderr, "[-] Could not connect to %s: %s\n", sock_path, strerror(errno));
            close(sock);
            return 1;
        }
        usleep(100000); // 100ms
    }
    printf("[+] Connected to fast_cam_capture server!\n");

    // 1. Receive handshake with shared memory buffer FDs
    fast_cam_handshake_multi_resp_t hs;
    int fds[FAST_CAM_MAX_TOTAL_BUFS];
    int num_fds = 0;

    int r = recv_fds(sock, fds, FAST_CAM_MAX_TOTAL_BUFS, &num_fds, &hs, sizeof(hs));
    if (r <= 0 || hs.magic != op_fcam_magic() || hs.msg_type != FAST_CAM_MSG_HANDSHAKE_RESP) {
        fprintf(stderr, "[-] Handshake failed (r=%d, magic=0x%x, fds=%d)\n", r, hs.magic, num_fds);
        close(sock);
        return 1;
    }

    printf("[+] Received %d shared ION file descriptors across %d streams via SCM_RIGHTS:\n",
           num_fds, hs.num_streams);
    for (uint32_t s = 0; s < hs.num_streams; s++) {
        printf("    Stream [%u]: Cam %u, %ux%u (stride=%u), %u buffers\n",
               s, hs.streams[s].cam_id, hs.streams[s].width, hs.streams[s].height,
               hs.streams[s].stride, hs.streams[s].num_buffers);
    }

    // 2. Mmap all shared ION buffers into client address space
    void* mapped_ptrs[FAST_CAM_MAX_TOTAL_BUFS];
    for (int i = 0; i < num_fds; i++) {
        mapped_ptrs[i] = mmap(NULL, 4992000, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        if (mapped_ptrs[i] == MAP_FAILED) {
            fprintf(stderr, "[-] mmap failed for fd=%d: %s\n", fds[i], strerror(errno));
            close(sock);
            return 1;
        }
    }
    printf("[+] All %d buffers Zero-Copy mapped into client RAM!\n", num_fds);

    printf("\n[+] Starting Zero-Copy Frame Ingestion...\n");
    printf("%-12s %-10s %-12s %-12s %-14s\n", "Elapsed", "Frames", "Inst. FPS", "Avg FPS", "Camera ID");
    printf("--------------------------------------------------------------------\n");

    uint64_t start_ns = get_time_ns();
    uint64_t last_report_ns = start_ns;
    uint32_t total_frames = 0;
    uint32_t window_frames = 0;

    while (g_running) {
        fast_cam_frame_msg_t msg;
        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;

        ssize_t n = recv(sock, &msg, sizeof(msg), MSG_WAITALL);
        if (n <= 0) {
            printf("[-] Server closed connection.\n");
            break;
        }
        if (msg.magic != op_fcam_magic() || msg.msg_type != FAST_CAM_MSG_FRAME_READY) continue;

        total_frames++;
        window_frames++;

        uint64_t now_ns = get_time_ns();
        if (now_ns - last_report_ns >= 1000000000ULL) {
            double elapsed_s = (double)(now_ns - start_ns) / 1e9;
            double win_s = (double)(now_ns - last_report_ns) / 1e9;
            double inst_fps = (double)window_frames / win_s;
            double avg_fps = (double)total_frames / elapsed_s;

            printf("[%5.1fs]     %-10u %-12.2f %-12.2f Cam %-10u\n",
                   elapsed_s, total_frames, inst_fps, avg_fps, msg.cam_id);

            window_frames = 0;
            last_report_ns = now_ns;

            if (test_duration > 0 && elapsed_s >= test_duration) {
                break;
            }
        }
    }

    uint64_t end_ns = get_time_ns();
    double total_time_s = (double)(end_ns - start_ns) / 1e9;
    double final_fps = total_time_s > 0 ? ((double)total_frames / total_time_s) : 0;

    printf("\n====================================================\n");
    printf(" Consumer Benchmark Results:\n");
    printf(" Total Ingested Frames : %u\n", total_frames);
    printf(" Elapsed Time          : %.2f seconds\n", total_time_s);
    printf(" Ingestion Framerate   : %.2f FPS (Zero-Copy)\n", final_fps);
    printf("====================================================\n");

    for (int i = 0; i < num_fds; i++) {
        munmap(mapped_ptrs[i], 4992000);
        close(fds[i]);
    }
    close(sock);
    return 0;
}
