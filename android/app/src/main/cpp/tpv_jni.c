#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include "tpv.h"
#include "tpv_internal.h"

#define LOG_TAG "tpv_jni"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Single-threaded camera callback path (spec §3), so a static buffer is
 * safe and avoids per-frame allocation. Sized exactly for the 640×480
 * packed-Y payload tpv requires; larger inputs are rejected in
 * processFrameDebug's bounds check. */
static uint8_t s_frame_buf[640 * 480];

static void throw_illegal_state(JNIEnv *env, const char *msg) {
    jclass cls = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (cls) (*env)->ThrowNew(env, cls, msg);
}

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
        (*env)->DeleteGlobalRef(env, s_cache.det_cls);
        (*env)->DeleteGlobalRef(env, s_cache.feat_cls);
        (*env)->DeleteGlobalRef(env, s_cache.dbg_cls);
        s_cache.det_cls = s_cache.feat_cls = s_cache.dbg_cls = NULL;
        return -1;
    }
    s_cache.initialized = 1;
    return 0;
}

typedef struct {
    jclass bbox_cls, dbg_v2_cls;
    jmethodID bbox_ctor, dbg_v2_ctor;
    int initialized;
} JniCacheV2;
static JniCacheV2 s_cache_v2;

static int init_cache_v2(JNIEnv *env) {
    if (s_cache_v2.initialized) return 0;
    /* v1 cache must already be initialized (TpvDetection/TpvFeatures exist). */
    if (init_cache(env) < 0) return -1;

    jclass bbox = (*env)->FindClass(env, "com/tpv/bench/TpvBbox");
    jclass dbg = (*env)->FindClass(env, "com/tpv/bench/TpvDetectionDebugV2");
    if (!bbox || !dbg) {
        LOGE("FindClass v2 failed");
        return -1;
    }
    s_cache_v2.bbox_cls   = (*env)->NewGlobalRef(env, bbox);
    s_cache_v2.dbg_v2_cls = (*env)->NewGlobalRef(env, dbg);

    /* TpvBbox(x, y, w, h): Int */
    s_cache_v2.bbox_ctor = (*env)->GetMethodID(env, s_cache_v2.bbox_cls, "<init>",
        "(IIII)V");
    /* TpvDetectionDebugV2(
     *     det: TpvDetection, features: TpvFeatures, distancesSq: IntArray,
     *     bbox: TpvBbox, areaPx: Int, grid8x8: Int,
     *     bin: ByteArray, allBlobsMask: ByteArray, mask: ByteArray) */
    s_cache_v2.dbg_v2_ctor = (*env)->GetMethodID(env, s_cache_v2.dbg_v2_cls, "<init>",
        "(Lcom/tpv/bench/TpvDetection;"
        "Lcom/tpv/bench/TpvFeatures;"
        "[I"
        "Lcom/tpv/bench/TpvBbox;"
        "II"
        "[B[B[B)V");

    if (!s_cache_v2.bbox_ctor || !s_cache_v2.dbg_v2_ctor) {
        LOGE("GetMethodID v2 failed");
        (*env)->DeleteGlobalRef(env, s_cache_v2.bbox_cls);
        (*env)->DeleteGlobalRef(env, s_cache_v2.dbg_v2_cls);
        s_cache_v2.bbox_cls = s_cache_v2.dbg_v2_cls = NULL;
        return -1;
    }
    s_cache_v2.initialized = 1;
    return 0;
}

/* v2 uses its own static output buffer so the 115 KB struct is not alloc'd
 * on the stack. Single-threaded camera callback makes this safe. */
