package com.tpv.bench.vision

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DetectionCadenceTest {
    @Test
    fun `twelve fps cadence skips intermediate camera frames`() {
        val cadence = DetectionCadence(targetFps = 12)

        assertTrue(cadence.shouldDetect(0L))
        assertFalse(cadence.shouldDetect(40_000_000L))
        assertTrue(cadence.shouldDetect(84_000_000L))
    }

    @Test
    fun `failed scheduled inference clears detections carried to later frames`() {
        val state = DetectionCarryState(targetFps = 12)
        val detection = VisionDetection(
            engineId = FACE_ENGINE_ID,
            detectionId = 10_000L,
            frameIdxInRun = 1L,
            classId = 0,
            className = "face",
            score = 0.9f,
            bbox640 = RectI(10, 20, 30, 40),
        )

        assertTrue(state.shouldDetect(0L))
        state.onSuccess(listOf(detection))
        assertTrue(state.carryToFrame(2L).isNotEmpty())

        assertTrue(state.shouldDetect(84_000_000L))
        state.onFailure()

        assertTrue(state.carryToFrame(3L).isEmpty())
    }
}
