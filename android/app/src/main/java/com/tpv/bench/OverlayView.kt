package com.tpv.bench

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.cos
import kotlin.math.sin

/**
 * Draws live TPV detection + persistent "last committed event" labels +
 * commit flash on top of the CameraX preview. The preview is a separate
 * PreviewView sibling; this View is transparent apart from the annotations.
 *
 * Thread model: CameraX callback thread calls updateLive() / onCommit();
 * UI thread calls onDraw(). Two AtomicReferences give a lock-free handoff.
 *
 * Persistence semantics (critical — see spec §5.5 + §9):
 *   - live fields (det, crop, sizes)      : overwritten EVERY frame
 *   - commit fields (eventClassId, ...)   : overwritten ONLY on onCommit()
 *   - flashEndMs                          : set ONLY on onCommit(), stays
 *                                           latched until naturally expires
 * A normal per-frame update must NOT clear flash or commit fields — that
 * was the P2-2 bug in the previous plan.
 */
class OverlayView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    private data class LiveState(
        val d: TpvDetectionDebug,
        val crop: YuvAdapter.CropRect,
        val nativeW: Int, val nativeH: Int,
    )
    private data class CommitState(
        val eventClassId: Int,
        val flicker: Boolean,
        val flashEndMs: Long,
    )

    private val live = AtomicReference<LiveState?>(null)
    private val commit = AtomicReference<CommitState?>(null)

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE ; strokeWidth = 4f
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 36f }
    private val flashPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 12f
        this.color = Color.GREEN   // `this.` to avoid shadow collision with outer `val color`
    }

    /** Called every frame (~24 fps). NEVER touches commit/flash state. */
    fun updateLive(
        d: TpvDetectionDebug, crop: YuvAdapter.CropRect,
        nativeW: Int, nativeH: Int,
    ) {
        live.set(LiveState(d, crop, nativeW, nativeH))
        postInvalidate()
    }

    /** Called when current frame has no valid detection (TPV_EMPTY or dropped).
     *  Clears the live circle/axis/line1 but leaves commit + flash state intact. */
    fun clearLive() {
        live.set(null)
        postInvalidate()
    }

    /** Called ONLY when TriggerMachine emits a Commit. */
    fun onCommit(eventClassId: Int, flicker: Boolean) {
        commit.set(CommitState(
            eventClassId = eventClassId, flicker = flicker,
            flashEndMs = System.currentTimeMillis() + 300
        ))
        postInvalidate()
    }

    /** Called by MainActivity.onStartClicked to wipe state at run boundary. */
    fun reset() {
        live.set(null) ; commit.set(null)
        postInvalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val f = live.get() ?: return
        if (width == 0 || height == 0) return    // pre-layout guard
        val w = width.toFloat() ; val h = height.toFloat()
        val sx = w / f.nativeW ; val sy = h / f.nativeH

        // --- Live annotation (every frame) ---
        val (nx, ny) = OverlayPainter.mapCoord(f.d.det.x, f.d.det.y, f.crop)
        val cx = nx * sx ; val cy = ny * sy
        val r = OverlayPainter.circleRadius(f.crop) * sx
        val axisLen = OverlayPainter.axisLength(f.crop) * sx
        val color = OverlayPainter.colorFor(f.d.det.classId)

        paint.color = color
        canvas.drawCircle(cx, cy, r, paint)

        val thetaRad = Math.toRadians(f.d.det.thetaX10 / 10.0)
        canvas.drawLine(
            cx, cy,
            (cx + axisLen * cos(thetaRad)).toFloat(),
            (cy + axisLen * sin(thetaRad)).toFloat(),
            paint
        )

        // Line 1 — live frame info (every frame updates)
        textPaint.color = color
        canvas.drawText(OverlayPainter.textLine1(f.d), 16f, 48f, textPaint)

        // Line 2 — last committed event info (persistent; empty until first commit)
        textPaint.color = OverlayPainter.GREY_NEUTRAL
        val c = commit.get()
        val line2 = if (c != null) OverlayPainter.textLine2(c.eventClassId, c.flicker)
                    else "event_cls=- flicker=-"
        canvas.drawText(line2, 16f, 96f, textPaint)

        // --- Commit flash (persistent until flashEndMs) ---
        if (c != null) {
            val nowMs = System.currentTimeMillis()
            val remaining = c.flashEndMs - nowMs
            if (remaining > 0) {
                flashPaint.alpha = (255 * remaining / 300).toInt().coerceIn(0, 255)
                canvas.drawRect(0f, 0f, w, h, flashPaint)
                postInvalidateDelayed(16)   // keep animating until the flash expires
            }
        }
    }
}
