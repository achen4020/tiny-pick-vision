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

    /** Reads `extern const uint8_t tpv_bin_threshold` from libtpv.so. */
    external fun binThreshold(): Int

    /** Reads the TPV_N_CLASSES macro compiled into libtpv.so. */
    external fun nClasses(): Int
}
