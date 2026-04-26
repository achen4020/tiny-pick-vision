package com.tpv.bench

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticsRendererTest {

    private val MASK_BYTES = 640 * 480 / 8
    private val Y_BYTES = 640 * 480

    private fun detection() = TpvDetectionDebugV2(
        det = TpvDetection(0, 2, 320, 240, 0, 200),
        features = TpvFeatures(IntArray(7), 0, 0, 0),
        distancesSq = IntArray(5),
        bbox = TpvBbox(0, 0, 0, 0),
        areaPx = 0,
        grid8x8 = 0,
        bin = ByteArray(MASK_BYTES),
        allBlobsMask = ByteArray(MASK_BYTES),
        mask = ByteArray(MASK_BYTES),
    )

    @Test
    fun `render returns six tiles each TILE_W times TILE_H size`() {
        val rawY = ByteArray(Y_BYTES) { 128.toByte() }
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        val expected = DiagnosticsRenderer.TILE_W * DiagnosticsRenderer.TILE_H
        assertEquals(expected, panels.rawY.size)
        assertEquals(expected, panels.roiCrop.size)
        assertEquals(expected, panels.binarized.size)
        assertEquals(expected, panels.allBlobs.size)
        assertEquals(expected, panels.winningBlob.size)
        assertEquals(expected, panels.lastEventMask.size)
    }

    @Test
    fun `raw Y tile preserves luminance mid-gray`() {
        val rawY = ByteArray(Y_BYTES) { 128.toByte() }
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        // Any pixel should be ARGB = FF808080
        val expected = 0xFF808080.toInt()
        for (p in panels.rawY) assertEquals(expected, p)
    }

    @Test
    fun `binarized with all-zero bin yields all background`() {
        val rawY = ByteArray(Y_BYTES)
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        // bg color is 0xFF000000 per renderer impl
        for (p in panels.binarized) assertEquals(0xFF000000.toInt(), p)
    }

    @Test
    fun `winning blob tile is all-black when mask is empty`() {
        val rawY = ByteArray(Y_BYTES)
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        // bg color for winning blob tile is 0xFF202020 per renderer impl
        for (p in panels.winningBlob) assertEquals(0xFF202020.toInt(), p)
    }

    @Test
    fun `winning blob tile shows green when mask bit is set at 0,0`() {
        val rawY = ByteArray(Y_BYTES)
        val d = detection()
        d.mask[0] = 0x01  // pixel (0, 0) set
        val panels = DiagnosticsRenderer.render(
            rawY, d, YuvAdapter.CropRect(0, 0, 640, 480), null)
        // Tile pixel (0, 0) samples source (0, 0) → should be green
        assertEquals(0xFF00FF00.toInt(), panels.winningBlob[0])
    }

    @Test
    fun `lastEventMask null yields all dark gray tile`() {
        val rawY = ByteArray(Y_BYTES)
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 640, 480), null)
        for (p in panels.lastEventMask) assertEquals(0xFF202020.toInt(), p)
    }

    @Test
    fun `ROI dim darkens pixels outside the ROI rect`() {
        val rawY = ByteArray(Y_BYTES) { 200.toByte() }
        // ROI covers only top-left 320×240; bottom-right should be dimmed
        val panels = DiagnosticsRenderer.render(
            rawY, detection(), YuvAdapter.CropRect(0, 0, 320, 240), null)
        // Inside ROI (tile 0,0): preserved 200
        val insideArgb = 0xFF shl 24 or (200 shl 16) or (200 shl 8) or 200
        assertEquals(insideArgb, panels.roiCrop[0])
        // Outside ROI (bottom-right corner): dimmed to 200 * 40 / 100 = 80
        val outsideArgb = 0xFF shl 24 or (80 shl 16) or (80 shl 8) or 80
        val brCorner = (DiagnosticsRenderer.TILE_H - 1) * DiagnosticsRenderer.TILE_W +
            (DiagnosticsRenderer.TILE_W - 1)
        assertEquals(outsideArgb, panels.roiCrop[brCorner])
    }
}
