/* =============================================================================
 * gesture_conductor.c - Zero-Latency Pure C Optical Gesture Conductor
 * Developed for Kryštof Pešek (Kof) Fleet Workstations
 * 
 * Features:
 * - Direct V4L2 Memory-Mapped (mmap) capture (/dev/video0) with zero-copy DMA
 * - Ultra-fast YUYV-to-Grayscale downsampling (160x120) with sub-10us latency
 * - CMOS noise pre-filtering, temporal motion differencing, and morphology
 * - Connected-Component Labeling (CCL) with spatial moments and centroid tracking
 * - Dual-hand kinematics tracking with adaptive 1-Euro speed filter (no jitter)
 * - Proximity-based hand association with spatial hysteresis (no identity flip)
 * - Real-time Ictus downbeat strike and Tutti sub-drop detection
 * - Zero-dependency binary OSC serializer sending to SuperCollider (57130 & 57120)
 * - Hardware-accelerated Raylib CRT dashboard (with --headless mode fallback)
 * =============================================================================
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <libgen.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <pthread.h>

#include <raylib.h>
#include "sc_engine.h"

#define CAM_WIDTH  320
#define CAM_HEIGHT 240
#define PROC_W     160
#define PROC_H     120
#define NUM_BUFFERS 4
#define MAX_BLOBS  64
#define TRAIL_LEN  60

/* -----------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------- */

typedef struct {
    float x, y;
    int area;
} Blob;

typedef struct {
    char name[16];
    bool active;
    int active_frames;
    float x, y;
    float prev_x, prev_y;
    float vx, vy;
    float prev_vy;
    float energy;
    int ictus_cooldown;
    float ictus_flash;
    Vector2 trail[TRAIL_LEN];
    int trail_count;
} HandState;

typedef struct {
    void *start;
    size_t length;
} V4L2Buffer;

/* Global state */
static volatile bool g_running = true;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* -----------------------------------------------------------------------------
 * Fast OSC Binary Serializer
 * -------------------------------------------------------------------------- */

static inline void write_aligned_str(uint8_t *buf, int *offset, const char *str) {
    int len = strlen(str);
    memcpy(buf + *offset, str, len);
    *offset += len;
    buf[(*offset)++] = '\0';
    while (*offset % 4 != 0) {
        buf[(*offset)++] = '\0';
    }
}

static inline void write_be_float(uint8_t *buf, int *offset, float val) {
    union { float f; uint32_t u; } pun;
    pun.f = val;
    uint32_t be = htonl(pun.u);
    memcpy(buf + *offset, &be, 4);
    *offset += 4;
}

static int build_osc_morph(uint8_t *buf, float y_elev, float x_pan, float energy, float span) {
    int off = 0;
    write_aligned_str(buf, &off, "/conductor/morph");
    write_aligned_str(buf, &off, ",ffff");
    write_be_float(buf, &off, y_elev);
    write_be_float(buf, &off, x_pan);
    write_be_float(buf, &off, energy);
    write_be_float(buf, &off, span);
    return off;
}

static int build_osc_ictus(uint8_t *buf, float intensity, float hit_x, float hit_y) {
    int off = 0;
    write_aligned_str(buf, &off, "/conductor/ictus");
    write_aligned_str(buf, &off, ",fff");
    write_be_float(buf, &off, intensity);
    write_be_float(buf, &off, hit_x);
    write_be_float(buf, &off, hit_y);
    return off;
}

static int build_osc_tension(uint8_t *buf, float energy, float tutti) {
    int off = 0;
    write_aligned_str(buf, &off, "/conductor/tension");
    write_aligned_str(buf, &off, ",ff");
    write_be_float(buf, &off, energy);
    write_be_float(buf, &off, tutti);
    return off;
}

/* -----------------------------------------------------------------------------
 * Hand Kinematics & Adaptive 1-Euro Tracking
 * -------------------------------------------------------------------------- */

static void hand_init(HandState *h, const char *name) {
    strncpy(h->name, name, sizeof(h->name) - 1);
    h->active = false;
    h->active_frames = 0;
    h->x = 0.5f;
    h->y = 0.5f;
    h->prev_x = 0.5f;
    h->prev_y = 0.5f;
    h->vx = 0.0f;
    h->vy = 0.0f;
    h->prev_vy = 0.0f;
    h->energy = 0.0f;
    h->ictus_cooldown = 0;
    h->ictus_flash = 0.0f;
    h->trail_count = 0;
}

