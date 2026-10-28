#include <jni.h>
#include "conductor_tracker.h"
#include "conductor_audio.h"
#include <android/log.h>
#include <stdlib.h>

#define TAG "ConductorJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

JNIEXPORT jboolean JNICALL
Java_org_kof_gestureconductor_MainActivity_nativeInit(
    JNIEnv *env,
    jobject thiz,
    jint w,
    jint h,
    jstring oscHost,
    jint oscPort,
    jboolean audioEnabled)
{
    (void)thiz;
    const char *host_str = NULL;
    if (oscHost) {
        host_str = (*env)->GetStringUTFChars(env, oscHost, NULL);
    }

    bool tracker_ok = tracker_init(w, h, host_str ? host_str : "127.0.0.1", oscPort);
    if (host_str) {
        (*env)->ReleaseStringUTFChars(env, oscHost, host_str);
    }

    bool audio_ok = audio_init();
    audio_set_enabled(audioEnabled);

    LOGI("nativeInit: tracker=%d, audio=%d", tracker_ok, audio_ok);
    return (tracker_ok && audio_ok);
}

JNIEXPORT void JNICALL
Java_org_kof_gestureconductor_MainActivity_nativeProcessFrame(
    JNIEnv *env,
    jobject thiz,
    jbyteArray yPlane,
    jint width,
    jint height,
    jint rowStride,
    jint rotation,
    jint isFront,
    jfloatArray outTelemetry)
{
    (void)thiz;
    if (!yPlane || !outTelemetry) return;

    jbyte *y_data = (*env)->GetPrimitiveArrayCritical(env, yPlane, NULL);
    if (!y_data) return;

    ConductorTelemetry telem;
    tracker_process_y_plane((const uint8_t *)y_data, width, height, rowStride, rotation, isFront, &telem);

    (*env)->ReleasePrimitiveArrayCritical(env, yPlane, y_data, JNI_ABORT);

    // Pack into outTelemetry float array:
    // [0]=left_x, [1]=left_y, [2]=left_energy, [3]=left_span, [4]=left_detected
    // [5]=right_x, [6]=right_y, [7]=right_energy, [8]=right_span, [9]=right_detected
    // [10]=ictus_trig, [11]=ictus_intensity, [12]=ictus_x, [13]=ictus_y
    // [14]=tutti_trig, [15]=tutti_intensity, [16]=tutti_pan, [17]=fps
    jfloat buf[18];
    buf[0] = telem.left_x;
    buf[1] = telem.left_y;
    buf[2] = telem.left_energy;
    buf[3] = telem.left_span;
    buf[4] = (jfloat)telem.left_detected;

    buf[5] = telem.right_x;
    buf[6] = telem.right_y;
    buf[7] = telem.right_energy;
    buf[8] = telem.right_span;
    buf[9] = (jfloat)telem.right_detected;

    buf[10] = (jfloat)telem.ictus_triggered;
    buf[11] = telem.ictus_intensity;
    buf[12] = telem.ictus_x;
    buf[13] = telem.ictus_y;

    buf[14] = (jfloat)telem.tutti_triggered;
    buf[15] = telem.tutti_intensity;
    buf[16] = telem.tutti_pan;
    buf[17] = telem.fps;

    (*env)->SetFloatArrayRegion(env, outTelemetry, 0, 18, buf);
}

JNIEXPORT void JNICALL
Java_org_kof_gestureconductor_MainActivity_nativeSetOscTarget(
    JNIEnv *env,
    jobject thiz,
    jstring oscHost,
    jint oscPort)
{
    (void)thiz;
    if (!oscHost) return;
    const char *host = (*env)->GetStringUTFChars(env, oscHost, NULL);
    tracker_set_osc_target(host, oscPort);
    (*env)->ReleaseStringUTFChars(env, oscHost, host);
}

JNIEXPORT void JNICALL
Java_org_kof_gestureconductor_MainActivity_nativeSetAudioEnabled(
    JNIEnv *env,
    jobject thiz,
    jboolean enabled)
{
    (void)thiz;
    audio_set_enabled(enabled);
}

JNIEXPORT void JNICALL
Java_org_kof_gestureconductor_MainActivity_nativeClose(
    JNIEnv *env,
    jobject thiz)
{
    (void)thiz;
    audio_close();
    tracker_close();
    LOGI("nativeClose: Audio and Tracker shut down.");
}
