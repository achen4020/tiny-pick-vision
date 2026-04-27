package com.tpv.bench

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
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
 *
 * v2 (T-v2.4): onDraw paints yellow ROI + green mask fill (only on TPV_OK)
 * + red center dot + short axis line. v1's circle + 2-line text is dropped;
 * status text moves to the HUD/status_line in T-v2.6.
 */
class OverlayView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null
) : View(context, attrs) {

    private data class LiveState(
        val d: TpvDetectionDebugV2,
        val roi: YuvAdapter.CropRect,         // ROI in 640×480 coords
        val crop: YuvAdapter.CropRect,        // camera→640×480 crop
        val nativeW: Int, val nativeH: Int,
    )
    private data class CommitState(
        val eventClassId: Int,
        val flicker: Boolean,
        val flashEndMs: Long,
    )

    private val live = AtomicReference<LiveState?>(null)
    private val commit = AtomicReference<CommitState?>(null)

    private val roiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE ; strokeWidth = 2f
        this.color = OverlayPainter.YELLOW_ROI_ARGB
    }
    private val centerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        this.color = OverlayPainter.RED_CENTER_ARGB
    }
    private val axisPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE ; strokeWidth = 3f
        this.color = OverlayPainter.RED_CENTER_ARGB
    }
    private val flashPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 12f
        this.color = OverlayPainter.GREEN_FLASH_ARGB
    }

    // Cached buffers for mask rendering. onDraw runs at ~24 fps; pre-allocating
    // avoids 2.4 MB / frame Java heap churn (~58 MB/s GC pressure). Reuse via
    // IntArray overload of decodeMaskToArgb + Bitmap.setPixels.
    private val maskPixels = IntArray(640 * 480)
    private val maskBitmap = Bitmap.createBitmap(640, 480, Bitmap.Config.ARGB_8888)

    /** Called every frame (~24 fps). NEVER touches commit/flash state. */
    fun updateLive(
        d: TpvDetectionDebugV2,
        roi: YuvAdapter.CropRect,
        crop: YuvAdapter.CropRect,
        nativeW: Int, nativeH: Int,
    ) {
        live.set(LiveState(d, roi, crop, nativeW, nativeH))
        postInvalidate()
    }

    /** Called when current frame has no valid detection (TPV_EMPTY or dropped).
     *  Clears the live mask/dot/axis but leaves commit + flash state intact. */
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
        val vw = width.toFloat() ; val vh = height.toFloat()
        val viewTransform = OverlayPainter.previewFillCenterTransform(
            width, height, f.nativeW, f.nativeH,
        )

        // ---- 1. Yellow ROI rectangle (drawn every frame, regardless of status) ----
        // ROI is in 640×480 coords; map both corners through crop → native → view.
        val (roiNx0, roiNy0) = OverlayPainter.mapCoord(f.roi.x, f.roi.y, f.crop)
        val (roiNx1, roiNy1) = OverlayPainter.mapCoord(
            f.roi.x + f.roi.w, f.roi.y + f.roi.h, f.crop)
        val (roiVx0, roiVy0) = OverlayPainter.mapNativeToView(roiNx0, roiNy0, viewTransform)
        val (roiVx1, roiVy1) = OverlayPainter.mapNativeToView(roiNx1, roiNy1, viewTransform)
        canvas.drawRect(
            roiVx0, roiVy0, roiVx1, roiVy1, roiPaint
        )

        // ---- 2. Green mask fill + red center dot + short axis line ----
        // Only draw when the C side reported TPV_OK (status == 0). On
        // TPV_EMPTY / SCENE_ERROR / BAD_INPUT the mask is all zeros and the
        // det struct is zero-filled, so painting at (0,0) would be misleading.
        val status = f.d.det.status
        if (status == 0) {
            // Decode v2.mask into our cached IntArray, push into cached Bitmap, draw.
            OverlayPainter.decodeMaskToArgb(
                f.d.mask, 640, 480, OverlayPainter.GREEN_MASK_ARGB, maskPixels)
            maskBitmap.setPixels(maskPixels, 0, 640, 0, 0, 640, 480)
            // Destination: map the 640×480 mask into the camera→target crop
            // rect on the native frame, then to view coords.
            val dstLeft   = viewTransform.offsetX + f.crop.x * viewTransform.scale
            val dstTop    = viewTransform.offsetY + f.crop.y * viewTransform.scale
            val dstRight  = viewTransform.offsetX + (f.crop.x + f.crop.w) * viewTransform.scale
            val dstBottom = viewTransform.offsetY + (f.crop.y + f.crop.h) * viewTransform.scale
            val dstRect = RectF(dstLeft, dstTop, dstRight, dstBottom)
            canvas.drawBitmap(maskBitmap, null, dstRect, null)

            // Red center dot
            val (nx, ny) = OverlayPainter.mapCoord(f.d.det.x, f.d.det.y, f.crop)
            val (cx, cy) = OverlayPainter.mapNativeToView(nx, ny, viewTransform)
            val dotR = (f.crop.w * 0.015f * viewTransform.scale).coerceAtLeast(4f)
            canvas.drawCircle(cx, cy, dotR, centerPaint)

            // Short axis line
            val axisLen = (f.crop.w * 0.04f * viewTransform.scale).coerceAtLeast(8f)
            val thetaRad = Math.toRadians(f.d.det.thetaX10 / 10.0)
            canvas.drawLine(
                cx, cy,
                (cx + axisLen * cos(thetaRad)).toFloat(),
                (cy + axisLen * sin(thetaRad)).toFloat(),
                axisPaint
            )
        }

        // ---- 3. Commit flash (persistent until flashEndMs) ----
        val c = commit.get()
        if (c != null) {
            val nowMs = System.currentTimeMillis()
            val remaining = c.flashEndMs - nowMs
            if (remaining > 0) {
                flashPaint.alpha = (255 * remaining / 300).toInt().coerceIn(0, 255)
                canvas.drawRect(0f, 0f, vw, vh, flashPaint)
                postInvalidateDelayed(16)   // keep animating until the flash expires
            }
        }
    }
}
