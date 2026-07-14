package com.tpv.bench

data class TpvDetection(
    val status: Int,
    val classId: Int,
    val x: Int, val y: Int,
    val thetaX10: Int,
    val confidenceQ8: Int,
)

data class TpvFeatures(
    val hu: IntArray,
    val perimRatio: Int,
    val eccentricity: Int,
    val m3AxisSign: Int,
) {
    override fun equals(other: Any?) = other is TpvFeatures &&
        hu.contentEquals(other.hu) &&
        perimRatio == other.perimRatio &&
        eccentricity == other.eccentricity &&
        m3AxisSign == other.m3AxisSign
    override fun hashCode() = hu.contentHashCode() * 31 * 31 * 31 +
        perimRatio * 31 * 31 + eccentricity * 31 + m3AxisSign
}

data class TpvDetectionDebug(
    val det: TpvDetection,
    val features: TpvFeatures,
    val distancesSq: IntArray,
) {
    override fun equals(other: Any?) = other is TpvDetectionDebug &&
        det == other.det && features == other.features &&
        distancesSq.contentEquals(other.distancesSq)
    override fun hashCode() =
        det.hashCode() * 31 * 31 + features.hashCode() * 31 +
        distancesSq.contentHashCode()
}

data class TpvBbox(val x: Int, val y: Int, val w: Int, val h: Int)

data class TpvDetectionDebugV2(
    val det: TpvDetection,
    val features: TpvFeatures,
    val distancesSq: IntArray,
    val bbox: TpvBbox,
    val areaPx: Int,
    val grid8x8: Int,
    val bin: ByteArray,
    val allBlobsMask: ByteArray,
    val mask: ByteArray,
) {
    override fun equals(other: Any?) = other is TpvDetectionDebugV2 &&
        det == other.det && features == other.features &&
        distancesSq.contentEquals(other.distancesSq) &&
        bbox == other.bbox && areaPx == other.areaPx && grid8x8 == other.grid8x8 &&
        bin.contentEquals(other.bin) &&
        allBlobsMask.contentEquals(other.allBlobsMask) &&
        mask.contentEquals(other.mask)
    override fun hashCode(): Int {
        var h = det.hashCode()
        h = h * 31 + features.hashCode()
        h = h * 31 + distancesSq.contentHashCode()
        h = h * 31 + bbox.hashCode()
        h = h * 31 + areaPx
        h = h * 31 + grid8x8
        h = h * 31 + bin.contentHashCode()
        h = h * 31 + allBlobsMask.contentHashCode()
        h = h * 31 + mask.contentHashCode()
        return h
    }
}

data class TpvVisionDetection(
    val engineId: Int,
    val detectionId: Long,
    val trackId: Long,
    val flags: Int,
    val classId: Int,
    val confidenceQ8: Int,
    val status: Int,
    val centerX: Int,
    val centerY: Int,
    val bbox: TpvBbox,
    val thetaX10: Int,
    val trackAgeFrames: Int,
    val trackHits: Int,
    val trackMisses: Int,
)

data class TpvVisionResult(
    val status: Int,
    val primaryEventEngine: Int,
    val detections: Array<TpvVisionDetection>,
) {
    override fun equals(other: Any?) = other is TpvVisionResult &&
        status == other.status &&
        primaryEventEngine == other.primaryEventEngine &&
        detections.contentEquals(other.detections)

    override fun hashCode(): Int =
        (status * 31 + primaryEventEngine) * 31 + detections.contentHashCode()
}

object TpvNative {
    init {
        System.loadLibrary("tpv")
        System.loadLibrary("tpv_jni")
    }

    /**
     * Run the debug-variant tpv pipeline.
     *
     * @param y              640×480 packed raw-Y buffer (rowStride == width).
     * @param width,height   currently must equal 640, 480.
     * @param outTimingNs    caller-allocated long[3]; JNI fills
     *                         [0] = t_jni_enter_ns  (clock_gettime(CLOCK_MONOTONIC), JNI entry)
     *                         [1] = t_tpv_enter_ns  (immediately before tpv_process_frame_debug)
     *                         [2] = t_tpv_exit_ns   (immediately after  tpv_process_frame_debug)
     *                       These come from CLOCK_MONOTONIC on the C side, which is the same
     *                       clock that Kotlin `System.nanoTime()` reads on Android, so the
     *                       timestamps are directly comparable to Kotlin-side nanoTime values.
     * @return               TpvDetectionDebug whose `det.status` is the tpv C return code
     *                       (0=OK, 1=EMPTY, 2=SCENE_ERROR, -1=BAD_INPUT — NOT a struct field).
     */
    external fun processFrameDebug(
        y: ByteArray, width: Int, height: Int,
        outTimingNs: LongArray,
    ): TpvDetectionDebug

    /**
     * Run the v2 debug variant with runtime-tunable threshold / dark mode / ROI.
     * Returns a TpvDetectionDebugV2 whose `bin`, `allBlobsMask`, and `mask` are
     * each 38400 bytes (640×480 / 8, LSB-first packed).
     *
     * @param binThreshold  0..255 cutoff; dark_object_mode determines polarity.
     * @param darkObjectMode true = Y < threshold is foreground; false = Y ≥ threshold.
     * @param roiX roiY roiW roiH ROI rect in 640×480 coords. Use (0,0,640,480) to disable.
     * @param outTimingNs    same semantics as v1: [jni_enter, tpv_enter, tpv_exit].
     */
    external fun processFrameDebugV2(
        y: ByteArray, width: Int, height: Int,
        binThreshold: Int,
        darkObjectMode: Boolean,
        roiX: Int, roiY: Int, roiW: Int, roiH: Int,
        outTimingNs: LongArray,
    ): TpvDetectionDebugV2

    external fun visionCreateV3(
        enabledEngines: Int,
        primaryEventEngine: Int,
        binThreshold: Int,
        darkObjectMode: Boolean,
        roiX: Int, roiY: Int, roiW: Int, roiH: Int,
        trackerMinHits: Int,
        trackerMaxAge: Int,
        trackerIouThreshold: Float,
        trackerCenterDistancePx: Float,
        faceMinScore: Float,
        objectMinScore: Float,
    ): Long

    external fun visionResetV3(handle: Long)

    external fun visionCloseV3(handle: Long)

    external fun processVisionFrameV3(
        handle: Long,
        y: ByteArray,
        width: Int,
        height: Int,
        outTimingNs: LongArray,
    ): TpvVisionResult

    external fun visionLastDebugV2(handle: Long): TpvDetectionDebugV2

    /** Reads `extern const uint8_t tpv_bin_threshold` from libtpv.so. */
    external fun binThreshold(): Int

    /** Reads the TPV_N_CLASSES macro compiled into libtpv.so. */
    external fun nClasses(): Int
}
