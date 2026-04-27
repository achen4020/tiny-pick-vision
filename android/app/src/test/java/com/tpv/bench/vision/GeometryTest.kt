package com.tpv.bench.vision

import org.junit.Assert.assertEquals
import org.junit.Test

class GeometryTest {
    @Test
    fun `area is zero for invalid rects`() {
        assertEquals(0, RectI(0, 0, 0, 10).area)
        assertEquals(0, RectI(0, 0, 10, -1).area)
    }

    @Test
    fun `iou returns expected overlap`() {
        val a = RectI(0, 0, 10, 10)
        val b = RectI(5, 0, 10, 10)
        assertEquals(50f / 150f, a.iou(b), 0.0001f)
    }

    @Test
    fun `iou is zero for no overlap`() {
        assertEquals(0f, RectI(0, 0, 10, 10).iou(RectI(20, 20, 5, 5)), 0.0001f)
    }

    @Test
    fun `center distance uses rect centers`() {
        val distance = RectI(0, 0, 10, 10).centerDistanceTo(RectI(30, 40, 10, 10))
        assertEquals(50f, distance, 0.0001f)
    }

    @Test
    fun `clamp trims rect to bounds`() {
        assertEquals(RectI(0, 5, 15, 20), RectI(-5, 5, 20, 20).clampTo(RectI(0, 0, 15, 30)))
    }
}