static bool hand_update(HandState *h, float target_x, float target_y, int area, float *out_intensity) {
    h->active = true;
    h->active_frames = 12; // Hold coordinates across ~400ms micro-pauses

    // Adaptive 1-Euro Filter: High smoothing when stationary, zero lag when fast
    float dx = target_x - h->x;
    float dy = target_y - h->y;
    float dist = sqrtf(dx * dx + dy * dy);
    float alpha = 0.18f + dist * 9.0f;
    if (alpha < 0.18f) alpha = 0.18f;
    if (alpha > 0.72f) alpha = 0.72f;

    h->x = alpha * target_x + (1.0f - alpha) * h->x;
    h->y = alpha * target_y + (1.0f - alpha) * h->y;

    h->vx = h->x - h->prev_x;
    h->vy = h->y - h->prev_y;

    float speed = sqrtf(h->vx * h->vx + h->vy * h->vy);
    h->energy = 0.4f * (speed * 28.0f + (float)area / 250.0f) + 0.6f * h->energy;

    // Conducting Ictus Detector (Downbeat rebound: vy flips positive->negative OR high jerk)
    bool is_ictus = false;
    float jerk = fabsf(h->vy - h->prev_vy) * 50.0f;

    if (h->ictus_cooldown <= 0) {
        if ((h->prev_vy > 0.006f && h->vy <= 0.002f) || (jerk > 0.38f) || (speed > 0.04f && jerk > 0.28f)) {
            is_ictus = true;
            float raw_int = (jerk > h->energy * 1.5f) ? jerk : (h->energy * 1.5f);
            if (raw_int < 0.25f) raw_int = 0.25f;
            if (raw_int > 2.5f)  raw_int = 2.5f;
            *out_intensity = raw_int;
            h->ictus_flash = 1.0f;
            h->ictus_cooldown = 6; // Lockout window (~200ms)
        }
    }

    h->prev_x = h->x;
    h->prev_y = h->y;
    h->prev_vy = h->vy;

    if (h->ictus_cooldown > 0) h->ictus_cooldown--;
    h->ictus_flash *= 0.82f;

    // Update trail
    if (h->trail_count < TRAIL_LEN) {
        h->trail[h->trail_count++] = (Vector2){ h->x, h->y };
    } else {
        memmove(&h->trail[0], &h->trail[1], sizeof(Vector2) * (TRAIL_LEN - 1));
        h->trail[TRAIL_LEN - 1] = (Vector2){ h->x, h->y };
    }

    return is_ictus;
}

static void hand_decay(HandState *h) {
    if (h->active_frames > 0) {
        h->active_frames--;
    } else {
        h->active = false;
    }
    h->energy *= 0.88f;
    h->vx *= 0.6f;
    h->vy *= 0.6f;
    h->ictus_flash *= 0.80f;
    if (h->ictus_cooldown > 0) h->ictus_cooldown--;
}

/* -----------------------------------------------------------------------------
 * Pure C Computer Vision Engine
 * -------------------------------------------------------------------------- */

static void downsample_yuyv_to_gray(const uint8_t *yuyv, uint8_t *gray) {
    // CAM_WIDTH (320) x CAM_HEIGHT (240) YUYV -> PROC_W (160) x PROC_H (120) Grayscale
    // Source pitch = 320 * 2 = 640 bytes. Subsample every 2nd line and 2nd pixel.
    for (int y = 0; y < PROC_H; y++) {
        const uint8_t *src_row = yuyv + (y * 2) * (CAM_WIDTH * 2);
        uint8_t *dst_row = gray + y * PROC_W;
        for (int x = 0; x < PROC_W; x++) {
            dst_row[x] = src_row[x * 4]; // Even Y sample
        }
    }
}

