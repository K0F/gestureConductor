#ifndef CONDUCTOR_AUDIO_H
#define CONDUCTOR_AUDIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_init(void);
void audio_set_enabled(bool enabled);
void audio_set_lead_morph(float y_elev, float x_pan, float energy, float span);
void audio_strike_ictus(float intensity, float hit_x, float hit_y);
void audio_strike_tutti(float intensity, float pan);
void audio_close(void);

#ifdef __cplusplus
}
#endif

#endif // CONDUCTOR_AUDIO_H
