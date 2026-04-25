package com.tpv.bench

/**
 * Pure-logic helper for annotated-frame rendering. Any Android-free so it
 * can be unit-tested on the JVM. Callers (OverlayView, RunRecorder) apply
 * the outputs to a Canvas or bitmap.
 */
object OverlayPainter {

    data class NativeSize(val w: Int, val h: Int)

    /** 5-class palette (tab10 first 5, #3 swapped to brown to avoid RED_ERR collision). */
    val PALETTE = intArrayOf(
        0xFF1F77B4.toInt(), 0xFFFF7F0E.toInt(), 0xFF2CA02C.toInt(),
        0xFF8C564B.toInt(), 0xFF9467BD.toInt()
    )
    const val AMBER_WARN   = 0xFFF5A623.toInt()
    const val RED_ERR      = 0xFFD0021B.toInt()
    const val GREY_NEUTRAL = 0xFF9B9B9B.toInt()

    /** §5.5 colour rule. Covers all 7 legal det_cls values (0..4, 254, 255). */
    fun colorFor(detClsId: Int): Int = when {
        detClsId in 0..4 -> PALETTE[detClsId]
        detClsId == 0xFE -> AMBER_WARN
        detClsId == 0xFF -> RED_ERR
        else -> GREY_NEUTRAL   // defensive; state machine never emits these
    }

    /**
     * Map a (x_640, y_640) point from the 640×480 tpv coord system to native
     * camera-frame coordinates using the crop that the YuvAdapter recorded.
     */
    fun mapCoord(x640: Int, y640: Int, crop: YuvAdapter.CropRect): Pair<Int, Int> {
        val nx = crop.x + (x640.toDouble() * crop.w / 640.0).toInt()
        val ny = crop.y + (y640.toDouble() * crop.h / 480.0).toInt()
        return nx to ny
    }

    /** Line 1 text per §5.5 three-branch rule. */
    fun textLine1(d: TpvDetectionDebugV2): String {
        val cls = d.det.classId
        return when {
            cls in 0..4 -> "det_cls=$cls conf=${d.det.confidenceQ8} d²=${d.distancesSq[cls]}"
            cls == 0xFE || cls == 0xFF -> {
                val dmin = d.distancesSq.min()
                "det_cls=$cls conf=${d.det.confidenceQ8} d²min=$dmin"
            }
            else -> "det_cls=$cls"   // defensive
        }
    }

    /** Line 2 text per §5.5. */
    fun textLine2(eventClassId: Int, flicker: Boolean): String =
        "event_cls=$eventClassId flicker=$flicker"

    /** Circle radius (native px) = crop.w × 0.05 per §5.5. */
    fun circleRadius(crop: YuvAdapter.CropRect): Int = (crop.w * 0.05).toInt().coerceAtLeast(1)

    /** Axis length (native px) = crop.w × 0.08 per §5.5. */
    fun axisLength(crop: YuvAdapter.CropRect): Int = (crop.w * 0.08).toInt().coerceAtLeast(1)
}
