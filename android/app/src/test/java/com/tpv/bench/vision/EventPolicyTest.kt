package com.tpv.bench.vision

import org.junit.Assert.assertEquals
import org.junit.Test

class EventPolicyTest {
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

