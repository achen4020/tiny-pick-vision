package com.tpv.bench

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class TriggerMachineTest {

    private fun makeDetection(classId: Int, x: Int = 320, y: Int = 240) =
        TpvDetectionDebug(
            det = TpvDetection(
                status = 0, classId = classId,
                x = x, y = y, thetaX10 = 0, confidenceQ8 = if (classId < 5) 200 else 0
            ),
            features = TpvFeatures(IntArray(7), 0, 0, 0),
            distancesSq = IntArray(5)
        )

    private fun present(classId: Int, x: Int = 320, y: Int = 240, idx: Long = 0L) =
        FrameObservation(
            presence = FramePresence.PRESENT,
            x = x, y = y, classId = classId, frameIdxInRun = idx,
            detection = makeDetection(classId, x, y)
        )

    private fun empty(idx: Long = 0L) = FrameObservation(
        presence = FramePresence.EMPTY,
        x = 0, y = 0, classId = -1, frameIdxInRun = idx, detection = null
    )

    private fun drop(idx: Long = 0L) = FrameObservation(
        presence = FramePresence.DROP,
        x = 0, y = 0, classId = -1, frameIdxInRun = idx, detection = null
    )

    @Test
    fun `starts in IDLE with no output`() {
        val tm = TriggerMachine(nStable = 3, kEmpty = 5, mDriftPx = 30)
        assertEquals(MachineState.IDLE, tm.state)
    }

    @Test
    fun `three stable present frames promote to COMMITTED`() {
        val tm = TriggerMachine(3, 5, 30)
        assertEquals(StateMachineOutput.None, tm.onFrame(present(2, 320, 240, 1)))
        assertEquals(MachineState.CANDIDATE, tm.state)
        assertEquals(StateMachineOutput.None, tm.onFrame(present(2, 321, 241, 2)))
        val out = tm.onFrame(present(2, 322, 242, 3))
        assertTrue("should commit on frame 3", out is StateMachineOutput.Commit)
        assertEquals(MachineState.COMMITTED, tm.state)
        val commit = out as StateMachineOutput.Commit
        assertEquals(1L, commit.event.eventIdx)
        assertEquals(3L, commit.event.triggerFrameIdx)
        assertEquals(2, commit.event.eventClassId)
        assertFalse(commit.event.flicker)
        assertEquals(mapOf(2 to 3), commit.event.classIdHistogram)
    }

    @Test
    fun `position drift beyond M resets to IDLE`() {
        val tm = TriggerMachine(3, 5, 30)
        tm.onFrame(present(2, 320, 240, 1))
        tm.onFrame(present(2, 321, 241, 2))
        // 31-pixel x drift from window-first frame (320) — exceeds M=30
        val out = tm.onFrame(present(2, 351, 241, 3))
        assertEquals(StateMachineOutput.None, out)
        assertEquals(MachineState.IDLE, tm.state)
    }

    @Test
    fun `EMPTY during CANDIDATE resets to IDLE`() {
        val tm = TriggerMachine(3, 5, 30)
        tm.onFrame(present(2, 320, 240, 1))
        tm.onFrame(present(2, 320, 240, 2))
        tm.onFrame(empty(3))
        assertEquals(MachineState.IDLE, tm.state)
    }

    @Test
    fun `DROP during CANDIDATE does not advance or reset`() {
        val tm = TriggerMachine(3, 5, 30)
        tm.onFrame(present(2, 320, 240, 1))
        tm.onFrame(drop(2))
        assertEquals(MachineState.CANDIDATE, tm.state)
        tm.onFrame(present(2, 320, 240, 3))
        val out = tm.onFrame(present(2, 320, 240, 4))
        assertTrue(out is StateMachineOutput.Commit)
    }

    @Test
    fun `COMMITTED returns to IDLE after K consecutive EMPTY frames`() {
        val tm = TriggerMachine(nStable = 3, kEmpty = 5, mDriftPx = 30)
        // Get into COMMITTED
        repeat(3) { tm.onFrame(present(2, 320, 240, (it + 1).toLong())) }
        assertEquals(MachineState.COMMITTED, tm.state)
        // 4 EMPTY frames: stay COMMITTED
        repeat(4) { tm.onFrame(empty((4 + it).toLong())) }
        assertEquals(MachineState.COMMITTED, tm.state)
        // 5th EMPTY frame: return to IDLE (no new commit output)
        val out = tm.onFrame(empty(8))
        assertEquals(StateMachineOutput.None, out)
        assertEquals(MachineState.IDLE, tm.state)
    }

    @Test
    fun `intermittent PRESENT in COMMITTED resets empty counter`() {
        val tm = TriggerMachine(3, 5, 30)
        repeat(3) { tm.onFrame(present(2, 320, 240, (it + 1).toLong())) }
        assertEquals(MachineState.COMMITTED, tm.state)
        tm.onFrame(empty(4))
        tm.onFrame(empty(5))
        tm.onFrame(present(2, 320, 240, 6))   // resets empty_count
        assertEquals(MachineState.COMMITTED, tm.state)
        // Now 5 more EMPTY needed (not 3)
        repeat(4) { tm.onFrame(empty((7 + it).toLong())) }
        assertEquals(MachineState.COMMITTED, tm.state)
        tm.onFrame(empty(11))
        assertEquals(MachineState.IDLE, tm.state)
    }

    @Test
    fun `flickering object with class_id swapping still commits — spec §4 4 4`() {
        // The whole point: classId in the window is [2, 254, 255] — none is
        // the same, yet position is stable. Must commit.
        val tm = TriggerMachine(3, 5, 30)
        tm.onFrame(present(2, 320, 240, 1))
        tm.onFrame(present(254, 321, 239, 2))
        val out = tm.onFrame(present(255, 319, 241, 3))
        assertTrue(out is StateMachineOutput.Commit)
        val ev = (out as StateMachineOutput.Commit).event
        assertTrue(ev.flicker)
        assertEquals(
            mapOf(2 to 1, 254 to 1, 255 to 1),
            ev.classIdHistogram
        )
        // Tie-break: real classes (0..4) > 0xFE > 0xFF, so event_class_id = 2
        assertEquals(2, ev.eventClassId)
    }

    @Test
    fun `majority vote picks most-frequent class`() {
        val tm = TriggerMachine(3, 5, 30)
        tm.onFrame(present(2, 320, 240, 1))
        tm.onFrame(present(2, 320, 240, 2))
        val out = tm.onFrame(present(254, 320, 240, 3))
        val ev = (out as StateMachineOutput.Commit).event
        assertEquals(2, ev.eventClassId)
        assertTrue(ev.flicker)
        assertEquals(mapOf(2 to 2, 254 to 1), ev.classIdHistogram)
    }

    @Test
    fun `trigger frame detection is the Nth frame's tpv output`() {
        val tm = TriggerMachine(3, 5, 30)
        tm.onFrame(present(2, 310, 230, 1))
        tm.onFrame(present(2, 315, 235, 2))
        val out = tm.onFrame(present(254, 320, 240, 3))
        val ev = (out as StateMachineOutput.Commit).event
        // triggerFrameDebug must be the 3rd frame's detection (classId=254)
        assertEquals(254, ev.triggerFrameDebug.det.classId)
        assertEquals(320, ev.triggerFrameDebug.det.x)
        assertEquals(240, ev.triggerFrameDebug.det.y)
        assertEquals(3L, ev.triggerFrameIdx)
    }

    @Test
    fun `second commit requires full cycle back to IDLE`() {
        val tm = TriggerMachine(3, 5, 30)
        repeat(3) { tm.onFrame(present(2, 320, 240, (it + 1).toLong())) }
        // Already COMMITTED — another PRESENT frame must NOT produce a new commit
        val out = tm.onFrame(present(2, 320, 240, 4))
        assertEquals(StateMachineOutput.None, out)
        // Only after K EMPTY + new stable PRESENT sequence do we commit again
        repeat(5) { tm.onFrame(empty((5 + it).toLong())) }
        assertEquals(MachineState.IDLE, tm.state)
        repeat(2) { tm.onFrame(present(2, 320, 240, (10 + it).toLong())) }
        val out2 = tm.onFrame(present(2, 320, 240, 12))
        assertTrue(out2 is StateMachineOutput.Commit)
        assertEquals(2L, (out2 as StateMachineOutput.Commit).event.eventIdx)
    }
}
