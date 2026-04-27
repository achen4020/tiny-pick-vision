package com.tpv.bench

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class Nv21ToArgbTest {
    @Test
    fun `neutral gray converts to opaque gray`() {
        val nv21 = ByteArray(2 * 2 * 3 / 2)
        nv21[0] = 128.toByte()
        nv21[1] = 128.toByte()
        nv21[2] = 128.toByte()
        nv21[3] = 128.toByte()
        nv21[4] = 128.toByte()
        nv21[5] = 128.toByte()
        val out = IntArray(4)

        Nv21ToArgb.convert(nv21, 2, 2, out)

        for (pixel in out) {
            val r = (pixel ushr 16) and 0xFF
            val g = (pixel ushr 8) and 0xFF
            val b = pixel and 0xFF
            assertEquals(0xFF, (pixel ushr 24) and 0xFF)
            assertTrue(abs(r - g) <= 2)
            assertTrue(abs(g - b) <= 2)
        }
    }

    @Test(expected = IllegalArgumentException::class)
    fun `requires even dimensions`() {
        Nv21ToArgb.convert(ByteArray(10), 3, 2, IntArray(6))
    }
}
