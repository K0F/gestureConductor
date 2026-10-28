/* =============================================================================
 * sc_engine.h - Embedded SuperCollider libscsynth Engine for Gesture Conductor
 * Pure C Interface to Embedded scsynth World
 * =============================================================================
 */

#ifndef SC_ENGINE_H
#define SC_ENGINE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize embedded scsynth, connect to audio hardware, and load synthdefs from dir */
bool sc_engine_init(const char *synthdefs_dir, int udp_port);

/* Real-time continuous parameter morphing (zero latency in-process OSC) */
void sc_engine_set_morph(float y_elev, float x_pan, float energy, float span);

/* Trigger modal chime downbeat strike */
void sc_engine_strike_ictus(float intensity, float hit_x, float hit_y);

/* Trigger orchestral tutti sub-drop */
void sc_engine_strike_tutti(float intensity, float pan);

/* Clean shutdown */
void sc_engine_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SC_ENGINE_H */
