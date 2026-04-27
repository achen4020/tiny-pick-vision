package com.tpv.bench.vision

import com.tpv.bench.YuvAdapter
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class VisionPipelineTest {
    @Test
    fun `pipeline invokes enabled engines in deterministic id order`() {
        val calls = ArrayList<String>()
        val pipeline = VisionPipeline(
            listOf(fakeEngine("z", calls), fakeEngine("a", calls)),
            NoopObjectTracker(),
            EventPolicyConfig(primaryEventEngine = "a", enabledCommitEngines = setOf("a")),
        )
        pipeline.process(frame())
        assertEquals(listOf("a", "z"), calls)
    }

    @Test
    fun `pipeline passes detection through noop tracker`() {
        val detection = detection(TPV_BLOB_ENGINE_ID, 1)
        val pipeline = VisionPipeline(
            listOf(fakeEngine(TPV_BLOB_ENGINE_ID, detections = listOf(detection))),
            NoopObjectTracker(),
        )
        val result = pipeline.process(frame())
        assertEquals(listOf(detection), result.primaryDetections)
        assertEquals(1L, result.primaryTracks.single().trackId)
        assertEquals(TrackState.CONFIRMED, result.primaryTracks.single().state)
    }

    @Test
    fun `native engine tracks bypass kotlin tracker`() {
        val detection = detection(TPV_BLOB_ENGINE_ID, 9)
        val nativeTrack = TrackedDetection(
            detection = detection,
            trackId = 77,
            state = TrackState.TENTATIVE,
            ageFrames = 1,
            hits = 1,
            misses = 0,
        )
        val pipeline = VisionPipeline(
            listOf(fakeEngine(TPV_BLOB_ENGINE_ID, detections = listOf(detection), nativeTracks = listOf(nativeTrack))),
            MultiObjectTracker(),
        )

        val result = pipeline.process(frame())

        assertEquals(listOf(nativeTrack), result.primaryTracks)
        assertEquals(77L, result.primaryTracks.single().trackId)
    }

    @Test
    fun `event policy returns only primary engine detections`() {
        val pipeline = VisionPipeline(
            listOf(
                fakeEngine(TPV_BLOB_ENGINE_ID, detections = listOf(detection(TPV_BLOB_ENGINE_ID, 1))),
                fakeEngine("face", detections = listOf(detection("face", 2))),
            ),
            NoopObjectTracker(),
        )
        val result = pipeline.process(frame())
        assertEquals(2, result.detections.size)
        assertEquals(TPV_BLOB_ENGINE_ID, result.primaryDetections.single().engineId)
    }

    @Test
    fun `pipeline skips disabled engines for exclusive recognition modes`() {
        val calls = ArrayList<String>()
        val pipeline = VisionPipeline(
            listOf(
                fakeEngine(TPV_BLOB_ENGINE_ID, calls, enabled = false),
                fakeEngine(FACE_ENGINE_ID, calls, enabled = true),
            ),
            NoopObjectTracker(),
            EventPolicyConfig(primaryEventEngine = FACE_ENGINE_ID, enabledCommitEngines = setOf(FACE_ENGINE_ID)),
        )

        val result = pipeline.process(frame())

        assertEquals(listOf(FACE_ENGINE_ID), calls)
        assertEquals(FACE_ENGINE_ID, result.primaryDetections.single().engineId)
    }

    @Test
    fun `engine exceptions surface as pipeline errors`() {
        val pipeline = VisionPipeline(
            listOf(object : VisionEngine {
                override val metadata = VisionEngineMetadata(
                    "bad", "test", "1", null, null, setOf(VisionInputFormat.Y640), true)
                override fun process(frame: VisionFrame): EngineFrameResult {
                    throw IllegalStateException("boom")
                }
            }),
            NoopObjectTracker(),
            EventPolicyConfig(primaryEventEngine = "bad", enabledCommitEngines = setOf("bad")),
        )
        val result = pipeline.process(frame())
        assertEquals(emptyList<VisionDetection>(), result.detections)
        assertEquals("bad", result.errors.single().engineId)
        assertTrue(result.errors.single().throwable.message!!.contains("boom"))
    }

    @Test
    fun `close closes closeable enabled engines`() {
        var closed = false
        val pipeline = VisionPipeline(
            listOf(object : VisionEngine, AutoCloseable {
                override val metadata = VisionEngineMetadata(
                    "closeable", "test", "1", null, null, setOf(VisionInputFormat.Y640), true)
                override fun process(frame: VisionFrame): EngineFrameResult =
                    EngineFrameResult("closeable", emptyList())
                override fun close() { closed = true }
            }),
            NoopObjectTracker(),
            EventPolicyConfig(primaryEventEngine = "closeable", enabledCommitEngines = setOf("closeable")),
        )

        pipeline.close()

        assertTrue(closed)
    }

    private fun fakeEngine(
        id: String,
        calls: MutableList<String> = mutableListOf(),
        detections: List<VisionDetection> = listOf(detection(id, 1)),
        nativeTracks: List<TrackedDetection> = emptyList(),
        enabled: Boolean = true,
    ) = object : VisionEngine {
        override val metadata = VisionEngineMetadata(
            id, "fake", "1", null, null, setOf(VisionInputFormat.Y640), enabled)
        override fun process(frame: VisionFrame): EngineFrameResult {
            calls.add(id)
            return EngineFrameResult(id, detections, nativeTracks = nativeTracks)
        }
    }

    private fun frame() = VisionFrame(
        frameIdxInRun = 1,
        tCameraArriveNs = 1,
        nativeW = 640,
        nativeH = 480,
        crop = YuvAdapter.CropRect(0, 0, 640, 480),
        y640 = ByteArray(640 * 480),
        rotationDegrees = 0,
        buffers = FrameScopedBufferProvider(nv21Supplier = { ByteArray(10) }),
    )

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
