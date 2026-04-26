package com.tpv.bench

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.os.SystemClock
import android.util.AttributeSet
import android.view.View
import java.util.concurrent.atomic.AtomicReference

/**
 * Six-cell diagnostic panel showing pipeline intermediate stages.
 * Pure rendering logic in DiagnosticsRenderer; this View just lays out
 * six Bitmaps plus labels on a Canvas.
 *
 * Thread model: camera executor calls update(); UI thread calls onDraw.
 * An AtomicReference hands off the latest Panels.
 *
 * Throttled to ~10 Hz — DiagnosticsRenderer is lightweight but 6 IntArrays
 * per frame @ 24 fps is wasteful when the human eye only perceives ~10 Hz.
 */
class DiagnosticsView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null,
) : View(context, attrs) {

    private data class State(
        val panels: DiagnosticsRenderer.Panels,
    )

    private val latest = AtomicReference<State?>(null)
    private val bitmaps = Array(6) {
        Bitmap.createBitmap(
            DiagnosticsRenderer.TILE_W,
            DiagnosticsRenderer.TILE_H,
            Bitmap.Config.ARGB_8888,
        )
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 20f ; color = Color.WHITE
    }
    private val bgPaint = Paint().apply { color = 0xFF101010.toInt() }

    @Volatile private var lastUpdateMs: Long = 0L
    private val throttleMs = 100L  // 10 Hz

    fun update(panels: DiagnosticsRenderer.Panels) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastUpdateMs < throttleMs) return
        lastUpdateMs = now
        latest.set(State(panels))
        postInvalidate()
    }

    fun clear() { latest.set(null) ; postInvalidate() }

    override fun onDraw(canvas: Canvas) {
        val s = latest.get() ?: return
        val vw = width ; val vh = height
        if (vw == 0 || vh == 0) return
        canvas.drawRect(0f, 0f, vw.toFloat(), vh.toFloat(), bgPaint)

        // Fill the 6 prepared bitmaps with the latest IntArray pixels
        val p = s.panels
        bitmaps[0].setPixels(p.rawY,        0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[1].setPixels(p.roiCrop,     0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[2].setPixels(p.binarized,   0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[3].setPixels(p.allBlobs,    0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[4].setPixels(p.winningBlob, 0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)
        bitmaps[5].setPixels(p.lastEventMask,0, DiagnosticsRenderer.TILE_W, 0, 0, DiagnosticsRenderer.TILE_W, DiagnosticsRenderer.TILE_H)

        val labels = arrayOf("raw Y", "ROI", "bin", "all blobs", "winner", "last event")

        // 2 rows × 3 cols layout
        val cellW = vw / 3
        val cellH = vh / 2
        val padX = 4
        val padY = 4
        val labelH = 24
        for (cell in 0 until 6) {
            val row = cell / 3 ; val col = cell % 3
            val x0 = col * cellW + padX
            val y0 = row * cellH + padY
            val bmpW = cellW - 2 * padX
            val bmpH = cellH - 2 * padY - labelH
            val dst = android.graphics.RectF(
                x0.toFloat(), y0.toFloat(),
                (x0 + bmpW).toFloat(), (y0 + bmpH).toFloat(),
            )
            canvas.drawBitmap(bitmaps[cell], null, dst, null)
            canvas.drawText(labels[cell], x0.toFloat(), (y0 + bmpH + labelH - 4).toFloat(), labelPaint)
        }
    }
}