static tpv_DetectionDebugV2 s_v2_out;

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

    if (init_cache(env) < 0) {
        throw_illegal_state(env, "tpv_jni: init_cache failed (FindClass/GetMethodID)");
        return NULL;
    }

    const jsize n = (jsize)(w * h);
    if (n <= 0 || n > (jsize)sizeof s_frame_buf) {
        LOGE("Y buffer size %d out of bounds", n);
        throw_illegal_state(env, "tpv_jni: Y buffer size out of bounds (expect 640x480)");
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

JNIEXPORT jobject JNICALL
Java_com_tpv_bench_TpvNative_processFrameDebugV2(
    JNIEnv *env, jobject thiz, jbyteArray y, jint w, jint h,
    jint bin_threshold, jboolean dark_object_mode,
    jint roi_x, jint roi_y, jint roi_w, jint roi_h,
    jlongArray out_timing_ns)
{
    (void)thiz;
    jlong t_jni_enter = monotonic_ns();

    if (init_cache_v2(env) < 0) {
        throw_illegal_state(env, "tpv_jni: v2 cache init failed");
        return NULL;
    }

    const jsize n = (jsize)(w * h);
    if (n <= 0 || n > (jsize)sizeof s_frame_buf) {
        throw_illegal_state(env, "tpv_jni: Y buffer size out of bounds");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, y, 0, n, (jbyte *)s_frame_buf);

    jlong t_tpv_enter = monotonic_ns();
    int rc = tpv_process_frame_debug_v2(
        s_frame_buf, w, h,
        (uint8_t)(bin_threshold & 0xFF),
        dark_object_mode ? 1 : 0,
        (int)roi_x, (int)roi_y, (int)roi_w, (int)roi_h,
        &s_v2_out);
    jlong t_tpv_exit = monotonic_ns();

    jlong times[3] = { t_jni_enter, t_tpv_enter, t_tpv_exit };
    (*env)->SetLongArrayRegion(env, out_timing_ns, 0, 3, times);

    (void)rc;  /* caller inspects det.status (== rc) */

    /* Build v1 det + features + distances objects (reuse v1 cache) */
    jobject det_obj = (*env)->NewObject(env, s_cache.det_cls, s_cache.det_ctor,
        (jint)rc,
        (jint)(uint32_t)s_v2_out.det.class_id,
        (jint)s_v2_out.det.x, (jint)s_v2_out.det.y,
        (jint)s_v2_out.det.theta_x10,
        (jint)(uint32_t)s_v2_out.det.confidence_q8);

    jintArray hu = (*env)->NewIntArray(env, 7);
    (*env)->SetIntArrayRegion(env, hu, 0, 7, (const jint *)s_v2_out.features.hu);
    jobject feat_obj = (*env)->NewObject(env, s_cache.feat_cls, s_cache.feat_ctor,
        hu,
        (jint)s_v2_out.features.perim_ratio,
        (jint)s_v2_out.features.eccentricity,
        (jint)s_v2_out.features.m3_axis_sign);

    jintArray dsq = (*env)->NewIntArray(env, TPV_N_CLASSES);
    (*env)->SetIntArrayRegion(env, dsq, 0, TPV_N_CLASSES,
        (const jint *)s_v2_out.distances_sq);

    /* v2-only objects.
     * bbox_{x1,y1} are INCLUSIVE endpoints (closed interval, matches
     * tpv_Blob convention — see include/tpv_internal.h above
     * tpv_DetectionDebugV2.bbox_x0 and src/ccl_moments.c ~line 149 Pass 3
     * `x <= bbox_x1` iteration), so convert to (w, h) with +1. */
    jobject bbox_obj = (*env)->NewObject(env, s_cache_v2.bbox_cls, s_cache_v2.bbox_ctor,
        (jint)s_v2_out.bbox_x0,
        (jint)s_v2_out.bbox_y0,
        (jint)(s_v2_out.bbox_x1 - s_v2_out.bbox_x0 + 1),
        (jint)(s_v2_out.bbox_y1 - s_v2_out.bbox_y0 + 1));

    const jsize MASK_LEN = TPV_WIDTH * TPV_HEIGHT / 8;
    jbyteArray bin_arr = (*env)->NewByteArray(env, MASK_LEN);
    (*env)->SetByteArrayRegion(env, bin_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.bin);
    jbyteArray all_arr = (*env)->NewByteArray(env, MASK_LEN);
    (*env)->SetByteArrayRegion(env, all_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.all_blobs_mask);
    jbyteArray mask_arr = (*env)->NewByteArray(env, MASK_LEN);
    (*env)->SetByteArrayRegion(env, mask_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.mask);

    return (*env)->NewObject(env, s_cache_v2.dbg_v2_cls, s_cache_v2.dbg_v2_ctor,
        det_obj, feat_obj, dsq,
        bbox_obj, (jint)s_v2_out.area_px, (jint)s_v2_out.grid_8x8,
        bin_arr, all_arr, mask_arr);
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
