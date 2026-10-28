#include "conductor_tracker.h"
#include "conductor_audio.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define TAG "ConductorTracker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define PROC_W 120
#define PROC_H 160
#define NUM_PIXELS (PROC_W * PROC_H)

static uint8_t g_prev_frame[NUM_PIXELS];
static uint8_t g_curr_frame[NUM_PIXELS];
static uint8_t g_diff_frame[NUM_PIXELS];
static bool g_has_prev = false;

// Socket for OSC
static int g_sock = -1;
static struct sockaddr_in g_osc_addr;
static bool g_osc_active = false;

// 1-Euro Filter state
typedef struct {
    float x, y;
    float dx, dy;
    float last_time;
} OneEuroState;

static OneEuroState g_left_euro;
static OneEuroState g_right_euro;

// Tracking state
typedef struct {
    float cx, cy;
    float vx, vy;
    float ax, ay;
    float energy;
    float span;
    int active_frames;
    int lost_frames;
} HandTracker;

static HandTracker g_left_hand;
static HandTracker g_right_hand;

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static float one_euro_filter(float val, float *last_val, float *last_dval, double dt, float min_cutoff, float beta, float d_cutoff) {
    if (dt <= 0.0) return val;
    float dval = (val - *last_val) / (float)dt;
    float a_d = 1.0f / (1.0f + (float)(1.0 / (2.0 * M_PI * d_cutoff * dt)));
    float filtered_dval = a_d * dval + (1.0f - a_d) * (*last_dval);
    *last_dval = filtered_dval;

    float cutoff = min_cutoff + beta * fabsf(filtered_dval);
    float a = 1.0f / (1.0f + (float)(1.0 / (2.0 * M_PI * cutoff * dt)));
    float filtered_val = a * val + (1.0f - a) * (*last_val);
    *last_val = filtered_val;
    return filtered_val;
}

static int pack_osc_blob(char *buf, const char *addr, const char *types, ...) {
    int off = 0;
    int len = strlen(addr);
    memcpy(buf + off, addr, len);
    off += len;
    buf[off++] = '\0';
    while (off % 4 != 0) buf[off++] = '\0';

    int tlen = strlen(types);
    buf[off++] = ',';
    memcpy(buf + off, types, tlen);
    off += tlen;
    buf[off++] = '\0';
    while (off % 4 != 0) buf[off++] = '\0';

    va_list ap;
    va_start(ap, types);
    for (int i = 0; i < tlen; i++) {
        if (types[i] == 'f') {
            float val = (float)va_arg(ap, double);
            union { float f; uint32_t u; } pun;
            pun.f = val;
            uint32_t be = htonl(pun.u);
            memcpy(buf + off, &be, 4);
            off += 4;
        } else if (types[i] == 'i') {
            int32_t val = va_arg(ap, int32_t);
            int32_t be = htonl(val);
            memcpy(buf + off, &be, 4);
            off += 4;
        }
    }
    va_end(ap);
    return off;
}

static void send_osc(const char *addr, const char *types, ...) {
    if (g_sock < 0 || !g_osc_active) return;
    char buf[512];
    int off = 0;
    int len = strlen(addr);
    memcpy(buf + off, addr, len);
    off += len;
    buf[off++] = '\0';
    while (off % 4 != 0) buf[off++] = '\0';

    int tlen = strlen(types);
    buf[off++] = ',';
    memcpy(buf + off, types, tlen);
    off += tlen;
    buf[off++] = '\0';
    while (off % 4 != 0) buf[off++] = '\0';

    va_list ap;
    va_start(ap, types);
    for (int i = 0; i < tlen; i++) {
        if (types[i] == 'f') {
            float val = (float)va_arg(ap, double);
            union { float f; uint32_t u; } pun;
            pun.f = val;
            uint32_t be = htonl(pun.u);
            memcpy(buf + off, &be, 4);
            off += 4;
        } else if (types[i] == 'i') {
            int32_t val = va_arg(ap, int32_t);
            int32_t be = htonl(val);
            memcpy(buf + off, &be, 4);
            off += 4;
        }
    }
    va_end(ap);

    sendto(g_sock, buf, off, 0, (struct sockaddr *)&g_osc_addr, sizeof(g_osc_addr));
}

