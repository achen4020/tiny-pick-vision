package com.tpv.bench.vision

import com.tpv.bench.TpvBbox
import com.tpv.bench.TpvDetection
import com.tpv.bench.TpvDetectionDebugV2
import com.tpv.bench.TpvFeatures
import com.tpv.bench.TpvVisionDetection
import com.tpv.bench.TpvVisionResult
import com.tpv.bench.YuvAdapter
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test

class TpvBlobEngineTest {
    @Test
    fun `processes each frame through native vision only once`() {
        val raw = debug(status = TpvBlobEngine.TPV_STATUS_OK, classId = 2)
        var visionCalls = 0
        val adapter = object : TpvNativeAdapter {
            override fun visionCreateV3(config: TpvBlobConfig): Long = 42L
            override fun visionCloseV3(handle: Long) = Unit

            override fun processVisionFrameV3(
                handle: Long,
                y: ByteArray,
                width: Int,
                height: Int,
                outTimingNs: LongArray,
            ): TpvVisionResult {
                visionCalls++
                return vision(classId = 2)
            }

            override fun visionLastDebugV2(handle: Long): TpvDetectionDebugV2 = raw
        }

        TpvBlobEngine(
            TpvBlobConfig(120, true, YuvAdapter.CropRect(0, 0, 640, 480), "sha"),
            adapter,
        ).process(frame())

        assertEquals(1, visionCalls)
    }

    @Test
    fun `empty status returns raw and no detections`() {
        val raw = debug(status = TpvBlobEngine.TPV_STATUS_EMPTY, classId = 0)
        val engine = engineReturning(raw)
        val result = engine.process(frame())
        assertSame(raw, result.raw)
        assertEquals(emptyList<VisionDetection>(), result.detections)
    }

    @Test
    fun `ok status maps bbox mask class and status`() {
        val raw = debug(status = TpvBlobEngine.TPV_STATUS_OK, classId = 2)
        val result = engineReturning(raw, vision(detectionId = 17, classId = 2)).process(frame(frameIdx = 17))
        val detection = result.detections.single()
        assertEquals(TPV_BLOB_ENGINE_ID, detection.engineId)
        assertEquals(17L, detection.detectionId)
        assertEquals(2, detection.classId)
        assertEquals("tpv_2", detection.className)
        assertEquals(200f / 255f, detection.score, 0.0001f)
        assertEquals(RectI(10, 20, 30, 40), detection.bbox640)
        assertArrayEquals(raw.mask, detection.mask)
        assertEquals(TpvBlobEngine.TPV_STATUS_OK, detection.rawStatus)
        assertEquals(7L, result.nativeTracks.single().trackId)
        assertEquals(TrackState.CONFIRMED, result.nativeTracks.single().state)
    }

    @Test
    fun `rejected class maps stable name and zero score`() {
        val raw = debug(status = TpvBlobEngine.TPV_STATUS_OK, classId = 0xFF)
        val detection = engineReturning(raw).process(frame()).detections.single()
        assertEquals("tpv_rejected", detection.className)
        assertEquals(0f, detection.score, 0.0001f)
    }

    @Test
    fun `metadata declares y640 only`() {
        val metadata = engineReturning(debug()).metadata
        assertEquals(TPV_BLOB_ENGINE_ID, metadata.id)
        assertEquals(setOf(VisionInputFormat.Y640), metadata.requiredInputs)
        assertEquals("native_c", metadata.type)
        assertEquals("v3", metadata.version)
        assertEquals("sha", metadata.modelSha256)
    }

    private fun engineReturning(
        raw: TpvDetectionDebugV2,
        vision: TpvVisionResult = vision(status = raw.det.status, classId = raw.det.classId),
    ) = TpvBlobEngine(
        TpvBlobConfig(120, true, YuvAdapter.CropRect(0, 0, 640, 480), "sha"),
        object : TpvNativeAdapter {
            override fun visionCreateV3(config: TpvBlobConfig): Long = 42L

            override fun visionCloseV3(handle: Long) = Unit

            override fun processVisionFrameV3(
                handle: Long,
                y: ByteArray,
                width: Int,
                height: Int,
                outTimingNs: LongArray,
            ): TpvVisionResult {
                assertEquals(42L, handle)
                outTimingNs[0] = 4
                outTimingNs[1] = 5
                outTimingNs[2] = 6
                return vision
            }

            override fun visionLastDebugV2(handle: Long): TpvDetectionDebugV2 = raw
        },
    )

    private fun frame(frameIdx: Long = 1) = VisionFrame(
        frameIdxInRun = frameIdx,
        tCameraArriveNs = 1,
        nativeW = 640,
        nativeH = 480,
        crop = YuvAdapter.CropRect(0, 0, 640, 480),
        y640 = ByteArray(640 * 480),
        rotationDegrees = 0,
        buffers = FrameScopedBufferProvider(nv21Supplier = { ByteArray(640 * 480 * 3 / 2) }),
        tpvTimingNs = LongArray(3),
    )

    private fun debug(
        status: Int = TpvBlobEngine.TPV_STATUS_OK,
        classId: Int = 2,
    ) = TpvDetectionDebugV2(
        det = TpvDetection(status, classId, 320, 240, 0, 200),
        features = TpvFeatures(IntArray(7), 0, 0, 0),
        distancesSq = intArrayOf(1, 2, 3, 4, 5),
        bbox = TpvBbox(10, 20, 30, 40),
        areaPx = 1200,
        grid8x8 = 1,
        bin = ByteArray(640 * 480 / 8),
        allBlobsMask = ByteArray(640 * 480 / 8),
        mask = ByteArray(640 * 480 / 8).also { it[0] = 1 },
    )

    private fun vision(
        status: Int = TpvBlobEngine.TPV_STATUS_OK,
        detectionId: Long = 1,
        classId: Int = 2,
    ) = TpvVisionResult(
        status = status,
        primaryEventEngine = TpvBlobEngine.TPV_ENGINE_ID_TPV_BLOB,
        detections = if (status == TpvBlobEngine.TPV_STATUS_OK) {
            arrayOf(
                TpvVisionDetection(
                    engineId = TpvBlobEngine.TPV_ENGINE_ID_TPV_BLOB,
                    detectionId = detectionId,
                    trackId = 7,
                    flags = TpvBlobEngine.TPV_DETECTION_HAS_BBOX or
                        TpvBlobEngine.TPV_DETECTION_TRACK_CONFIRMED,
                    classId = classId,
                    confidenceQ8 = 200,
                    status = status,
                    centerX = 320,
                    centerY = 240,
                    bbox = TpvBbox(10, 20, 30, 40),
                    thetaX10 = 0,
                    trackAgeFrames = 2,
                    trackHits = 2,
                    trackMisses = 0,
                )
            )
        } else {
            emptyArray()
        },
    )
}
