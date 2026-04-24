#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include "tpv.h"
#include "tpv_internal.h"

#define LOG_TAG "tpv_jni"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Single-threaded camera callback path, so a static buffer is safe and
 * avoids per-frame allocation. Sized for 640×480 + some slack. */
static uint8_t s_frame_buf[640 * 480];

/* Cached class + method IDs, looked up once from the first invocation so we
 * don't pay reflection cost per frame. */
typedef struct {
    jclass det_cls, feat_cls, dbg_cls;
    jmethodID det_ctor, feat_ctor, dbg_ctor;
    int initialized;
} JniCache;
static JniCache s_cache;

static int init_cache(JNIEnv *env) {
    if (s_cache.initialized) return 0;

    jclass det = (*env)->FindClass(env, "com/tpv/bench/TpvDetection");
    jclass feat = (*env)->FindClass(env, "com/tpv/bench/TpvFeatures");
    jclass dbg = (*env)->FindClass(env, "com/tpv/bench/TpvDetectionDebug");
    if (!det || !feat || !dbg) {
        LOGE("FindClass failed");
        return -1;
    }
    s_cache.det_cls  = (*env)->NewGlobalRef(env, det);
    s_cache.feat_cls = (*env)->NewGlobalRef(env, feat);
    s_cache.dbg_cls  = (*env)->NewGlobalRef(env, dbg);

    s_cache.det_ctor = (*env)->GetMethodID(env, s_cache.det_cls, "<init>",
        "(IIIIII)V");
    s_cache.feat_ctor = (*env)->GetMethodID(env, s_cache.feat_cls, "<init>",
        "([IIII)V");
    s_cache.dbg_ctor = (*env)->GetMethodID(env, s_cache.dbg_cls, "<init>",
        "(Lcom/tpv/bench/TpvDetection;Lcom/tpv/bench/TpvFeatures;[I)V");
    if (!s_cache.det_ctor || !s_cache.feat_ctor || !s_cache.dbg_ctor) {
        LOGE("GetMethodID failed");
        return -1;
    }
    s_cache.initialized = 1;
    return 0;
}

static jlong monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000000000LL + (jlong)ts.tv_nsec;
}

JNIEXPORT jobject JNICALL
Java_com_tpv_bench_TpvNative_processFrameDebug(
        JNIEnv *env, jobject thiz, jbyteArray y, jint w, jint h,
        jlongArray out_timing_ns) {
    jlong t_jni_enter = monotonic_ns();

    if (init_cache(env) < 0) return NULL;

    const jsize n = (jsize)(w * h);
    if (n <= 0 || n > (jsize)sizeof s_frame_buf) {
        LOGE("Y buffer size %d out of bounds", n);
        return NULL;
    }

    (*env)->GetByteArrayRegion(env, y, 0, n, (jbyte *)s_frame_buf);

    /* Bracket the real tpv call as tightly as possible. These two
     * timestamps ARE the spec §A2 measurement — everything else (JNI array
     * copies, Java object construction) is outside the gate. */
    jlong t_tpv_enter = monotonic_ns();
    tpv_DetectionDebug out;
    int rc = tpv_process_frame_debug(s_frame_buf, w, h, &out);
    jlong t_tpv_exit = monotonic_ns();

    /* Publish the three timestamps. Kotlin's System.nanoTime() reads the
     * same CLOCK_MONOTONIC on Android, so the caller can subtract these
     * against any nanoTime() it takes. */
    jlong times[3] = { t_jni_enter, t_tpv_enter, t_tpv_exit };
    (*env)->SetLongArrayRegion(env, out_timing_ns, 0, 3, times);

    /* Even on non-OK rc we still construct a valid TpvDetectionDebug — the
     * caller checks det.status (which is rc, not a struct member, since
     * tpv_Detection has no status field). tpv_process_frame_debug zero-
     * fills features and distances_sq on non-OK, so those are already
     * deterministic. */
    jobject det_obj = (*env)->NewObject(env, s_cache.det_cls, s_cache.det_ctor,
        (jint)rc,                                   /* status = return code */
        (jint)(uint32_t)out.det.class_id,           /* 0..4, 0xFE, 0xFF */
        (jint)out.det.x, (jint)out.det.y,
        (jint)out.det.theta_x10,
        (jint)(uint32_t)out.det.confidence_q8);

    jintArray hu = (*env)->NewIntArray(env, 7);
    (*env)->SetIntArrayRegion(env, hu, 0, 7, (const jint *)out.features.hu);
    jobject feat_obj = (*env)->NewObject(env, s_cache.feat_cls, s_cache.feat_ctor,
        hu,
        (jint)out.features.perim_ratio,
        (jint)out.features.eccentricity,
        (jint)out.features.m3_axis_sign);

    jintArray dsq = (*env)->NewIntArray(env, TPV_N_CLASSES);
    (*env)->SetIntArrayRegion(env, dsq, 0, TPV_N_CLASSES,
        (const jint *)out.distances_sq);

    return (*env)->NewObject(env, s_cache.dbg_cls, s_cache.dbg_ctor,
        det_obj, feat_obj, dsq);
}

JNIEXPORT jint JNICALL
Java_com_tpv_bench_TpvNative_binThreshold(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)tpv_bin_threshold;
}

JNIEXPORT jint JNICALL
Java_com_tpv_bench_TpvNative_nClasses(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return (jint)TPV_N_CLASSES;
}
