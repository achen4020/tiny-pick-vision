package com.tpv.bench.vision

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class EventPolicyTest {
    @Test
    fun `live only face policy exposes detections without enabling commits`() {
        val policy = EventPolicy(
            EventPolicyConfig(
                primaryEventEngine = FACE_ENGINE_ID,
                enabledCommitEngines = emptySet(),
                mode = CommitMode.LIVE_ONLY,
            ),
            setOf(FACE_ENGINE_ID),
        )
        val face = detection(FACE_ENGINE_ID, 1)

        assertEquals(listOf(face), policy.primaryDetections(listOf(face)))
        assertFalse(policy.commitsEnabled(FACE_ENGINE_ID))
    }

    @Test
    fun `accepts default tpv config`() {
        val policy = EventPolicy(EventPolicyConfig(), setOf(TPV_BLOB_ENGINE_ID))
        assertEquals(TPV_BLOB_ENGINE_ID, policy.primaryEngineId)
    }

    @Test(expected = IllegalArgumentException::class)
    fun `rejects empty commit set`() {
        EventPolicy(
            EventPolicyConfig(enabledCommitEngines = emptySet()),
            setOf(TPV_BLOB_ENGINE_ID),
        )
    }

    @Test(expected = IllegalArgumentException::class)
    fun `rejects extra engine ids in primary only`() {
        EventPolicy(
            EventPolicyConfig(enabledCommitEngines = setOf(TPV_BLOB_ENGINE_ID, "face")),
            setOf(TPV_BLOB_ENGINE_ID, "face"),
        )
    }

    @Test(expected = IllegalArgumentException::class)
    fun `rejects unknown primary engine`() {
        EventPolicy(
            EventPolicyConfig(primaryEventEngine = "face", enabledCommitEngines = setOf("face")),
            setOf(TPV_BLOB_ENGINE_ID),
        )
    }

    @Test
    fun `filters detections and tracks to primary engine`() {
        val policy = EventPolicy(EventPolicyConfig(), setOf(TPV_BLOB_ENGINE_ID, "face"))
        val tpv = detection(TPV_BLOB_ENGINE_ID, 1)
        val face = detection("face", 2)
        assertEquals(listOf(tpv), policy.primaryDetections(listOf(tpv, face)))
        val tracks = listOf(
            TrackedDetection(tpv, 1, TrackState.CONFIRMED, 1, 1, 0),
            TrackedDetection(face, 2, TrackState.CONFIRMED, 1, 1, 0),
        )
        assertEquals(listOf(tracks[0]), policy.primaryTracks(tracks))
    }

    private fun detection(engineId: String, id: Long) = VisionDetection(
        engineId = engineId,
        detectionId = id,
        frameIdxInRun = id,
        classId = 0,
        className = "cls",
        score = 1f,
        bbox640 = RectI(0, 0, 10, 10),
    )
}