bool tracker_init(int in_w, int in_h, const char *osc_host, int osc_port) {
    (void)in_w;
    (void)in_h;
    memset(g_prev_frame, 0, sizeof(g_prev_frame));
    memset(g_curr_frame, 0, sizeof(g_curr_frame));
    memset(g_diff_frame, 0, sizeof(g_diff_frame));
    g_has_prev = false;

    memset(&g_left_hand, 0, sizeof(g_left_hand));
    memset(&g_right_hand, 0, sizeof(g_right_hand));
    memset(&g_left_euro, 0, sizeof(g_left_euro));
    memset(&g_right_euro, 0, sizeof(g_right_euro));

    g_left_hand.cx = 0.35f; g_left_hand.cy = 0.5f;
    g_right_hand.cx = 0.65f; g_right_hand.cy = 0.5f;

    tracker_set_osc_target(osc_host, osc_port);
    LOGI("Tracker initialized (160x120 internal CCL grid)");
    return true;
}

void tracker_set_osc_target(const char *host, int port) {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_osc_active = false;

    if (!host || strlen(host) == 0 || port <= 0) return;

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        LOGE("Failed to create OSC UDP socket");
        return;
    }

    memset(&g_osc_addr, 0, sizeof(g_osc_addr));
    g_osc_addr.sin_family = AF_INET;
    g_osc_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &g_osc_addr.sin_addr) <= 0) {
        LOGE("Invalid OSC host address: %s", host);
        close(g_sock);
        g_sock = -1;
        return;
    }

    g_osc_active = true;
    LOGI("OSC destination configured: %s:%d", host, port);
}

