#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include "tpv.h"
#include "tpv_internal.h"
#include "tpv_vision.h"

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

    if (!s_cache_v2.bbox_ctor) {
        LOGE("GetMethodID v2 failed: TpvBbox.<init>(IIII)V not found");
    }
    if (!s_cache_v2.dbg_v2_ctor) {
        LOGE("GetMethodID v2 failed: TpvDetectionDebugV2.<init>(Lcom/tpv/bench/TpvDetection;Lcom/tpv/bench/TpvFeatures;[ILcom/tpv/bench/TpvBbox;II[B[B[B)V not found");
    }
    if (!s_cache_v2.bbox_ctor || !s_cache_v2.dbg_v2_ctor) {
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

typedef struct {
    void *mem;
    size_t bytes;
    tpv_vision_context *ctx;
} VisionHandle;

typedef struct {
    jclass bbox_cls, vision_det_cls, vision_result_cls;
    jmethodID bbox_ctor, vision_det_ctor, vision_result_ctor;
    int initialized;
} JniCacheV3;
static JniCacheV3 s_cache_v3;

static int init_cache_v3(JNIEnv *env) {
    if (s_cache_v3.initialized) return 0;

    jclass bbox = (*env)->FindClass(env, "com/tpv/bench/TpvBbox");
    jclass det = (*env)->FindClass(env, "com/tpv/bench/TpvVisionDetection");
    jclass result = (*env)->FindClass(env, "com/tpv/bench/TpvVisionResult");
    if (!bbox || !det || !result) {
        LOGE("FindClass v3 failed");
        return -1;
    }

    s_cache_v3.bbox_cls = (*env)->NewGlobalRef(env, bbox);
    s_cache_v3.vision_det_cls = (*env)->NewGlobalRef(env, det);
    s_cache_v3.vision_result_cls = (*env)->NewGlobalRef(env, result);

    s_cache_v3.bbox_ctor = (*env)->GetMethodID(env, s_cache_v3.bbox_cls, "<init>",
        "(IIII)V");
    s_cache_v3.vision_det_ctor = (*env)->GetMethodID(env, s_cache_v3.vision_det_cls,
        "<init>", "(IJJIIIIIILcom/tpv/bench/TpvBbox;IIII)V");
    s_cache_v3.vision_result_ctor = (*env)->GetMethodID(env,
        s_cache_v3.vision_result_cls, "<init>",
        "(II[Lcom/tpv/bench/TpvVisionDetection;)V");

    if (!s_cache_v3.bbox_ctor || !s_cache_v3.vision_det_ctor ||
        !s_cache_v3.vision_result_ctor) {
        LOGE("GetMethodID v3 failed");
        if (s_cache_v3.bbox_cls) (*env)->DeleteGlobalRef(env, s_cache_v3.bbox_cls);
        if (s_cache_v3.vision_det_cls) (*env)->DeleteGlobalRef(env, s_cache_v3.vision_det_cls);
        if (s_cache_v3.vision_result_cls) (*env)->DeleteGlobalRef(env, s_cache_v3.vision_result_cls);
        memset(&s_cache_v3, 0, sizeof s_cache_v3);
        return -1;
    }

    s_cache_v3.initialized = 1;
    return 0;
}

static VisionHandle *vision_handle_from_jlong(jlong handle) {
    return (VisionHandle *)(intptr_t)handle;
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

    if (bin_threshold < 0 || bin_threshold > 255) {
        throw_illegal_state(env, "tpv_jni: bin_threshold out of [0,255]");
        return NULL;
    }
    if (roi_x < 0 || roi_y < 0 || roi_w <= 0 || roi_h <= 0 ||
        roi_w > w - roi_x || roi_h > h - roi_y) {
        throw_illegal_state(env, "tpv_jni: ROI out of frame bounds");
        return NULL;
    }

    (*env)->GetByteArrayRegion(env, y, 0, n, (jbyte *)s_frame_buf);

    jlong t_tpv_enter = monotonic_ns();
    /* s_v2_out is a module-static, so the &s_v2_out here is never NULL.
     * tpv_process_frame_debug_v2's early `if (!out) return TPV_BAD_INPUT;`
     * branch is unreachable from this call site — all other non-OK paths
     * (BAD_INPUT via roi validation / SCENE_ERROR / EMPTY) do memset(out, 0)
     * before returning, so s_v2_out contains deterministic zeros on any
     * non-OK rc. Do not change this call to pass a nullable pointer
     * without also revisiting the stale-mask risk. */
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

    /* Build v1 det + features + distances objects (reuse v1 cache).
     * Every JNI allocation below can return NULL with a pending exception
     * (OOM / class-not-found). Per JNI spec, passing NULL to subsequent
     * SetXxxArrayRegion / NewObject calls is undefined behavior, so we
     * bail out early on each failure — the pending exception propagates
     * cleanly to Kotlin. */
    jobject det_obj = (*env)->NewObject(env, s_cache.det_cls, s_cache.det_ctor,
        (jint)rc,
        (jint)(uint32_t)s_v2_out.det.class_id,
        (jint)s_v2_out.det.x, (jint)s_v2_out.det.y,
        (jint)s_v2_out.det.theta_x10,
        (jint)(uint32_t)s_v2_out.det.confidence_q8);
    if (!det_obj) return NULL;

    jintArray hu = (*env)->NewIntArray(env, 7);
    if (!hu) return NULL;
    (*env)->SetIntArrayRegion(env, hu, 0, 7, (const jint *)s_v2_out.features.hu);
    jobject feat_obj = (*env)->NewObject(env, s_cache.feat_cls, s_cache.feat_ctor,
        hu,
        (jint)s_v2_out.features.perim_ratio,
        (jint)s_v2_out.features.eccentricity,
        (jint)s_v2_out.features.m3_axis_sign);
    if (!feat_obj) return NULL;

    jintArray dsq = (*env)->NewIntArray(env, TPV_N_CLASSES);
    if (!dsq) return NULL;
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
    if (!bbox_obj) return NULL;

    /* TODO(v2.4 perf): 3 × 38400 B NewByteArray + SetByteArrayRegion per
     * frame = ~115 KB/frame Java heap allocation. At ~24 fps this is
     * 2.8 MB/s GC pressure. If Overlay draw introduces stutter, switch to
     * Kotlin-owned preallocated ByteArray passed INTO this JNI call and
     * filled via SetByteArrayRegion, eliminating the allocation. */
    const jsize MASK_LEN = TPV_WIDTH * TPV_HEIGHT / 8;
    jbyteArray bin_arr = (*env)->NewByteArray(env, MASK_LEN);
    if (!bin_arr) return NULL;
    (*env)->SetByteArrayRegion(env, bin_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.bin);
    jbyteArray all_arr = (*env)->NewByteArray(env, MASK_LEN);
    if (!all_arr) return NULL;
    (*env)->SetByteArrayRegion(env, all_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.all_blobs_mask);
    jbyteArray mask_arr = (*env)->NewByteArray(env, MASK_LEN);
    if (!mask_arr) return NULL;
    (*env)->SetByteArrayRegion(env, mask_arr, 0, MASK_LEN, (const jbyte *)s_v2_out.mask);

    return (*env)->NewObject(env, s_cache_v2.dbg_v2_cls, s_cache_v2.dbg_v2_ctor,
        det_obj, feat_obj, dsq,
        bbox_obj, (jint)s_v2_out.area_px, (jint)s_v2_out.grid_8x8,
        bin_arr, all_arr, mask_arr);
}

JNIEXPORT jlong JNICALL
Java_com_tpv_bench_TpvNative_visionCreateV3(
    JNIEnv *env, jobject thiz,
    jint enabled_engines,
    jint primary_event_engine,
    jint bin_threshold,
    jboolean dark_object_mode,
    jint roi_x, jint roi_y, jint roi_w, jint roi_h,
    jint tracker_min_hits,
    jint tracker_max_age,
    jfloat tracker_iou_threshold,
    jfloat tracker_center_distance_px,
    jfloat face_min_score,
    jfloat object_min_score)
{
    (void)thiz;

    tpv_vision_config cfg;
    tpv_vision_default_config(&cfg);
    cfg.enabled_engines = (uint32_t)enabled_engines;
    cfg.primary_event_engine = (uint32_t)primary_event_engine;
    cfg.bin_threshold = (uint8_t)(bin_threshold & 0xFF);
    cfg.dark_object_mode = dark_object_mode ? 1u : 0u;
    cfg.roi_x = (int16_t)roi_x;
    cfg.roi_y = (int16_t)roi_y;
    cfg.roi_w = (int16_t)roi_w;
    cfg.roi_h = (int16_t)roi_h;
    cfg.tracker_min_hits = tracker_min_hits;
    cfg.tracker_max_age = tracker_max_age;
    cfg.tracker_iou_threshold = tracker_iou_threshold;
    cfg.tracker_center_distance_px = tracker_center_distance_px;
    cfg.face_min_score = face_min_score;
    cfg.object_min_score = object_min_score;

    size_t bytes = 0;
    int rc = tpv_vision_context_size(&cfg, &bytes);
    if (rc != TPV_OK || bytes == 0) {
        throw_illegal_state(env, "tpv_jni: tpv_vision_context_size failed");
        return 0;
    }

    VisionHandle *handle = (VisionHandle *)calloc(1, sizeof *handle);
    if (!handle) {
        throw_illegal_state(env, "tpv_jni: VisionHandle allocation failed");
        return 0;
    }
    handle->mem = calloc(1, bytes);
    if (!handle->mem) {
        free(handle);
        throw_illegal_state(env, "tpv_jni: tpv_vision_context allocation failed");
        return 0;
    }
    handle->bytes = bytes;
    rc = tpv_vision_init(handle->mem, handle->bytes, &cfg, &handle->ctx);
    if (rc != TPV_OK || !handle->ctx) {
        free(handle->mem);
        free(handle);
        throw_illegal_state(env, "tpv_jni: tpv_vision_init failed");
        return 0;
    }
    return (jlong)(intptr_t)handle;
}

JNIEXPORT void JNICALL
Java_com_tpv_bench_TpvNative_visionResetV3(JNIEnv *env, jobject thiz, jlong handle_value) {
    (void)env; (void)thiz;
    VisionHandle *handle = vision_handle_from_jlong(handle_value);
    if (handle && handle->ctx) tpv_vision_reset(handle->ctx);
}

JNIEXPORT void JNICALL
Java_com_tpv_bench_TpvNative_visionCloseV3(JNIEnv *env, jobject thiz, jlong handle_value) {
    (void)env; (void)thiz;
    VisionHandle *handle = vision_handle_from_jlong(handle_value);
    if (!handle) return;
    free(handle->mem);
    memset(handle, 0, sizeof *handle);
    free(handle);
}

JNIEXPORT jobject JNICALL
Java_com_tpv_bench_TpvNative_processVisionFrameV3(
    JNIEnv *env, jobject thiz,
    jlong handle_value,
    jbyteArray y,
    jint w,
    jint h,
    jlongArray out_timing_ns)
{
    (void)thiz;
    jlong t_jni_enter = monotonic_ns();

    if (init_cache_v3(env) < 0) {
        throw_illegal_state(env, "tpv_jni: v3 cache init failed");
        return NULL;
    }

    VisionHandle *handle = vision_handle_from_jlong(handle_value);
    if (!handle || !handle->ctx) {
        throw_illegal_state(env, "tpv_jni: invalid vision handle");
        return NULL;
    }

    const jsize n = (jsize)(w * h);
    if (n <= 0 || n > (jsize)sizeof s_frame_buf) {
        throw_illegal_state(env, "tpv_jni: Y buffer size out of bounds");
        return NULL;
    }
    (*env)->GetByteArrayRegion(env, y, 0, n, (jbyte *)s_frame_buf);

    tpv_vision_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.data = s_frame_buf;
    frame.width = w;
    frame.height = h;
    frame.stride = w;
    frame.format = TPV_PIXEL_Y8_640X480;
    frame.rotation_degrees = 0;
    frame.timestamp_ns = t_jni_enter;

    tpv_vision_detection detections[16];
    tpv_vision_result result;
    memset(&result, 0, sizeof result);
    result.detections = detections;
    result.detection_capacity = 16;

    jlong t_tpv_enter = monotonic_ns();
    int rc = tpv_vision_process(handle->ctx, &frame, &result);
    jlong t_tpv_exit = monotonic_ns();

    jlong times[3] = { t_jni_enter, t_tpv_enter, t_tpv_exit };
    (*env)->SetLongArrayRegion(env, out_timing_ns, 0, 3, times);

    int count = result.detection_count;
    if (count < 0) count = 0;
    if (count > 16) count = 16;

    jobjectArray det_array = (*env)->NewObjectArray(env, count,
        s_cache_v3.vision_det_cls, NULL);
    if (!det_array) return NULL;

    for (int i = 0; i < count; i++) {
        const tpv_vision_detection *d = &detections[i];
        jobject bbox_obj = (*env)->NewObject(env, s_cache_v3.bbox_cls,
            s_cache_v3.bbox_ctor,
            (jint)d->bbox_x, (jint)d->bbox_y,
            (jint)d->bbox_w, (jint)d->bbox_h);
        if (!bbox_obj) return NULL;

        jobject det_obj = (*env)->NewObject(env, s_cache_v3.vision_det_cls,
            s_cache_v3.vision_det_ctor,
            (jint)d->engine_id,
            (jlong)d->detection_id,
            (jlong)d->track_id,
            (jint)d->flags,
            (jint)d->class_id,
            (jint)d->confidence_q8,
            (jint)d->status,
            (jint)d->center_x,
            (jint)d->center_y,
            bbox_obj,
            (jint)d->theta_x10,
            (jint)d->track_age_frames,
            (jint)d->track_hits,
            (jint)d->track_misses);
        if (!det_obj) return NULL;
        (*env)->SetObjectArrayElement(env, det_array, i, det_obj);
    }

    return (*env)->NewObject(env, s_cache_v3.vision_result_cls,
        s_cache_v3.vision_result_ctor,
        (jint)rc,
        (jint)result.primary_event_engine,
        det_array);
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
