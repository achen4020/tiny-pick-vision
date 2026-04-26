package com.tpv.bench

/**
 * Pure logic: turns TpvDetectionDebugV2 + latest raw Y frame + ROI into
 * six IntArray pixel arrays (ARGB), each downsampled to a fixed small tile
 * size. JVM-testable without Android. DiagnosticsView wraps these into
 * Bitmaps and lays them out on a Canvas.
 *
 * Output tile dimensions: 160×120 per cell (small enough to be cheap,
 * large enough to see shape). 6 cells laid out 2 rows × 3 cols.
 */
object DiagnosticsRenderer {

    const val TILE_W = 160
    const val TILE_H = 120

    /** Result of rendering: six tiles in documented order. */
    data class Panels(
        val rawY: IntArray,
        val roiCrop: IntArray,
        val binarized: IntArray,
        val allBlobs: IntArray,
        val winningBlob: IntArray,
        val lastEventMask: IntArray,
    )

    /**
     * Build 6 diagnostic tiles from the latest frame.
     *
     * **Input dimensions are pinned to 640×480** by tpv's compile-time
     * configuration (TPV_WIDTH/TPV_HEIGHT). Every literal `640`/`480` in
     * this function reflects that single source of truth — if tpv ever
     * supports a different working resolution, audit `render()` and
     * `bitmapToTile`'s callers in lockstep with `tpv_config.h`.
     *
     * @param rawY          current frame 640×480 Y buffer (must be 307200 B)
     * @param d             latest detection (v2)
     * @param roi           ROI rect in 640×480 coords
     * @param lastEventMask last committed event's mask (null until first commit)
     */
    fun render(
        rawY: ByteArray,
        d: TpvDetectionDebugV2,
        roi: YuvAdapter.CropRect,
        lastEventMask: ByteArray?,
    ): Panels {
        require(rawY.size == 640 * 480) { "rawY must be 640*480 bytes" }

        val tileBytes = TILE_W * TILE_H

        // 1. Raw Y, downsampled to TILE_W × TILE_H, grayscale ARGB
        val rawTile = IntArray(tileBytes)
        for (ty in 0 until TILE_H) {
            val syIdx = (ty * 480) / TILE_H
            for (tx in 0 until TILE_W) {
                val sxIdx = (tx * 640) / TILE_W
                val v = rawY[syIdx * 640 + sxIdx].toInt() and 0xFF
                rawTile[ty * TILE_W + tx] = 0xFF shl 24 or (v shl 16) or (v shl 8) or v
            }
        }

        // 2. ROI crop: same as raw but with pixels outside ROI dimmed to 40%
        val roiTile = IntArray(tileBytes)
        for (ty in 0 until TILE_H) {
            val syIdx = (ty * 480) / TILE_H
            val inRow = (syIdx >= roi.y && syIdx < roi.y + roi.h)
            for (tx in 0 until TILE_W) {
                val sxIdx = (tx * 640) / TILE_W
                val v = rawY[syIdx * 640 + sxIdx].toInt() and 0xFF
                val inRoi = inRow && sxIdx >= roi.x && sxIdx < roi.x + roi.w
                val vv = if (inRoi) v else v * 40 / 100
                roiTile[ty * TILE_W + tx] = 0xFF shl 24 or (vv shl 16) or (vv shl 8) or vv
            }
        }

        // 3. Binarized: d.bin → black/white
        val binTile = bitmapToTile(d.bin, 640, 480, 0xFFFFFFFF.toInt(), 0xFF000000.toInt())

        // 4. All blobs: d.allBlobsMask → gray fg on black bg
        val allTile = bitmapToTile(d.allBlobsMask, 640, 480, 0xFFAAAAAA.toInt(), 0xFF202020.toInt())

        // 5. Winning blob: d.mask → green fg on black bg
        val winTile = bitmapToTile(d.mask, 640, 480, 0xFF00FF00.toInt(), 0xFF202020.toInt())

        // 6. Last event mask: same as 5 but using committed event's snapshot
        val evTile = if (lastEventMask != null)
            bitmapToTile(lastEventMask, 640, 480, 0xFF00FF00.toInt(), 0xFF202020.toInt())
        else
            IntArray(tileBytes) { 0xFF202020.toInt() }

        return Panels(rawTile, roiTile, binTile, allTile, winTile, evTile)
    }

    /**
     * Convert a packed bitmap (LSB-first, `w*h/8` bytes) into a TILE_W×TILE_H
     * ARGB tile where set bits → `fgArgb`, clear bits → `bgArgb`. Nearest-
     * neighbour downsample.
     */
    internal fun bitmapToTile(
        bits: ByteArray, w: Int, h: Int, fgArgb: Int, bgArgb: Int,
    ): IntArray {
        require(bits.size == w * h / 8) { "bits size mismatch" }
        val out = IntArray(TILE_W * TILE_H)
        for (ty in 0 until TILE_H) {
            val sy = (ty * h) / TILE_H
            for (tx in 0 until TILE_W) {
                val sx = (tx * w) / TILE_W
                val i = sy * w + sx
                val bit = (bits[i shr 3].toInt() ushr (i and 7)) and 1
                out[ty * TILE_W + tx] = if (bit == 1) fgArgb else bgArgb
            }
        }
        return out
    }
}
