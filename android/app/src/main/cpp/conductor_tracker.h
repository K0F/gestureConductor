#ifndef CONDUCTOR_TRACKER_H
#define CONDUCTOR_TRACKER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float left_x, left_y, left_energy, left_span;
    float right_x, right_y, right_energy, right_span;
    int left_detected, right_detected;
    int ictus_triggered;
    float ictus_intensity, ictus_x, ictus_y;
    int tutti_triggered;
    float tutti_intensity, tutti_pan;
    float fps;
} ConductorTelemetry;

bool tracker_init(int in_w, int in_h, const char *osc_host, int osc_port);
void tracker_process_y_plane(const uint8_t *y_data, int width, int height, int row_stride, int rotation, int is_front, ConductorTelemetry *out_telem);
void tracker_set_osc_target(const char *host, int port);
void tracker_close(void);

#ifdef __cplusplus
}
#endif

#endif // CONDUCTOR_TRACKER_H