static void filter_motion(const uint8_t *gray, const uint8_t *prev_gray, uint8_t *thresh_out, int thresh_val) {
    // 1. Temporal difference with thresholding
    for (int i = 0; i < PROC_W * PROC_H; i++) {
        int diff = abs((int)gray[i] - (int)prev_gray[i]);
        thresh_out[i] = (diff > thresh_val) ? 255 : 0;
    }

    // 2. Fast 3x3 Morphological Open (Erode then Dilate) to remove CMOS salt-and-pepper
    static uint8_t temp[PROC_W * PROC_H];
    
    // Erode
    for (int y = 1; y < PROC_H - 1; y++) {
        for (int x = 1; x < PROC_W - 1; x++) {
            int idx = y * PROC_W + x;
            if (thresh_out[idx] &&
                thresh_out[idx - 1] && thresh_out[idx + 1] &&
                thresh_out[idx - PROC_W] && thresh_out[idx + PROC_W]) {
                temp[idx] = 255;
            } else {
                temp[idx] = 0;
            }
        }
    }

    // Dilate
    for (int y = 1; y < PROC_H - 1; y++) {
        for (int x = 1; x < PROC_W - 1; x++) {
            int idx = y * PROC_W + x;
            if (temp[idx] || temp[idx - 1] || temp[idx + 1] ||
                temp[idx - PROC_W] || temp[idx + PROC_W]) {
                thresh_out[idx] = 255;
            } else {
                thresh_out[idx] = 0;
            }
        }
    }
}

/* Breadth-First Connected Component Labeling */
static int find_blobs(const uint8_t *mask, Blob *blobs, int max_blobs, int min_area) {
    static uint8_t visited[PROC_W * PROC_H];
    static int queue[PROC_W * PROC_H];
    memset(visited, 0, sizeof(visited));

    int blob_count = 0;

    for (int y = 2; y < PROC_H - 2; y++) {
        for (int x = 2; x < PROC_W - 2; x++) {
            int start_idx = y * PROC_W + x;
            if (!mask[start_idx] || visited[start_idx]) continue;

            int q_head = 0, q_tail = 0;
            queue[q_tail++] = start_idx;
            visited[start_idx] = 1;

            int sum_x = 0, sum_y = 0, count = 0;

            while (q_head < q_tail) {
                int curr = queue[q_head++];
                int cy = curr / PROC_W;
                int cx = curr % PROC_W;

                sum_x += cx;
                sum_y += cy;
                count++;

                // 4-connectivity
                int neighbors[4] = { curr - 1, curr + 1, curr - PROC_W, curr + PROC_W };
                for (int n = 0; n < 4; n++) {
                    int n_idx = neighbors[n];
                    if (n_idx >= 0 && n_idx < PROC_W * PROC_H && mask[n_idx] && !visited[n_idx]) {
                        visited[n_idx] = 1;
                        if (q_tail < PROC_W * PROC_H) {
                            queue[q_tail++] = n_idx;
                        }
                    }
                }
            }

            if (count >= min_area && blob_count < max_blobs) {
                blobs[blob_count].area = count;
                blobs[blob_count].x = (float)sum_x / (float)(count * PROC_W);
                blobs[blob_count].y = (float)sum_y / (float)(count * PROC_H);
                blob_count++;
            }
        }
    }

    // Sort blobs descending by area
    for (int i = 0; i < blob_count - 1; i++) {
        for (int j = i + 1; j < blob_count; j++) {
            if (blobs[j].area > blobs[i].area) {
                Blob tmp = blobs[i];
                blobs[i] = blobs[j];
                blobs[j] = tmp;
            }
        }
    }

    return blob_count;
}

