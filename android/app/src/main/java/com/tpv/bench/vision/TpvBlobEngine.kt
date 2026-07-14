package com.tpv.bench.vision

import com.tpv.bench.TpvBbox
import com.tpv.bench.TpvDetectionDebugV2
import com.tpv.bench.TpvNative
import com.tpv.bench.TpvVisionDetection
import com.tpv.bench.TpvVisionResult
import com.tpv.bench.YuvAdapter

data class TpvBlobConfig(
    val binThreshold: Int,
    val darkObjectMode: Boolean,
    val roi: YuvAdapter.CropRect,
    val modelDataSha256: String? = null,
    val trackerEnabled: Boolean = true,
    val trackerMinHits: Int = 2,
    val trackerMaxAge: Int = 10,
    val trackerIouThreshold: Float = 0.25f,
    val trackerCenterDistancePx: Float = 80f,
)

interface TpvNativeAdapter {
    fun visionCreateV3(config: TpvBlobConfig): Long

    fun visionCloseV3(handle: Long)

    fun processVisionFrameV3(
        handle: Long,
        y: ByteArray,
        width: Int,
        height: Int,
        outTimingNs: LongArray,
    ): TpvVisionResult

    fun visionLastDebugV2(handle: Long): TpvDetectionDebugV2
}

object ProductionTpvNativeAdapter : TpvNativeAdapter {
    override fun visionCreateV3(config: TpvBlobConfig): Long =
        TpvNative.visionCreateV3(
            TpvBlobEngine.TPV_ENGINE_FLAG_TPV_BLOB,
            TpvBlobEngine.TPV_ENGINE_ID_TPV_BLOB,
            config.binThreshold,
            config.darkObjectMode,
            config.roi.x, config.roi.y, config.roi.w, config.roi.h,
            config.trackerMinHits,
            config.trackerMaxAge,
            config.trackerIouThreshold,
            config.trackerCenterDistancePx,
            0.50f,
            0.50f,
        )

    override fun visionCloseV3(handle: Long) = TpvNative.visionCloseV3(handle)

    override fun processVisionFrameV3(
        handle: Long,
        y: ByteArray,
        width: Int,
        height: Int,
        outTimingNs: LongArray,
    ): TpvVisionResult = TpvNative.processVisionFrameV3(
        handle, y, width, height, outTimingNs,
    )

    override fun visionLastDebugV2(handle: Long): TpvDetectionDebugV2 =
        TpvNative.visionLastDebugV2(handle)
}

class TpvBlobEngine(
    private val config: TpvBlobConfig,
    private val nativeAdapter: TpvNativeAdapter = ProductionTpvNativeAdapter,
) : VisionEngine, AutoCloseable {
    private val visionHandle: Long = nativeAdapter.visionCreateV3(config)

    override val metadata = VisionEngineMetadata(
        id = TPV_BLOB_ENGINE_ID,
        type = "native_c",
        version = "v3",
        modelSha256 = config.modelDataSha256,
        providerVersion = null,
        requiredInputs = setOf(VisionInputFormat.Y640),
        enabled = true,
    )

    override fun process(frame: VisionFrame): EngineFrameResult {
        val vision = nativeAdapter.processVisionFrameV3(
            visionHandle,
            frame.y640, 640, 480,
            frame.tpvTimingNs,
        )
        val raw = nativeAdapter.visionLastDebugV2(visionHandle)
        val tReturnNs = System.nanoTime()
        val detections = if (vision.status == TPV_STATUS_OK) {
            vision.detections.map { it.toVisionDetection(frame.frameIdxInRun, raw) }
        } else {
            emptyList()
        }
        val nativeTracks = if (config.trackerEnabled) {
            detections.zip(vision.detections.asIterable())
                .mapNotNull { (detection, nativeDetection) ->
                    nativeDetection.toTrackedDetection(detection)
                }
        } else {
            emptyList()
        }
        return EngineFrameResult(
            TPV_BLOB_ENGINE_ID,
            detections,
            raw,
            tReturnNs,
            nativeTracks,
        )
    }

    override fun close() {
        nativeAdapter.visionCloseV3(visionHandle)
    }

    private fun TpvVisionDetection.toVisionDetection(
        frameIdxInRun: Long,
        raw: TpvDetectionDebugV2,
    ): VisionDetection {
        val bboxFromVision = if ((flags and TPV_DETECTION_HAS_BBOX) != 0) bbox else raw.bbox
        return VisionDetection(
            engineId = TPV_BLOB_ENGINE_ID,
            detectionId = detectionId,
            frameIdxInRun = frameIdxInRun,
            classId = classId,
            className = tpvClassName(classId),
            score = if (classId in 0..4) confidenceQ8 / 255f else 0f,
            bbox640 = bboxFromVision.toRectI(),
            mask = raw.mask,
            rawStatus = status,
        )
    }

    private fun TpvVisionDetection.toTrackedDetection(
        detection: VisionDetection,
    ): TrackedDetection? {
        if (trackId == 0L) return null
        val state = when {
            (flags and TPV_DETECTION_TRACK_CONFIRMED) != 0 -> TrackState.CONFIRMED
            (flags and TPV_DETECTION_TRACK_LOST) != 0 -> TrackState.LOST
            else -> TrackState.TENTATIVE
        }
        return TrackedDetection(
            detection = detection,
            trackId = trackId,
            state = state,
            ageFrames = trackAgeFrames,
            hits = trackHits,
            misses = trackMisses,
        )
    }

    private fun TpvBbox.toRectI(): RectI = RectI(x, y, w, h)

    companion object {
        const val TPV_ENGINE_ID_TPV_BLOB = 1
        const val TPV_ENGINE_FLAG_TPV_BLOB = 1
        const val TPV_STATUS_OK = 0
        const val TPV_STATUS_EMPTY = 1
        const val TPV_DETECTION_HAS_BBOX = 1 shl 1
        const val TPV_DETECTION_TRACK_TENTATIVE = 1 shl 4
        const val TPV_DETECTION_TRACK_CONFIRMED = 1 shl 5
        const val TPV_DETECTION_TRACK_LOST = 1 shl 6

        fun tpvClassName(classId: Int): String = when (classId) {
            in 0..4 -> "tpv_$classId"
            0xFE -> "tpv_ambiguous"
            0xFF -> "tpv_rejected"
            else -> "tpv_$classId"
        }
    }
}