void tracker_process_y_plane(const uint8_t *y_data, int width, int height, int row_stride, int rotation, int is_front, ConductorTelemetry *out_telem) {
    static double last_frame_time = 0.0;
    double now = get_time_sec();
    double dt = (last_frame_time > 0.0) ? (now - last_frame_time) : 0.033;
    if (dt <= 0.001) dt = 0.033;
    last_frame_time = now;

    // Center-crop factor: screen is ~9:20 (0.45), portrait camera is 3:4 (0.75)
    // 0.45 / 0.75 = 0.60
    const float CROP_SPAN_X = 0.60f;

    // Resample raw camera into screen-aligned portrait grid (120x160)
    for (int y = 0; y < PROC_H; y++) {
        float v = (float)y / (float)(PROC_H - 1); // 0.0 (top) to 1.0 (bottom)
        for (int x = 0; x < PROC_W; x++) {
            float u_screen = (float)x / (float)(PROC_W - 1); // 0.0 (left) to 1.0 (right)
            float u_cam = 0.5f + (u_screen - 0.5f) * CROP_SPAN_X;
            if (u_cam < 0.0f) u_cam = 0.0f;
            if (u_cam > 1.0f) u_cam = 1.0f;

            int src_x = 0;
            int src_y = 0;

            if (rotation == 270 && is_front) {
                // Front selfie camera in portrait:
                // Screen top (v=0) -> sensor right (src_x = width - 1)
                // Screen right (u=1) -> sensor top (src_y = 0)
                src_x = (int)((1.0f - v) * (width - 1));
                src_y = (int)((1.0f - u_cam) * (height - 1));
            } else if (rotation == 90 && !is_front) {
                // Back camera in portrait:
                // Screen top (v=0) -> sensor left (src_x = 0)
                // Screen right (u=1) -> sensor bottom (src_y = height - 1)
                src_x = (int)(v * (width - 1));
                src_y = (int)(u_cam * (height - 1));
            } else {
                // Default fallback
                src_x = (int)(u_cam * (width - 1));
                src_y = (int)(v * (height - 1));
            }

            if (src_x < 0) src_x = 0;
            if (src_x >= width) src_x = width - 1;
            if (src_y < 0) src_y = 0;
            if (src_y >= height) src_y = height - 1;

            const uint8_t *row_ptr = y_data + src_y * row_stride;
            g_curr_frame[y * PROC_W + x] = row_ptr[src_x];
        }
    }

    if (!g_has_prev) {
        memcpy(g_prev_frame, g_curr_frame, NUM_PIXELS);
        g_has_prev = true;
        return;
    }

    // 2. Motion Differencing & Spatial Energy Centroids
    float sum_l_x = 0, sum_l_y = 0, mass_l = 0;
    float sum_r_x = 0, sum_r_y = 0, mass_r = 0;
    int min_lx = PROC_W, max_lx = 0, min_rx = PROC_W, max_rx = 0;

    const uint8_t NOISE_GATE = 14;

    for (int y = 4; y < PROC_H - 4; y++) {
        for (int x = 4; x < PROC_W - 4; x++) {
            int idx = y * PROC_W + x;
            int diff = abs((int)g_curr_frame[idx] - (int)g_prev_frame[idx]);
            if (diff > NOISE_GATE) {
                float w = (float)(diff - NOISE_GATE);

                // Attenuate center zone (head/torso) so lateral hand movements dominate
                float norm_x = (float)x / (float)PROC_W;
                if (norm_x > 0.40f && norm_x < 0.60f) {
                    w *= 0.25f;
                }

                // Spatial partition: Left side (x < PROC_W / 2) vs Right side (x >= PROC_W / 2)
                if (x < PROC_W / 2) {
                    sum_l_x += x * w;
                    sum_l_y += y * w;
                    mass_l += w;
                    if (x < min_lx) min_lx = x;
                    if (x > max_lx) max_lx = x;
                } else {
                    sum_r_x += x * w;
                    sum_r_y += y * w;
                    mass_r += w;
                    if (x < min_rx) min_rx = x;
                    if (x > max_rx) max_rx = x;
                }
            }
        }
    }
    memcpy(g_prev_frame, g_curr_frame, NUM_PIXELS);

    // 3. Left Hand Filtering (Threshold: 140.0)
    bool det_l = (mass_l > 140.0f);
    if (det_l) {
        float raw_lx = (sum_l_x / mass_l) / (float)PROC_W;
        float raw_ly = (sum_l_y / mass_l) / (float)PROC_H;
        g_left_hand.cx = one_euro_filter(raw_lx, &g_left_euro.x, &g_left_euro.dx, dt, 1.2f, 0.4f, 1.0f);
        g_left_hand.cy = one_euro_filter(raw_ly, &g_left_euro.y, &g_left_euro.dy, dt, 1.2f, 0.4f, 1.0f);
        g_left_hand.energy = mass_l / 8000.0f;
        g_left_hand.span = (float)(max_lx - min_lx) / (float)PROC_W;
        g_left_hand.active_frames++;
        g_left_hand.lost_frames = 0;
    } else {
        g_left_hand.lost_frames++;
        g_left_hand.energy *= 0.85f;
    }

    // 4. Right Hand Filtering & Kinematics (Threshold: 140.0)
    bool det_r = (mass_r > 140.0f);
    float last_vy = g_right_hand.vy;
    if (det_r) {
        float raw_rx = (sum_r_x / mass_r) / (float)PROC_W;
        float raw_ry = (sum_r_y / mass_r) / (float)PROC_H;
        float filt_rx = one_euro_filter(raw_rx, &g_right_euro.x, &g_right_euro.dx, dt, 1.2f, 0.4f, 1.0f);
        float filt_ry = one_euro_filter(raw_ry, &g_right_euro.y, &g_right_euro.dy, dt, 1.2f, 0.4f, 1.0f);

        g_right_hand.vx = (filt_rx - g_right_hand.cx) / (float)dt;
        g_right_hand.vy = (filt_ry - g_right_hand.cy) / (float)dt;
        g_right_hand.ay = (g_right_hand.vy - last_vy) / (float)dt;

        g_right_hand.cx = filt_rx;
        g_right_hand.cy = filt_ry;
        g_right_hand.energy = mass_r / 8000.0f;
        g_right_hand.span = (float)(max_rx - min_rx) / (float)PROC_W;
        g_right_hand.active_frames++;
        g_right_hand.lost_frames = 0;
    } else {
        g_right_hand.lost_frames++;
        g_right_hand.energy *= 0.85f;
        g_right_hand.vy *= 0.8f;
        g_right_hand.ay = 0.0f;
    }

    // 5. Downbeat (Ictus) Detection: Rapid downward motion inflecting upward
    int ictus_triggered = 0;
    float ictus_intensity = 0.0f;
    static double last_ictus_time = 0.0;
    if (det_r && g_right_hand.cy > 0.4f && (now - last_ictus_time) > 0.18) {
        if (last_vy > 1.2f && g_right_hand.vy < 0.2f && g_right_hand.ay < -15.0f) {
            ictus_triggered = 1;
            ictus_intensity = fminf(fabsf(g_right_hand.ay) / 35.0f, 1.0f);
            last_ictus_time = now;
            audio_strike_ictus(ictus_intensity, g_right_hand.cx, 1.0f - g_right_hand.cy);
            send_osc("/gesture/ictus", "fff", ictus_intensity, g_right_hand.cx, 1.0f - g_right_hand.cy);
        }
    }

    // 6. Tutti Strike: Explosive bilateral simultaneous motion
    int tutti_triggered = 0;
    float tutti_intensity = 0.0f;
    static double last_tutti_time = 0.0;
    if (det_l && det_r && (now - last_tutti_time) > 0.35) {
        float combined_energy = g_left_hand.energy + g_right_hand.energy;
        if (combined_energy > 1.8f) {
            tutti_triggered = 1;
            tutti_intensity = fminf(combined_energy / 3.0f, 1.0f);
            last_tutti_time = now;
            float tutti_pan = (g_right_hand.cx + g_left_hand.cx) - 1.0f;
            audio_strike_tutti(tutti_intensity, tutti_pan);
            send_osc("/gesture/tutti", "ff", tutti_intensity, tutti_pan);
        }
    }

    // 7. Modulate Audio Synth & Send Continuous OSC
    float norm_elev = 1.0f - g_right_hand.cy;
    float norm_pan = g_right_hand.cx * 2.0f - 1.0f;
    float total_energy = g_right_hand.energy + g_left_hand.energy;
    float hand_span = fabsf(g_right_hand.cx - g_left_hand.cx);

    audio_set_lead_morph(norm_elev, norm_pan, total_energy, hand_span);

    static int osc_throttle = 0;
    if (++osc_throttle % 2 == 0) { // 30 Hz OSC rate
        if (det_l) {
            send_osc("/gesture/hand/left", "ffff", 1.0f - g_left_hand.cy, g_left_hand.cx * 2.0f - 1.0f, g_left_hand.energy, g_left_hand.span);
        }
        if (det_r) {
            send_osc("/gesture/hand/right", "ffff", norm_elev, norm_pan, g_right_hand.energy, g_right_hand.span);
        }
    }

    // 8. Output telemetry for UI
    if (out_telem) {
        out_telem->left_x = g_left_hand.cx;
        out_telem->left_y = g_left_hand.cy;
        out_telem->left_energy = g_left_hand.energy;
        out_telem->left_span = g_left_hand.span;
        out_telem->left_detected = det_l ? 1 : 0;

        out_telem->right_x = g_right_hand.cx;
        out_telem->right_y = g_right_hand.cy;
        out_telem->right_energy = g_right_hand.energy;
        out_telem->right_span = g_right_hand.span;
        out_telem->right_detected = det_r ? 1 : 0;

        out_telem->ictus_triggered = ictus_triggered;
        out_telem->ictus_intensity = ictus_intensity;
        out_telem->ictus_x = g_right_hand.cx;
        out_telem->ictus_y = g_right_hand.cy;

        out_telem->tutti_triggered = tutti_triggered;
        out_telem->tutti_intensity = tutti_intensity;
        out_telem->tutti_pan = norm_pan;
        out_telem->fps = (float)(1.0 / dt);
    }
}

void tracker_close(void) {
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    g_osc_active = false;
    LOGI("Tracker resources closed.");
}
