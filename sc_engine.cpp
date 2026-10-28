/* =============================================================================
 * sc_engine.cpp - Embedded libscsynth Audio Engine
 * Links directly with libscsynth.so to embed SuperCollider in-process.
 * 
 * - Loads compiled .scsyndef files dynamically from ./synthdefs/ next to binary
 * - Direct in-memory zero-latency OSC packet dispatch (no network sockets)
 * - Zero hardcoded SynthDefs: 100% modular, editable via synthdefs.scd
 * =============================================================================
 */

#include "sc_engine.h"

#include <SuperCollider/server/SC_WorldOptions.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <arpa/inet.h>
#include <stdarg.h>

static struct World* g_world = nullptr;
static const int NODE_FLOW = 1001;
static const int NODE_WASH = 1002;
static bool g_nodes_active = false;

/* Reply callback for scsynth internal messages */
static void sc_reply_callback(struct ReplyAddress* addr, char* buf, int size) {
    (void)addr;
    (void)buf;
    (void)size;
}

/* Fast binary OSC packer for in-memory dispatch */
static int pack_osc(char *buf, const char *addr, const char *types, ...) {
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
        if (types[i] == 's') {
            const char *str = va_arg(ap, const char*);
            if (!str) str = "";
            int slen = strlen(str);
            memcpy(buf + off, str, slen);
            off += slen;
            buf[off++] = '\0';
            while (off % 4 != 0) buf[off++] = '\0';
        } else if (types[i] == 'i') {
            int32_t val = va_arg(ap, int32_t);
            int32_t be = htonl(val);
            memcpy(buf + off, &be, 4);
            off += 4;
        } else if (types[i] == 'f') {
            float val = (float)va_arg(ap, double);
            union { float f; uint32_t u; } pun;
            pun.f = val;
            uint32_t be = htonl(pun.u);
            memcpy(buf + off, &be, 4);
            off += 4;
        }
    }
    va_end(ap);
    return off;
}

extern "C" bool sc_engine_init(const char *synthdefs_dir, int udp_port) {
    printf("[sc_engine] Booting embedded libscsynth audio engine...\n");

    /* Ensure PipeWire / JACK environment auto-connects audio ports */
    if (system("pgrep -x carla >/dev/null 2>&1") == 0) {
        setenv("SC_JACK_DEFAULT_INPUTS", "system", 0);
        setenv("SC_JACK_DEFAULT_OUTPUTS", "Carla", 0);
    } else {
        setenv("SC_JACK_DEFAULT_INPUTS", "system", 0);
        setenv("SC_JACK_DEFAULT_OUTPUTS", "system", 0);
    }

    WorldOptions options;
    options.mRealTime = true;
    options.mNumAudioBusChannels = 128;
    options.mNumControlBusChannels = 4096;
    options.mNumInputBusChannels = 2;
    options.mNumOutputBusChannels = 2;
    options.mBufLength = 128;
    options.mRealTimeMemorySize = 65536; // 64 MB real-time synthesis pool
    options.mLoadGraphDefs = 1;
    options.mVerbosity = 0;

    g_world = World_New(&options);
    if (!g_world) {
        fprintf(stderr, "[sc_engine] ERROR: Failed to instantiate libscsynth World!\n");
        return false;
    }

    if (udp_port > 0) {
        World_OpenUDP(g_world, "127.0.0.1", udp_port);
        printf("[sc_engine] Embedded scsynth listening on UDP port %d for external tools\n", udp_port);
    }

    /* Load SynthDefs from directory next to binary (NO hardcoded definitions) */
    printf("[sc_engine] Loading external SynthDefs from: %s\n", synthdefs_dir);
    char buf[1024];
    int len = pack_osc(buf, "/d_loadDir", "s", synthdefs_dir);
    World_SendPacket(g_world, len, buf, sc_reply_callback);

    /* Allow scsynth audio thread 80ms to index .scsyndef files */
    usleep(80000);

    /* Connect JACK/PipeWire output ports to hardware audio sinks */
    system("pw-link SuperCollider:out_1 $(pw-link -i 2>/dev/null | grep -E 'playback_FL|playback_1' | head -n1) 2>/dev/null || pw-link SuperCollider:out_1 system:playback_1 2>/dev/null");
    system("pw-link SuperCollider:out_2 $(pw-link -i 2>/dev/null | grep -E 'playback_FR|playback_2' | head -n1) 2>/dev/null || pw-link SuperCollider:out_2 system:playback_2 2>/dev/null");

    /* Instantiate persistent synthesis nodes */
    // 1. Lead Conductor Flow
    len = pack_osc(buf, "/s_new", "siii", "conductorFlow", NODE_FLOW, 1, 0);
    World_SendPacket(g_world, len, buf, sc_reply_callback);

    // 2. Signature Ambient Wash
    len = pack_osc(buf, "/s_new", "siii", "conductorWash", NODE_WASH, 1, 0);
    World_SendPacket(g_world, len, buf, sc_reply_callback);

    g_nodes_active = true;
    printf("[sc_engine] SuperCollider live audio engine ready (Nodes %d & %d active)\n", NODE_FLOW, NODE_WASH);
    return true;
}