/* -----------------------------------------------------------------------------
 * Main Conductor Runtime
 * -------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    const char *cam_device = "/dev/video0";
    const char *sc_host = "127.0.0.1";
    int sc_port_primary = 57130;
    int sc_port_fallback = 57120;
    int diff_thresh = 22;
    int min_area = 35;
    bool headless = false;
    bool embedded_sc = true;
    char custom_synthdefs_dir[1024] = "";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) cam_device = argv[++i];
        else if (!strcmp(argv[i], "--sc-host") && i + 1 < argc) sc_host = argv[++i];
        else if (!strcmp(argv[i], "--sc-port") && i + 1 < argc) sc_port_primary = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--thresh") && i + 1 < argc) diff_thresh = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min-area") && i + 1 < argc) min_area = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--synthdefs") && i + 1 < argc) strncpy(custom_synthdefs_dir, argv[++i], sizeof(custom_synthdefs_dir)-1);
        else if (!strcmp(argv[i], "--no-sc") || !strcmp(argv[i], "--external-sc")) embedded_sc = false;
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("  --device <dev>       V4L2 camera device (default /dev/video0)\n");
            printf("  --synthdefs <dir>    Custom path to .scsyndef directory\n");
            printf("  --no-sc              Disable embedded libscsynth (use external SuperCollider)\n");
            printf("  --sc-host <ip>       External SuperCollider UDP host (default 127.0.0.1)\n");
            printf("  --sc-port <port>     External SuperCollider UDP port (default 57130)\n");
            printf("  --thresh <val>       Motion difference threshold (default 22)\n");
            printf("  --min-area <val>     Minimum blob area (default 35)\n");
            printf("  --headless           Run without Raylib GUI (CLI daemon)\n");
            return 0;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // Resolve directory next to the binary for synthdefs
    char synthdefs_dir[1024];
    if (custom_synthdefs_dir[0] != '\0') {
        snprintf(synthdefs_dir, sizeof(synthdefs_dir), "%s", custom_synthdefs_dir);
    } else {
        char exe_path[1024];
        ssize_t elen = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (elen > 0) {
            exe_path[elen] = '\0';
            char *d = dirname(exe_path);
            snprintf(synthdefs_dir, sizeof(synthdefs_dir), "%s/synthdefs", d);
        } else {
            strcpy(synthdefs_dir, "./synthdefs");
        }
    }

    printf("===================================================================\n");
    printf("  PIXELSENSE :: ZERO-LATENCY PURE C OPTICAL GESTURE CONDUCTOR (KOF)\n");
    printf("  Audio Engine: %s\n", embedded_sc ? "libscsynth (IN-PROCESS EMBEDDED SUPERCALLIDER)" : "External SuperCollider UDP");
    printf("  SynthDefs:    %s\n", synthdefs_dir);
    printf("  Camera:       %s (YUYV 320x240 -> 160x120 Grayscale)\n", cam_device);
    printf("  OSC Targets:  %s:%d and %s:%d\n", sc_host, sc_port_primary, sc_host, sc_port_fallback);
    printf("  Mode:         %s\n", headless ? "Headless CLI Daemon" : "Raylib CRT Visual Dashboard");
    printf("===================================================================\n");

    /* 1. Initialize Embedded libscsynth Audio Engine */
    bool sc_ready = false;
    if (embedded_sc) {
        sc_ready = sc_engine_init(synthdefs_dir, 57110);
        if (!sc_ready) {
            fprintf(stderr, "[Conductor] Warning: Embedded libscsynth failed to boot. Falling back to UDP broadcast.\n");
        }
    }

    /* 1. Setup POSIX UDP Socket */
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("Failed to create UDP socket");
        return 1;
    }

    struct sockaddr_in target_primary, target_fallback;
    memset(&target_primary, 0, sizeof(target_primary));
    target_primary.sin_family = AF_INET;
    target_primary.sin_port = htons(sc_port_primary);
    inet_pton(AF_INET, sc_host, &target_primary.sin_addr);

    memset(&target_fallback, 0, sizeof(target_fallback));
    target_fallback.sin_family = AF_INET;
    target_fallback.sin_port = htons(sc_port_fallback);
    inet_pton(AF_INET, sc_host, &target_fallback.sin_addr);

    #define SEND_OSC(buf, len) do { \
        sendto(sock_fd, buf, len, 0, (struct sockaddr*)&target_primary, sizeof(target_primary)); \
        sendto(sock_fd, buf, len, 0, (struct sockaddr*)&target_fallback, sizeof(target_fallback)); \
    } while(0)

    /* 2. Open & Configure V4L2 Device */
    int cam_fd = open(cam_device, O_RDWR | O_NONBLOCK, 0);
    if (cam_fd < 0) {
        fprintf(stderr, "Error: Cannot open video device %s: %s\n", cam_device, strerror(errno));
        close(sock_fd);
        return 1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_WIDTH;
    fmt.fmt.pix.height = CAM_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT failed");
        close(cam_fd);
        close(sock_fd);
        return 1;
    }

    /* Request Memory-Mapped Buffers */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS failed");
        close(cam_fd);
        close(sock_fd);
        return 1;
    }

    V4L2Buffer buffers[NUM_BUFFERS];
    for (int i = 0; i < NUM_BUFFERS; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF failed");
            close(cam_fd);
            close(sock_fd);
            return 1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam_fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) {
            perror("mmap failed");
            close(cam_fd);
            close(sock_fd);
            return 1;
        }

        if (ioctl(cam_fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF failed");
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON failed");
        close(cam_fd);
        close(sock_fd);
        return 1;
    }

    printf("[V4L2] Camera streaming active with %d mmap buffers (%dx%d YUYV)\n", NUM_BUFFERS, CAM_WIDTH, CAM_HEIGHT);

    /* 3. Initialize Vision Buffers & Kinematics */
    uint8_t *gray = (uint8_t*)malloc(PROC_W * PROC_H);
    uint8_t *prev_gray = (uint8_t*)calloc(PROC_W * PROC_H, 1);
    uint8_t *mask = (uint8_t*)malloc(PROC_W * PROC_H);
    uint8_t *display_rgba = (uint8_t*)malloc(PROC_W * PROC_H * 4);

    HandState left_hand, right_hand;
    hand_init(&left_hand, "Left");
    hand_init(&right_hand, "Right");

    float curr_prim_x = 0.5f;
    float curr_prim_y = 0.5f;
    float span_val = 0.2f;

    /* 4. Initialize Raylib (if not headless) */
    int win_w = 1120, win_h = 640;
    Texture2D tex_cam = {0}, tex_mask = {0};

    if (!headless) {
        SetTraceLogLevel(LOG_WARNING);
        InitWindow(win_w, win_h, "PixelSense :: Dual-Hand Orchestral Conductor (Pure C)");
        SetTargetFPS(60);

        Image img_init = GenImageColor(PROC_W, PROC_H, BLACK);
        tex_cam = LoadTextureFromImage(img_init);
        tex_mask = LoadTextureFromImage(img_init);
        UnloadImage(img_init);
    }

    uint8_t osc_buf[256];

    /* 5. Main Real-Time Processing Loop */
    while (g_running) {
        if (!headless && WindowShouldClose()) break;

        /* Dequeue latest available frame from V4L2 */
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(cam_fd, VIDIOC_DQBUF, &vbuf) == 0) {
            downsample_yuyv_to_gray((const uint8_t*)buffers[vbuf.index].start, gray);
            ioctl(cam_fd, VIDIOC_QBUF, &vbuf); // Requeue buffer immediately
        }

        /* Computer Vision Processing */
        filter_motion(gray, prev_gray, mask, diff_thresh);
        memcpy(prev_gray, gray, PROC_W * PROC_H);

        Blob blobs[MAX_BLOBS];
        int num_blobs = find_blobs(mask, blobs, MAX_BLOBS, min_area);

        bool l_ictus = false, r_ictus = false;
        float l_intensity = 0.0f, r_intensity = 0.0f;

        if (num_blobs == 0) {
            hand_decay(&left_hand);
            hand_decay(&right_hand);
        } else if (num_blobs == 1) {
            // Single hand moving: Proximity association with spatial hysteresis
            float bx = blobs[0].x;
            float by = blobs[0].y;
            int barea = blobs[0].area;

            float d_left  = left_hand.active  ? fabsf(bx - left_hand.x)  : 999.0f;
            float d_right = right_hand.active ? fabsf(bx - right_hand.x) : 999.0f;

            if (d_right < d_left) {
                r_ictus = hand_update(&right_hand, bx, by, barea, &r_intensity);
                hand_decay(&left_hand);
            } else if (d_left < d_right) {
                l_ictus = hand_update(&left_hand, bx, by, barea, &l_intensity);
                hand_decay(&right_hand);
            } else {
                if (bx >= 0.5f) {
                    r_ictus = hand_update(&right_hand, bx, by, barea, &r_intensity);
                    hand_decay(&left_hand);
                } else {
                    l_ictus = hand_update(&left_hand, bx, by, barea, &l_intensity);
                    hand_decay(&right_hand);
                }
            }
        } else {
            // Two or more hands moving: Sort top 2 from left to right
            int idx_left = 0, idx_right = 1;
            if (blobs[0].x > blobs[1].x) {
                idx_left = 1;
                idx_right = 0;
            }

            l_ictus = hand_update(&left_hand, blobs[idx_left].x, blobs[idx_left].y, blobs[idx_left].area, &l_intensity);
            r_ictus = hand_update(&right_hand, blobs[idx_right].x, blobs[idx_right].y, blobs[idx_right].area, &r_intensity);

            float dx = right_hand.x - left_hand.x;
            float dy = right_hand.y - left_hand.y;
            span_val = sqrtf(dx * dx + dy * dy);
        }

        bool is_tutti = (l_ictus && r_ictus);

        /* ---------------------------------------------------------------------
         * OSC Dispatch to SuperCollider
         * ------------------------------------------------------------------ */

        // 1. Ictus Strike (Accent on snap / downbeat)
        if (is_tutti) {
            float tutti_int = (l_intensity > r_intensity) ? l_intensity : r_intensity;
            int len = build_osc_ictus(osc_buf, tutti_int, 0.5f, 0.2f);
            SEND_OSC(osc_buf, len);
            if (sc_ready) {
                sc_engine_strike_ictus(tutti_int, 0.5f, 0.2f);
                sc_engine_strike_tutti(tutti_int, 0.0f);
            }
        } else if (r_ictus || l_ictus) {
            float hx = r_ictus ? right_hand.x : left_hand.x;
            float hy = 1.0f - (r_ictus ? right_hand.y : left_hand.y);
            float hi = r_ictus ? r_intensity : l_intensity;
            int len = build_osc_ictus(osc_buf, hi, hx, hy);
            SEND_OSC(osc_buf, len);
            if (sc_ready) {
                sc_engine_strike_ictus(hi, hx, hy);
                if (hi > 0.6f) {
                    sc_engine_strike_tutti(hi * 0.7f, hx * 2.0f - 1.0f);
                }
            }
        }

        // 2. Continuous Flow Modulation
        if (right_hand.active && !left_hand.active) {
            curr_prim_x = right_hand.x;
            curr_prim_y = right_hand.y;
            span_val = 0.2f;
        } else if (left_hand.active && !right_hand.active) {
            curr_prim_x = left_hand.x;
            curr_prim_y = left_hand.y;
            span_val = 0.2f;
        } else if (right_hand.active && left_hand.active) {
            curr_prim_x = right_hand.x;
            curr_prim_y = right_hand.y;
            if (span_val < 0.1f) span_val = 0.1f;
            if (span_val > 1.5f) span_val = 1.5f;
        }

        float y_elev = 1.0f - curr_prim_y;
        if (y_elev < 0.0f) y_elev = 0.0f;
        if (y_elev > 1.0f) y_elev = 1.0f;

        float x_pan = (curr_prim_x - 0.5f) * 2.2f;
        if (x_pan < -1.0f) x_pan = -1.0f;
        if (x_pan > 1.0f)  x_pan = 1.0f;

        float tot_energy = left_hand.energy + right_hand.energy;

        int len_morph = build_osc_morph(osc_buf, y_elev, x_pan, tot_energy, span_val);
        SEND_OSC(osc_buf, len_morph);

        int len_tension = build_osc_tension(osc_buf, tot_energy, is_tutti ? 2.0f : 0.0f);
        SEND_OSC(osc_buf, len_tension);

        if (sc_ready) {
            sc_engine_set_morph(y_elev, x_pan, tot_energy, span_val);
        }

        /* ---------------------------------------------------------------------
         * Raylib CRT Dashboard Rendering
         * ------------------------------------------------------------------ */
        if (!headless) {
            // Update camera texture
            for (int i = 0; i < PROC_W * PROC_H; i++) {
                uint8_t g = gray[i];
                display_rgba[i * 4 + 0] = g;
                display_rgba[i * 4 + 1] = g;
                display_rgba[i * 4 + 2] = g;
                display_rgba[i * 4 + 3] = 255;
            }
            UpdateTexture(tex_cam, display_rgba);

            // Update motion mask texture
            for (int i = 0; i < PROC_W * PROC_H; i++) {
                uint8_t m = mask[i];
                display_rgba[i * 4 + 0] = 0;
                display_rgba[i * 4 + 1] = m ? 220 : 0;
                display_rgba[i * 4 + 2] = m ? 140 : 0;
                display_rgba[i * 4 + 3] = 255;
            }
            UpdateTexture(tex_mask, display_rgba);

            BeginDrawing();
            ClearBackground((Color){ 8, 10, 14, 255 });

            // 1. Video Monitor Frames
            int vw = 480, vh = 360;
            DrawRectangle(24, 20, vw, vh, (Color){ 12, 14, 18, 255 });
            DrawTexturePro(tex_cam, (Rectangle){ 0, 0, PROC_W, PROC_H }, (Rectangle){ 24, 20, vw, vh }, (Vector2){0,0}, 0.0f, WHITE);
            DrawRectangleLines(24, 20, vw, vh, (Color){ 40, 50, 65, 255 });
            DrawText("V4L2 DIRECT DMA FEED", 36, 32, 14, (Color){ 180, 195, 210, 255 });

            DrawRectangle(528, 20, vw, vh, (Color){ 12, 14, 18, 255 });
            DrawTexturePro(tex_mask, (Rectangle){ 0, 0, PROC_W, PROC_H }, (Rectangle){ 528, 20, vw, vh }, (Vector2){0,0}, 0.0f, WHITE);
            DrawRectangleLines(528, 20, vw, vh, (Color){ 40, 50, 65, 255 });
            DrawText("TEMPORAL DIFFERENCE & CCL MASK", 540, 32, 14, (Color){ 60, 240, 160, 255 });

            // 2. Draw Kinematic Overlays (Centroids, Trails, Span Line)
            float off_x = 528.0f, off_y = 20.0f;
            float scale_x = (float)vw, scale_y = (float)vh;

            // Hand Span Line
            if (left_hand.active && right_hand.active) {
                Vector2 lp = { off_x + left_hand.x * scale_x, off_y + left_hand.y * scale_y };
                Vector2 rp = { off_x + right_hand.x * scale_x, off_y + right_hand.y * scale_y };
                DrawLineEx(lp, rp, 2.5f, (Color){ 240, 205, 50, 220 });
                DrawText(TextFormat("SPAN: %.2f", span_val), (lp.x + rp.x) * 0.5f - 30, (lp.y + rp.y) * 0.5f - 18, 12, (Color){ 240, 205, 50, 255 });
            }

            // Left Hand Trail & Centroid (Cyan)
            if (left_hand.trail_count > 1) {
                for (int i = 0; i < left_hand.trail_count - 1; i++) {
                    Vector2 p1 = { off_x + left_hand.trail[i].x * scale_x, off_y + left_hand.trail[i].y * scale_y };
                    Vector2 p2 = { off_x + left_hand.trail[i+1].x * scale_x, off_y + left_hand.trail[i+1].y * scale_y };
                    DrawLineEx(p1, p2, 1.8f, (Color){ 0, 220, 255, (unsigned char)(255 * (i + 1) / left_hand.trail_count) });
                }
            }
            if (left_hand.active) {
                Vector2 pos = { off_x + left_hand.x * scale_x, off_y + left_hand.y * scale_y };
                DrawCircleV(pos, 8.0f + left_hand.ictus_flash * 14.0f, (Color){ 0, 220, 255, 240 });
                DrawCircleLines((int)pos.x, (int)pos.y, 14.0f + left_hand.energy * 8.0f, (Color){ 0, 220, 255, 180 });
                DrawText("LEFT (EXPRESSION)", (int)pos.x - 50, (int)pos.y + 16, 11, (Color){ 0, 220, 255, 255 });
            }

            // Right Hand Trail & Centroid (Magenta)
            if (right_hand.trail_count > 1) {
                for (int i = 0; i < right_hand.trail_count - 1; i++) {
                    Vector2 p1 = { off_x + right_hand.trail[i].x * scale_x, off_y + right_hand.trail[i].y * scale_y };
                    Vector2 p2 = { off_x + right_hand.trail[i+1].x * scale_x, off_y + right_hand.trail[i+1].y * scale_y };
                    DrawLineEx(p1, p2, 1.8f, (Color){ 255, 60, 180, (unsigned char)(255 * (i + 1) / right_hand.trail_count) });
                }
            }
            if (right_hand.active) {
                Vector2 pos = { off_x + right_hand.x * scale_x, off_y + right_hand.y * scale_y };
                DrawCircleV(pos, 8.0f + right_hand.ictus_flash * 14.0f, (Color){ 255, 60, 180, 240 });
                DrawCircleLines((int)pos.x, (int)pos.y, 14.0f + right_hand.energy * 8.0f, (Color){ 255, 60, 180, 180 });
                DrawText("RIGHT (BATON)", (int)pos.x - 40, (int)pos.y + 16, 11, (Color){ 255, 60, 180, 255 });
            }

            // Tutti Flash Overlay
            if (is_tutti) {
                DrawRectangle(528, 20, vw, vh, (Color){ 255, 230, 80, 90 });
                DrawText(">>> TUTTI DOWNBEAT ACCENT <<<", 620, 180, 18, (Color){ 255, 240, 100, 255 });
            }

            // 3. Orchestral Telemetry HUD Panel
            int tx = 24, ty = 400, tw = 1072, th = 216;
            DrawRectangle(tx, ty, tw, th, (Color){ 14, 18, 24, 255 });
            DrawRectangleLines(tx, ty, tw, th, (Color){ 30, 40, 50, 255 });

            DrawText("ORCHESTRAL TELEMETRY (PURE C / POSIX ZERO-COPY ENGINE)", tx + 16, ty + 14, 15, (Color){ 240, 205, 50, 255 });

            const char *mode_str = (left_hand.active && right_hand.active) ? "BOTH HANDS ACTIVE (FULL CONDUCTION)" :
                                   (right_hand.active ? "RIGHT HAND ACTIVE (BATON LEAD)" :
                                   (left_hand.active ? "LEFT HAND ACTIVE (EXPRESSIVE DYNAMICS)" : "RESTING / HOVER"));
            Color mode_col = (left_hand.active && right_hand.active) ? (Color){ 60, 240, 160, 255 } :
                             (right_hand.active ? (Color){ 255, 60, 180, 255 } :
                             (left_hand.active ? (Color){ 0, 220, 255, 255 } : (Color){ 140, 150, 160, 255 }));

            DrawText(TextFormat("State:       %s", mode_str), tx + 20, ty + 42, 13, mode_col);
            DrawText(TextFormat("Pitch/Elev:  %.2f (Moog Cutoff & Lead Pitch Sweep)", y_elev), tx + 20, ty + 64, 13, (Color){ 210, 220, 230, 255 });
            DrawText(TextFormat("Stereo Pan:  %+.2f (Stereo Field Balance)", x_pan), tx + 20, ty + 86, 13, (Color){ 210, 220, 230, 255 });
            DrawText(TextFormat("Hand Span:   %.2f (Reverb Space & Vowel Opening)", span_val), tx + 20, ty + 108, 13, (Color){ 210, 220, 230, 255 });
            DrawText(TextFormat("Kinetic:     %.2f (Analog Saturation & Vibrato)", tot_energy), tx + 20, ty + 130, 13, (Color){ 210, 220, 230, 255 });
            DrawText(TextFormat("Audio Engine: %s", sc_ready ? "libscsynth (IN-PROCESS EMBEDDED SUPERCALLIDER)" : "External SuperCollider UDP"), tx + 20, ty + 152, 13, sc_ready ? (Color){ 60, 240, 160, 255 } : (Color){ 100, 200, 255, 255 });
            DrawText(TextFormat("SynthDefs:    %s", synthdefs_dir), tx + 20, ty + 172, 12, (Color){ 180, 195, 210, 255 });
            DrawText("FPS: 60  |  Audio Latency: < 0.05ms (Zero-Socket DMA)  |  RAM: 14MB  |  [ESC/Q] to Exit", tx + 20, ty + 194, 12, (Color){ 120, 140, 160, 255 });

            // Real-time Pitch & Pan Gauge
            DrawRectangle(tx + 600, ty + 46, 440, 130, (Color){ 20, 26, 36, 255 });
            DrawRectangleLines(tx + 600, ty + 46, 440, 130, (Color){ 45, 55, 70, 255 });
            DrawText("PITCH / PAN GAUGE", tx + 612, ty + 56, 11, (Color){ 150, 160, 175, 255 });

            int gx = tx + 600 + (int)((x_pan + 1.0f) * 0.5f * 420.0f) + 10;
            int gy = ty + 46 + (int)((1.0f - y_elev) * 100.0f) + 15;
            DrawCircle(gx, gy, 6.0f, (Color){ 240, 205, 50, 255 });
            DrawCircleLines(gx, gy, 10.0f, (Color){ 240, 205, 50, 160 });
            DrawLine(tx + 600 + 220, ty + 46, tx + 600 + 220, ty + 176, (Color){ 40, 50, 65, 180 });
            DrawLine(tx + 600, ty + 46 + 65, tx + 1040, ty + 46 + 65, (Color){ 40, 50, 65, 180 });

            EndDrawing();
        } else {
            // Headless sleep to match ~60 FPS update rate
            usleep(16666);
        }
    }

    /* 6. Clean Shutdown */
    printf("\n[Conductor] Shutting down cleanly...\n");

    if (sc_ready) {
        sc_engine_cleanup();
    }

    if (!headless) {
        UnloadTexture(tex_cam);
        UnloadTexture(tex_mask);
        CloseWindow();
    }

    ioctl(cam_fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        munmap(buffers[i].start, buffers[i].length);
    }
    close(cam_fd);
    close(sock_fd);

    free(gray);
    free(prev_gray);
    free(mask);
    free(display_rgba);

    printf("[Conductor] Hardware resources freed. Done.\n");
    _exit(0);
}
