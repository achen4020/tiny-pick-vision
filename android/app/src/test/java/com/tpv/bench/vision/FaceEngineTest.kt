package com.tpv.bench.vision

import com.tpv.bench.YuvAdapter
import org.junit.Assert.assertEquals
import org.junit.Test

class FaceEngineTest {
    @Test
    fun `native face bbox maps through centered crop into 640 space`() {
        val crop = YuvAdapter.CropRect(160, 0, 960, 720)

        val mapped = FaceEngine.nativeRectTo640(
            left = 400f,
            top = 180f,
            right = 640f,
            bottom = 360f,
            crop = crop,
        )

        assertEquals(RectI(160, 120, 160, 120), mapped)
    }

    @Test
    fun `native face bbox clamps outside crop`() {
        val crop = YuvAdapter.CropRect(160, 0, 960, 720)

        val mapped = FaceEngine.nativeRectTo640(
            left = 0f,
            top = -50f,
            right = 2000f,
            bottom = 900f,
            crop = crop,
        )

        assertEquals(RectI(0, 0, 640, 480), mapped)
    }

    @Test
    fun `native landmark maps and clamps into frame`() {
        val crop = YuvAdapter.CropRect(160, 0, 960, 720)

        val point = FaceEngine.nativePointTo640(nativeX = 1120, nativeY = 720, crop = crop)

        assertEquals(PointI(639, 479), point)
    }
}
