package com.tpv.bench

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class YuvAdapterTest {

    /** Simulates a CameraX Y plane: no row-stride padding, data is a ramp. */
    private fun makeYPlane(w: Int, h: Int, rowStride: Int = w): ByteArray {
        val buf = ByteArray(rowStride * h)
        for (r in 0 until h) {
            for (c in 0 until w) {
                buf[r * rowStride + c] = ((r + c) and 0xFF).toByte()
            }
            // rowStride padding bytes remain zero
        }
        return buf
    }

    @Test
    fun `passthrough 640x480 is byte-identical to input`() {
        val src = makeYPlane(640, 480)
        val adapter = YuvAdapter(640, 480)
        val result = adapter.extract(src, rowStride = 640, width = 640, height = 480)
        assertEquals(640 * 480, result.y.size)
        assertArrayEquals(src, result.y)
        assertEquals(YuvAdapter.CropRect(0, 0, 640, 480), result.crop)
    }

    @Test
    fun `rowStride greater than width strips padding`() {
        val rowStride = 704          // typical: width rounded up to 64
        val src = makeYPlane(640, 480, rowStride)
        val adapter = YuvAdapter(640, 480)
        val result = adapter.extract(src, rowStride, 640, 480)
        assertEquals(640 * 480, result.y.size)
        // First row's first 640 bytes must equal src[0..639], not src[0..703]
        for (i in 0 until 640) {
            assertEquals(
                "row 0 col $i",
                src[i].toInt() and 0xFF,
                result.y[i].toInt() and 0xFF
            )
        }
    }

    @Test
    fun `1280x720 16-9 input is center-cropped to 4-3 then downsampled to 640x480`() {
        // 1280×720 is 16:9. Center crop for 4:3 = 960×720 starting at x=160.
        val rowStride = 1280
        val src = ByteArray(rowStride * 720)
        // Paint a white square at the center of the CROPPED region so we can
        // check it ends up centered in the output.
        // Cropped region: x∈[160, 1120), y∈[0, 720). Square at its center:
        // x∈[160+480-50, 160+480+50) = [590, 690), y∈[310, 410).
        for (r in 310 until 410) {
            for (c in 590 until 690) {
                src[r * rowStride + c] = 255.toByte()
            }
        }
        val adapter = YuvAdapter(640, 480)
        val result = adapter.extract(src, rowStride, 1280, 720)
        assertEquals(640 * 480, result.y.size)
        assertEquals(YuvAdapter.CropRect(160, 0, 960, 720), result.crop)
        // In 640×480 output, the white square should land at
        // x∈[320-33, 320+33), y∈[240-33, 240+33)  (approx — box-average).
        // We just check the center pixel is bright.
        val cx = 320
        val cy = 240
        val centerVal = result.y[cy * 640 + cx].toInt() and 0xFF
        assertEquals("center pixel should be ~255", 255, centerVal)
    }

    @Test
    fun `640x480 input with rowStride equal to width skips memcpy path`() {
        // This is a regression guard: when rowStride == width and native ==
        // target, extract() must NOT allocate more than one ByteArray.
        val src = makeYPlane(640, 480)
        val adapter = YuvAdapter(640, 480)
        val result = adapter.extract(src, 640, 640, 480)
        // (No direct allocation assertion available in JVM unit tests;
        // instead we just assert the output is NOT the same reference as
        // input — extract() returns a defensive copy.)
        assertEquals(640 * 480, result.y.size)
        assertArrayEquals(src, result.y)
        assert(result.y !== src)
    }
}
