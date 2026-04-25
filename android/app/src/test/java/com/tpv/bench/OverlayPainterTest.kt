package com.tpv.bench

import org.junit.Assert.assertEquals
import org.junit.Test

class OverlayPainterTest {

    private val nativeSize = OverlayPainter.NativeSize(1280, 720)
    private val crop = YuvAdapter.CropRect(160, 0, 960, 720)

    @Test
    fun `maps 640x480 center to native center of crop`() {
        val (nx, ny) = OverlayPainter.mapCoord(320, 240, crop)
        // Crop center in native coords: (160 + 480, 0 + 360) = (640, 360)
        assertEquals(640, nx)
        assertEquals(360, ny)
    }

    @Test
    fun `identity mapping when crop is 640x480`() {
        val c = YuvAdapter.CropRect(0, 0, 640, 480)
        val (nx, ny) = OverlayPainter.mapCoord(123, 456, c)
        assertEquals(123, nx)
        assertEquals(456, ny)
    }

    @Test
    fun `ACCEPTED uses class palette`() {
        val color = OverlayPainter.colorFor(detClsId = 2)
        assertEquals(OverlayPainter.PALETTE[2], color)
    }

    @Test
    fun `AMBIGUOUS uses amber warning color`() {
        assertEquals(OverlayPainter.AMBER_WARN, OverlayPainter.colorFor(0xFE))
    }

    @Test
    fun `REJECTED uses red error color`() {
        assertEquals(OverlayPainter.RED_ERR, OverlayPainter.colorFor(0xFF))
    }

    @Test
    fun `line 1 text shows d² on ACCEPTED`() {
        val d = TpvDetectionDebugV2(
            det = TpvDetection(0, 2, 320, 240, -450, 200),
            features = TpvFeatures(IntArray(7), 0, 0, 0),
            distancesSq = intArrayOf(9, 8, 7, 6, 5),
            bbox = TpvBbox(0, 0, 0, 0),
            areaPx = 0, grid8x8 = 0,
            bin = ByteArray(0), allBlobsMask = ByteArray(0), mask = ByteArray(0),
        )
        val line1 = OverlayPainter.textLine1(d)
        assertEquals("det_cls=2 conf=200 d²=7", line1)
    }

    @Test
    fun `line 1 text shows d²min on AMBIGUOUS`() {
        val d = TpvDetectionDebugV2(
            det = TpvDetection(0, 0xFE, 320, 240, 0, 0),
            features = TpvFeatures(IntArray(7), 0, 0, 0),
            distancesSq = intArrayOf(9, 8, 7, 6, 5),
            bbox = TpvBbox(0, 0, 0, 0),
            areaPx = 0, grid8x8 = 0,
            bin = ByteArray(0), allBlobsMask = ByteArray(0), mask = ByteArray(0),
        )
        val line1 = OverlayPainter.textLine1(d)
        assertEquals("det_cls=254 conf=0 d²min=5", line1)
    }

    @Test
    fun `line 1 text shows d²min on REJECTED`() {
        val d = TpvDetectionDebugV2(
            det = TpvDetection(0, 0xFF, 320, 240, 0, 0),
            features = TpvFeatures(IntArray(7), 0, 0, 0),
            distancesSq = intArrayOf(5, 4, 3, 2, 1),
            bbox = TpvBbox(0, 0, 0, 0),
            areaPx = 0, grid8x8 = 0,
            bin = ByteArray(0), allBlobsMask = ByteArray(0), mask = ByteArray(0),
        )
        val line1 = OverlayPainter.textLine1(d)
        assertEquals("det_cls=255 conf=0 d²min=1", line1)
    }

    @Test
    fun `line 2 text shows event_cls and flicker`() {
        val line2 = OverlayPainter.textLine2(eventClassId = 2, flicker = true)
        assertEquals("event_cls=2 flicker=true", line2)
    }
}