extern "C" void sc_engine_set_morph(float y_elev, float x_pan, float energy, float span) {
    if (!g_world || !g_nodes_active) return;

    // Pitch: 55 Hz (A1) to 880 Hz (A5)
    float pitch_hz = 55.0f * powf(2.0f, y_elev * 4.0f);
    if (pitch_hz < 45.0f) pitch_hz = 45.0f;
    if (pitch_hz > 1200.0f) pitch_hz = 1200.0f;

    // Moog Cutoff: 80 Hz to 16,000 Hz
    float cutoff_hz = 100.0f * powf(2.0f, y_elev * 6.5f);
    if (cutoff_hz < 80.0f) cutoff_hz = 80.0f;
    if (cutoff_hz > 16000.0f) cutoff_hz = 16000.0f;

    float pan_val = x_pan;
    if (pan_val < -1.0f) pan_val = -1.0f;
    if (pan_val > 1.0f)  pan_val = 1.0f;

    float res_val = 0.5f + span * 1.8f;
    if (res_val < 0.2f) res_val = 0.2f;
    if (res_val > 2.0f) res_val = 2.0f;

    float energy_val = energy;
    if (energy_val < 0.0f) energy_val = 0.0f;
    if (energy_val > 3.0f) energy_val = 3.0f;

    char buf[512];
    int len;

    // 1. Modulate Lead Tone (Node 1001)
    len = pack_osc(buf, "/n_set", "isfsfsfsfsf",
                   NODE_FLOW,
                   "pitch", pitch_hz,
                   "cutoff", cutoff_hz,
                   "pan", pan_val,
                   "res", res_val,
                   "energy", energy_val);
    World_SendPacket(g_world, len, buf, sc_reply_callback);

    // 2. Modulate Ambient Wash (Node 1002)
    len = pack_osc(buf, "/n_set", "isfsfsfsfsf",
                   NODE_WASH,
                   "pitchNorm", y_elev,
                   "cutoffNorm", y_elev,
                   "pan", pan_val,
                   "energy", energy_val,
                   "span", span);
    World_SendPacket(g_world, len, buf, sc_reply_callback);
}

extern "C" void sc_engine_strike_ictus(float intensity, float hit_x, float hit_y) {
    if (!g_world) return;

    float freq = 110.0f * powf(2.0f, hit_y * 2.5f);
    if (freq < 65.0f) freq = 65.0f;
    if (freq > 880.0f) freq = 880.0f;

    float decay = 0.3f + intensity * 0.4f;
    if (decay < 0.15f) decay = 0.15f;
    if (decay > 1.8f)  decay = 1.8f;

    float amp = 0.06f + intensity * 0.10f;
    if (amp < 0.03f) amp = 0.03f;
    if (amp > 0.20f) amp = 0.20f;

    float pan = hit_x * 2.0f - 1.0f;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f)  pan = 1.0f;

    float brightness = 0.8f + intensity;
    if (brightness < 0.4f) brightness = 0.4f;
    if (brightness > 2.2f) brightness = 2.2f;

    char buf[512];
    int len = pack_osc(buf, "/s_new", "siiisfsfsfsfsf",
                       "ictusStrike", -1, 1, 0,
                       "freq", freq,
                       "decay", decay,
                       "amp", amp,
                       "pan", pan,
                       "brightness", brightness);
    World_SendPacket(g_world, len, buf, sc_reply_callback);
}

extern "C" void sc_engine_strike_tutti(float intensity, float pan) {
    if (!g_world) return;

    float amp = 0.08f + intensity * 0.10f;
    if (amp < 0.05f) amp = 0.05f;
    if (amp > 0.22f) amp = 0.22f;

    float pan_val = pan;
    if (pan_val < -1.0f) pan_val = -1.0f;
    if (pan_val > 1.0f)  pan_val = 1.0f;

    char buf[256];
    int len = pack_osc(buf, "/s_new", "siiisfsf",
                       "tuttiThump", -1, 1, 0,
                       "amp", amp,
                       "pan", pan_val);
    World_SendPacket(g_world, len, buf, sc_reply_callback);
}

extern "C" void sc_engine_cleanup(void) {
    if (!g_world) return;

    printf("[sc_engine] Cleaning up embedded SuperCollider engine...\n");
    char buf[256];
    if (g_nodes_active) {
        int len1 = pack_osc(buf, "/n_free", "i", NODE_FLOW);
        World_SendPacket(g_world, len1, buf, sc_reply_callback);
        int len2 = pack_osc(buf, "/n_free", "i", NODE_WASH);
        World_SendPacket(g_world, len2, buf, sc_reply_callback);
        g_nodes_active = false;
    }

    char quit_msg[] = "/quit\0\0\0,\0\0\0";
    World_SendPacket(g_world, 12, quit_msg, sc_reply_callback);
    usleep(50000);

    World_Cleanup(g_world, false);
    g_world = nullptr;
    printf("[sc_engine] libscsynth stopped cleanly.\n");
}
