package com.tpv.bench

import kotlin.math.roundToInt

/**
 * Converts a YUV_420_888 Y-plane byte buffer into the packed 640×480 byte
 * array consumed by tpv_process_frame_debug. Pure Kotlin / JVM so we can
 * unit-test without Android framework classes.
 */
class YuvAdapter(
    private val targetW: Int,
    private val targetH: Int,
) {
    init {
        require(targetW > 0 && targetH > 0) { "target must be positive" }
    }

    data class CropRect(val x: Int, val y: Int, val w: Int, val h: Int)
    data class Result(val y: ByteArray, val crop: CropRect) {
        override fun equals(other: Any?) = other is Result &&
            y.contentEquals(other.y) && crop == other.crop
        override fun hashCode() = y.contentHashCode() * 31 + crop.hashCode()
    }

    /**
     * @param src       Y-plane bytes as returned by the camera (may include
     *                  rowStride padding). Length must be ≥ rowStride * height.
     * @param rowStride Bytes per row in `src`; ≥ width.
     * @param width     Native frame width in pixels.
     * @param height    Native frame height in pixels.
     */
    fun extract(src: ByteArray, rowStride: Int, width: Int, height: Int): Result {
        require(width > 0 && height > 0) { "native size must be positive" }
        require(rowStride >= width) { "rowStride=$rowStride < width=$width" }
        require(width >= targetW && height >= targetH) {
            "native ${width}x${height} smaller than target ${targetW}x${targetH}; YuvAdapter downsamples only, does not upsample"
        }
        require(src.size >= rowStride * height) { "src buffer too small" }

        val crop = computeCrop(width, height)

        if (crop.w == targetW && crop.h == targetH) {
            // Fast path: native is already target-sized (after stripping
            // padding if any). Copy row-by-row.
            val out = ByteArray(targetW * targetH)
            for (r in 0 until targetH) {
                val srcRow = (crop.y + r) * rowStride + crop.x
                System.arraycopy(src, srcRow, out, r * targetW, targetW)
            }
            return Result(out, crop)
        }

        // Downsample path: box-average from (crop.w × crop.h) to (targetW × targetH).
        val out = ByteArray(targetW * targetH)
        val sxInt = crop.w.toDouble() / targetW       // may be non-integer
        val syInt = crop.h.toDouble() / targetH
        for (ty in 0 until targetH) {
            val y0 = (crop.y + ty * syInt).toInt()
            val y1 = (crop.y + (ty + 1) * syInt).toInt().coerceAtLeast(y0 + 1)
            for (tx in 0 until targetW) {
                val x0 = (crop.x + tx * sxInt).toInt()
                val x1 = (crop.x + (tx + 1) * sxInt).toInt().coerceAtLeast(x0 + 1)
                var sum = 0
                var count = 0
                for (sy in y0 until y1) {
                    val rowBase = sy * rowStride
                    for (sx in x0 until x1) {
                        sum += src[rowBase + sx].toInt() and 0xFF
                        count++
                    }
                }
                out[ty * targetW + tx] = (sum / count).toByte()
            }
        }
        return Result(out, crop)
    }

    /**
     * Pick the largest centered rectangle inside `width × height` whose
     * aspect ratio matches `targetW : targetH`. Result's (x, y) is top-left,
     * (w, h) is size, in native-frame pixels.
     */
    private fun computeCrop(width: Int, height: Int): CropRect {
        val targetRatio = targetW.toDouble() / targetH
        val nativeRatio = width.toDouble() / height
        return if (nativeRatio > targetRatio) {
            // Native is wider than target (e.g. 16:9 into 4:3) → crop columns
            val cw = (height * targetRatio).roundToInt()
            val cx = (width - cw) / 2
            CropRect(cx, 0, cw, height)
        } else if (nativeRatio < targetRatio) {
            // Native is taller (unusual on phones) → crop rows
            val ch = (width / targetRatio).roundToInt()
            val cy = (height - ch) / 2
            CropRect(0, cy, width, ch)
        } else {
            CropRect(0, 0, width, height)
        }
    }
}
