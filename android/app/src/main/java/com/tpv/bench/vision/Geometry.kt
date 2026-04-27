package com.tpv.bench.vision

import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

data class RectI(val x: Int, val y: Int, val w: Int, val h: Int) {
    val x1Inclusive: Int get() = x + w - 1
    val y1Inclusive: Int get() = y + h - 1
    val area: Int get() = if (w <= 0 || h <= 0) 0 else w * h
    val centerX: Float get() = x + w / 2f
    val centerY: Float get() = y + h / 2f
}

data class PointI(val x: Int, val y: Int)

fun RectI.clampTo(bounds: RectI): RectI {
    if (area == 0 || bounds.area == 0) return RectI(bounds.x, bounds.y, 0, 0)
    val x0 = x.coerceIn(bounds.x, bounds.x + bounds.w)
    val y0 = y.coerceIn(bounds.y, bounds.y + bounds.h)
    val x1 = (x + w).coerceIn(bounds.x, bounds.x + bounds.w)
    val y1 = (y + h).coerceIn(bounds.y, bounds.y + bounds.h)
    return RectI(x0, y0, (x1 - x0).coerceAtLeast(0), (y1 - y0).coerceAtLeast(0))
}

fun RectI.iou(other: RectI): Float {
    if (area == 0 || other.area == 0) return 0f
    val ix0 = max(x, other.x)
    val iy0 = max(y, other.y)
    val ix1 = min(x + w, other.x + other.w)
    val iy1 = min(y + h, other.y + other.h)
    val iw = (ix1 - ix0).coerceAtLeast(0)
    val ih = (iy1 - iy0).coerceAtLeast(0)
    val intersection = iw * ih
    if (intersection == 0) return 0f
    return intersection.toFloat() / (area + other.area - intersection).toFloat()
}

fun RectI.centerDistanceTo(other: RectI): Float =
    hypot((centerX - other.centerX).toDouble(), (centerY - other.centerY).toDouble()).toFloat()

