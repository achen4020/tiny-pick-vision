package com.tpv.bench.vision

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class MultiObjectTrackerTest {
    @Test
    fun `single static detection keeps same id`() {
        val tracker = MultiObjectTracker()
        val first = tracker.update(listOf(detection(1, RectI(100, 100, 20, 20)))).single()
        val second = tracker.update(listOf(detection(2, RectI(100, 100, 20, 20)))).single()
        assertEquals(first.trackId, second.trackId)
        assertEquals(TrackState.CONFIRMED, second.state)
    }

    @Test
    fun `slow moving detection keeps same id`() {
        val tracker = MultiObjectTracker()
        val first = tracker.update(listOf(detection(1, RectI(100, 100, 20, 20)))).single()
        val second = tracker.update(listOf(detection(2, RectI(120, 100, 20, 20)))).single()
        assertEquals(first.trackId, second.trackId)
    }

    @Test
    fun `missing detections before max age keep lost track alive`() {
        val tracker = MultiObjectTracker(TrackerConfig(maxAge = 2))
        tracker.update(listOf(detection(1, RectI(100, 100, 20, 20))))
        tracker.update(listOf(detection(2, RectI(100, 100, 20, 20))))
        val lost = tracker.update(emptyList()).single()
        assertEquals(TrackState.LOST, lost.state)
        assertEquals(1, lost.misses)
    }

    @Test
    fun `missing detections past max age removes track`() {
        val tracker = MultiObjectTracker(TrackerConfig(maxAge = 1))
        tracker.update(listOf(detection(1, RectI(100, 100, 20, 20))))
        tracker.update(emptyList())
        assertEquals(emptyList<TrackedDetection>(), tracker.update(emptyList()))
    }

    @Test
    fun `two synthetic objects keep bounded id switches while crossing`() {
        val tracker = MultiObjectTracker(TrackerConfig(centerDistancePx = 200f))
        val frame1 = tracker.update(listOf(
            detection(1, RectI(100, 100, 30, 30), detectionId = 1),
            detection(1, RectI(300, 100, 30, 30), detectionId = 2),
        ))
        val frame2 = tracker.update(listOf(
            detection(2, RectI(140, 100, 30, 30), detectionId = 3),
            detection(2, RectI(260, 100, 30, 30), detectionId = 4),
        ))
        assertEquals(frame1.map { it.trackId }.toSet(), frame2.map { it.trackId }.toSet())
    }

    @Test
    fun `different engine or class groups do not match`() {
        val tracker = MultiObjectTracker(TrackerConfig(centerDistancePx = 200f))
        val first = tracker.update(listOf(detection(1, RectI(100, 100, 20, 20), engineId = "a"))).single()
        val second = tracker.update(listOf(detection(2, RectI(100, 100, 20, 20), engineId = "b")))
            .first { it.detection.engineId == "b" }
        assertNotEquals(first.trackId, second.trackId)

        val classTracker = MultiObjectTracker(TrackerConfig(centerDistancePx = 200f))
        val c1 = classTracker.update(listOf(detection(1, RectI(100, 100, 20, 20), className = "a"))).single()
        val c2 = classTracker.update(listOf(detection(2, RectI(100, 100, 20, 20), className = "b")))
            .first { it.detection.className == "b" }
        assertNotEquals(c1.trackId, c2.trackId)
    }

    @Test
    fun `reset restarts track ids`() {
        val tracker = MultiObjectTracker()
        assertEquals(1L, tracker.update(listOf(detection(1, RectI(0, 0, 10, 10)))).single().trackId)
        tracker.reset()
        assertEquals(1L, tracker.update(listOf(detection(2, RectI(0, 0, 10, 10)))).single().trackId)
    }

    @Test
    fun `snapshot can exclude lost tracks`() {
        val tracker = MultiObjectTracker(TrackerConfig(maxAge = 2))
        tracker.update(listOf(detection(1, RectI(0, 0, 10, 10))))
        tracker.update(emptyList())
        assertTrue(tracker.snapshot(includeLost = false).isEmpty())
    }

    private fun detection(
        frameIdx: Long,
        bbox: RectI,
        engineId: String = TPV_BLOB_ENGINE_ID,
        className: String = "tpv_0",
        detectionId: Long = frameIdx,
    ) = VisionDetection(
        engineId = engineId,
        detectionId = detectionId,
        frameIdxInRun = frameIdx,
        classId = 0,
        className = className,
        score = 1f,
        bbox640 = bbox,
    )
}

