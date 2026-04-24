package com.tpv.bench

import androidx.camera.core.ImageProxy

/**
 * Collects the three planes of a CameraX YUV_420_888 ImageProxy into a
 * contiguous NV21 byte array (Y + interleaved VU). Handles rowStride
 * padding and U/V pixelStride == 1 or 2.
 *
 * Output length = width * height * 3 / 2.
 */
object Yuv420ToNv21 {
    fun convert(proxy: ImageProxy): ByteArray {
        val w = proxy.width ; val h = proxy.height
        require(w % 2 == 0 && h % 2 == 0) {
            "Yuv420ToNv21 requires even dimensions; got ${w}x${h}"
        }
        val out = ByteArray(w * h * 3 / 2)

        // --- Y plane (top of NV21) ---
        val yPlane = proxy.planes[0]
        val yBuf = yPlane.buffer ; val yRowStride = yPlane.rowStride
        for (r in 0 until h) {
            yBuf.position(r * yRowStride)
            yBuf.get(out, r * w, w)
        }

        // --- Interleaved VU (bottom of NV21) ---
        val uPlane = proxy.planes[1]
        val vPlane = proxy.planes[2]
        val uBuf = uPlane.buffer ; val vBuf = vPlane.buffer
        val uRowStride = uPlane.rowStride ; val uPixStride = uPlane.pixelStride
        val vRowStride = vPlane.rowStride ; val vPixStride = vPlane.pixelStride
        val chromaH = h / 2 ; val chromaW = w / 2
        val vuBase = w * h
        for (r in 0 until chromaH) {
            val uRow = r * uRowStride ; val vRow = r * vRowStride
            val outRow = vuBase + r * w
            for (c in 0 until chromaW) {
                // NV21 order is V then U, per pair.
                out[outRow + 2 * c]     = vBuf.get(vRow + c * vPixStride)
                out[outRow + 2 * c + 1] = uBuf.get(uRow + c * uPixStride)
            }
        }
        return out
    }
}
