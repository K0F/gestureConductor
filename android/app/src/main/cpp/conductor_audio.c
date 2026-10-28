#include "conductor_audio.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TAG "ConductorAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SAMPLE_RATE 48000.0f
#define TWO_PI 6.28318530717958647692f

static AAudioStream *g_stream = NULL;
static bool g_enabled = true;

/* DSP State */
typedef struct {
    // Lead tone
    float lead_phase;
    float lead_freq;
    float lead_target_freq;
    float lead_cutoff;
    float lead_target_cutoff;
    float lead_res;
    float lead_pan;
    float lead_energy;
    // 4-pole Moog ladder filter state
    float moog_s[4];

    // Ambient Wash
    float wash_phase[4];
    float wash_elev;
    float wash_energy;

    // Ictus bell strike
    bool ictus_active;
    float ictus_phase_c;
    float ictus_phase_m;
    float ictus_freq;
    float ictus_env;
    float ictus_decay;
    float ictus_amp;
    float ictus_pan;

    // Tutti thump strike
    bool tutti_active;
    float tutti_phase;
    float tutti_pitch_env;
    float tutti_env;
    float tutti_amp;
    float tutti_pan;

    // DC blocker
    float dc_in_l, dc_out_l;
    float dc_in_r, dc_out_r;
} SynthState;

static SynthState g_synth;

