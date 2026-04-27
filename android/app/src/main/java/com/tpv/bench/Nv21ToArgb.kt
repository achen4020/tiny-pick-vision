package com.tpv.bench

object Nv21ToArgb {
    fun convert(nv21: ByteArray, width: Int, height: Int, out: IntArray): IntArray {
        require(width > 0 && height > 0) { "width and height must be positive" }
        require(width % 2 == 0 && height % 2 == 0) {
            "NV21 requires even dimensions; got ${width}x$height"
        }
        require(nv21.size >= width * height * 3 / 2) {
            "nv21 size ${nv21.size} < expected ${width * height * 3 / 2}"
        }
        require(out.size >= width * height) {
            "out size ${out.size} < expected ${width * height}"
        }

        val frameSize = width * height
        var outIndex = 0
        for (y in 0 until height) {
            val uvRow = frameSize + (y shr 1) * width
            for (x in 0 until width) {
                val yValue = nv21[y * width + x].toInt() and 0xFF
                val uvIndex = uvRow + (x and -2)
                val vValue = (nv21[uvIndex].toInt() and 0xFF) - 128
                val uValue = (nv21[uvIndex + 1].toInt() and 0xFF) - 128

                val c = (yValue - 16).coerceAtLeast(0)
                val r = (298 * c + 409 * vValue + 128) shr 8
                val g = (298 * c - 100 * uValue - 208 * vValue + 128) shr 8
                val b = (298 * c + 516 * uValue + 128) shr 8
                out[outIndex++] =
                    (0xFF shl 24) or
                    (r.coerceIn(0, 255) shl 16) or
                    (g.coerceIn(0, 255) shl 8) or
                    b.coerceIn(0, 255)
            }
        }
        return out
    }
}
