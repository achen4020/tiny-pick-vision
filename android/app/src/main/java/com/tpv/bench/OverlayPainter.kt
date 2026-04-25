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

    /** Mask fill color — pure green, alpha ≈ 120/255 ≈ 47% (spec §5.1). */
    const val GREEN_MASK_ARGB  = 0x7800FF00.toInt()
    /** ROI rectangle stroke color — same amber used for AMBIGUOUS warning. */
    const val YELLOW_ROI_ARGB  = 0xFFF5A623.toInt()
    /** Center dot color. */
    const val RED_CENTER_ARGB  = 0xFFD0021B.toInt()
    /** Commit-flash border color — pure green, opaque. (alpha set per-frame in OverlayView.) */
    const val GREEN_FLASH_ARGB = 0xFF00FF00.toInt()

    /** 640×480 / 8 = 38400 bytes per mask. */
    const val MASK_BYTES = 640 * 480 / 8

    /**
     * Decode an LSB-first packed mask bitmap (width*height/8 bytes) into an
     * ARGB IntArray where set bits → `argb`, clear bits → 0 (fully
     * transparent). Pure logic — kept on `OverlayPainter` so JVM unit tests
     * exercise it without Android. The caller wraps the IntArray into an
     * `android.graphics.Bitmap` (see `OverlayView.onDraw`).
     */
    fun decodeMaskToArgb(mask: ByteArray, w: Int, h: Int, argb: Int): IntArray {
        require(mask.size == w * h / 8) {
            "mask size ${mask.size} != expected ${w * h / 8}"
        }
        val out = IntArray(w * h)
        for (i in 0 until w * h) {
            val byte = mask[i shr 3].toInt() and 0xFF
            val bit = (byte ushr (i and 7)) and 1
            if (bit == 1) out[i] = argb
        }
        return out
    }

    /**
     * In-place variant of [decodeMaskToArgb] that writes into a caller-owned
     * IntArray to avoid per-frame allocation. Used by OverlayView.onDraw at
     * ~24 fps where 1.2 MB / call adds up to GC pressure.
     *
     * @param out  pre-allocated IntArray of length w*h. Contents are
     *             overwritten on every call (no need to pre-clear).
     */
    fun decodeMaskToArgb(mask: ByteArray, w: Int, h: Int, argb: Int, out: IntArray) {
        require(mask.size == w * h / 8) {
            "mask size ${mask.size} != expected ${w * h / 8}"
        }
        require(out.size == w * h) {
            "out size ${out.size} != expected ${w * h}"
        }
        for (i in 0 until w * h) {
            val byte = mask[i shr 3].toInt() and 0xFF
            val bit = (byte ushr (i and 7)) and 1
            out[i] = if (bit == 1) argb else 0
        }
    }

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