static aaudio_data_callback_result_t audio_callback(
    AAudioStream *stream,
    void *userData,
    void *audioData,
    int32_t numFrames)
{
    (void)stream;
    (void)userData;
    float *out = (float *)audioData;

    if (!g_enabled) {
        memset(out, 0, numFrames * 2 * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    SynthState *s = &g_synth;

    for (int i = 0; i < numFrames; i++) {
        // Smooth parameter interpolation
        s->lead_freq += 0.005f * (s->lead_target_freq - s->lead_freq);
        s->lead_cutoff += 0.005f * (s->lead_target_cutoff - s->lead_cutoff);

        // 1. Lead Generator: Band-limited PolyBLEP Sawtooth
        s->lead_phase += s->lead_freq / SAMPLE_RATE;
        if (s->lead_phase >= 1.0f) s->lead_phase -= 1.0f;

        float saw = 2.0f * s->lead_phase - 1.0f;
        // PolyBLEP anti-aliasing
        float dt = s->lead_freq / SAMPLE_RATE;
        if (s->lead_phase < dt) {
            float t = s->lead_phase / dt;
            saw += (2.0f * t - t * t - 1.0f);
        } else if (s->lead_phase > 1.0f - dt) {
            float t = (s->lead_phase - 1.0f) / dt;
            saw += (t * t + 2.0f * t + 1.0f);
        }

        // 4-pole Moog ladder low-pass filter
        float fc = s->lead_cutoff / SAMPLE_RATE;
        if (fc > 0.45f) fc = 0.45f;
        if (fc < 0.001f) fc = 0.001f;
        float f = fc * 1.16f;
        float fb = s->lead_res * (1.0f - 0.15f * f * f);
        float input = saw - fb * s->moog_s[3];
        input = tanhf(input * 0.7f); // soft saturation inside loop

        s->moog_s[0] += f * (input - s->moog_s[0]);
        s->moog_s[1] += f * (s->moog_s[0] - s->moog_s[1]);
        s->moog_s[2] += f * (s->moog_s[1] - s->moog_s[2]);
        s->moog_s[3] += f * (s->moog_s[2] - s->moog_s[3]);

        float lead_sig = s->moog_s[3] * (0.08f + s->lead_energy * 0.12f);
        float lead_pan_l = 0.5f * (1.0f - s->lead_pan);
        float lead_pan_r = 0.5f * (1.0f + s->lead_pan);

        // 2. Ambient Wash: Polyphonic detuned sines
        float wash_base = 110.0f * (1.0f + s->wash_elev * 2.0f);
        float wash_detunes[4] = { 0.992f, 1.008f, 1.498f, 2.003f };
        float wash_l = 0.0f;
        float wash_r = 0.0f;

        for (int w = 0; w < 4; w++) {
            s->wash_phase[w] += (wash_base * wash_detunes[w]) / SAMPLE_RATE;
            if (s->wash_phase[w] >= 1.0f) s->wash_phase[w] -= 1.0f;
            float sw = sinf(TWO_PI * s->wash_phase[w]);
            if (w % 2 == 0) wash_l += sw * 0.25f;
            else wash_r += sw * 0.25f;
        }
        float wash_gain = (0.04f + s->wash_energy * 0.08f);
        wash_l *= wash_gain;
        wash_r *= wash_gain;

        // 3. Ictus Bell Chime (FM strike)
        float ictus_sig = 0.0f;
        if (s->ictus_active) {
            float mod_ratio = 2.756f;
            float mod_freq = s->ictus_freq * mod_ratio;
            s->ictus_phase_m += mod_freq / SAMPLE_RATE;
            if (s->ictus_phase_m >= 1.0f) s->ictus_phase_m -= 1.0f;

            float mod = sinf(TWO_PI * s->ictus_phase_m) * (s->ictus_env * 3.5f);

            s->ictus_phase_c += (s->ictus_freq + mod * s->ictus_freq) / SAMPLE_RATE;
            if (s->ictus_phase_c >= 1.0f) s->ictus_phase_c -= 1.0f;

            ictus_sig = sinf(TWO_PI * s->ictus_phase_c) * s->ictus_env * s->ictus_amp;
            s->ictus_env *= s->ictus_decay;
            if (s->ictus_env < 0.0001f) {
                s->ictus_active = false;
            }
        }
        float ictus_pan_l = 0.5f * (1.0f - s->ictus_pan);
        float ictus_pan_r = 0.5f * (1.0f + s->ictus_pan);

        // 4. Tutti Thump (Sub-bass burst)
        float tutti_sig = 0.0f;
        if (s->tutti_active) {
            float tutti_f = 42.0f + s->tutti_pitch_env * 90.0f;
            s->tutti_phase += tutti_f / SAMPLE_RATE;
            if (s->tutti_phase >= 1.0f) s->tutti_phase -= 1.0f;

            tutti_sig = sinf(TWO_PI * s->tutti_phase) * s->tutti_env * s->tutti_amp;
            s->tutti_pitch_env *= 0.9985f;
            s->tutti_env *= 0.9992f;
            if (s->tutti_env < 0.0001f) {
                s->tutti_active = false;
            }
        }
        float tutti_pan_l = 0.5f * (1.0f - s->tutti_pan);
        float tutti_pan_r = 0.5f * (1.0f + s->tutti_pan);

        // Mix down
        float mix_l = lead_sig * lead_pan_l + wash_l + ictus_sig * ictus_pan_l + tutti_sig * tutti_pan_l;
        float mix_r = lead_sig * lead_pan_r + wash_r + ictus_sig * ictus_pan_r + tutti_sig * tutti_pan_r;

        // DC Blocker Filter
        s->dc_out_l = mix_l - s->dc_in_l + 0.995f * s->dc_out_l;
        s->dc_in_l = mix_l;
        s->dc_out_r = mix_r - s->dc_in_r + 0.995f * s->dc_out_r;
        s->dc_in_r = mix_r;

        // Soft clip
        out[i * 2 + 0] = tanhf(s->dc_out_l * 1.5f);
        out[i * 2 + 1] = tanhf(s->dc_out_r * 1.5f);
    }

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool audio_init(void) {
    memset(&g_synth, 0, sizeof(g_synth));
    g_synth.lead_freq = 220.0f;
    g_synth.lead_target_freq = 220.0f;
    g_synth.lead_cutoff = 1200.0f;
    g_synth.lead_target_cutoff = 1200.0f;
    g_synth.lead_res = 1.0f;
    g_synth.lead_pan = 0.0f;

    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        LOGE("AAudio_createStreamBuilder failed: %s", AAudio_convertResultToText(result));
        return false;
    }

    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, audio_callback, NULL);

    result = AAudioStreamBuilder_openStream(builder, &g_stream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        LOGE("AAudioStreamBuilder_openStream failed: %s", AAudio_convertResultToText(result));
        return false;
    }

    result = AAudioStream_requestStart(g_stream);
    if (result != AAUDIO_OK) {
        LOGE("AAudioStream_requestStart failed: %s", AAudio_convertResultToText(result));
        AAudioStream_close(g_stream);
        g_stream = NULL;
        return false;
    }

    LOGI("AAudio real-time synthesizer started (48000 Hz, stereo, low-latency)");
    return true;
}

void audio_set_enabled(bool enabled) {
    g_enabled = enabled;
}

void audio_set_lead_morph(float y_elev, float x_pan, float energy, float span) {
    float pitch_hz = 55.0f * powf(2.0f, y_elev * 3.5f);
    if (pitch_hz < 45.0f) pitch_hz = 45.0f;
    if (pitch_hz > 1400.0f) pitch_hz = 1400.0f;

    float cutoff_hz = 120.0f * powf(2.0f, y_elev * 6.0f);
    if (cutoff_hz < 80.0f) cutoff_hz = 80.0f;
    if (cutoff_hz > 16000.0f) cutoff_hz = 16000.0f;

    float res = 0.5f + span * 2.5f;
    if (res < 0.2f) res = 0.2f;
    if (res > 3.6f) res = 3.6f;

    g_synth.lead_target_freq = pitch_hz;
    g_synth.lead_target_cutoff = cutoff_hz;
    g_synth.lead_res = res;
    g_synth.lead_pan = x_pan;
    g_synth.lead_energy = energy;

    g_synth.wash_elev = y_elev;
    g_synth.wash_energy = energy;
}

void audio_strike_ictus(float intensity, float hit_x, float hit_y) {
    g_synth.ictus_freq = 130.0f * powf(2.0f, hit_y * 2.2f);
    g_synth.ictus_amp = 0.12f + intensity * 0.15f;
    if (g_synth.ictus_amp > 0.40f) g_synth.ictus_amp = 0.40f;
    g_synth.ictus_decay = 0.9992f - intensity * 0.0002f;
    g_synth.ictus_pan = hit_x * 2.0f - 1.0f;
    g_synth.ictus_env = 1.0f;
    g_synth.ictus_phase_c = 0.0f;
    g_synth.ictus_phase_m = 0.0f;
    g_synth.ictus_active = true;
}

void audio_strike_tutti(float intensity, float pan) {
    g_synth.tutti_amp = 0.18f + intensity * 0.18f;
    if (g_synth.tutti_amp > 0.50f) g_synth.tutti_amp = 0.50f;
    g_synth.tutti_pan = pan;
    g_synth.tutti_pitch_env = 1.0f;
    g_synth.tutti_env = 1.0f;
    g_synth.tutti_phase = 0.0f;
    g_synth.tutti_active = true;
}

void audio_close(void) {
    if (g_stream) {
        AAudioStream_requestStop(g_stream);
        AAudioStream_close(g_stream);
        g_stream = NULL;
        LOGI("AAudio stream closed.");
    }
}
