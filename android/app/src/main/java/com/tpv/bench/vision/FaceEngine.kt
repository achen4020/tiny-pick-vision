package com.tpv.bench.vision

import android.content.Context
import android.graphics.Bitmap
import android.graphics.RectF
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.tasks.components.containers.Detection
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.facedetector.FaceDetector
import com.google.mediapipe.tasks.vision.facedetector.FaceDetectorResult
import com.tpv.bench.YuvAdapter
import kotlin.math.max
import kotlin.math.roundToInt

const val FACE_ENGINE_ID = "face"
const val FACE_ENGINE_MODEL_ASSET = "blaze_face_short_range.tflite"
const val MEDIAPIPE_TASKS_VISION_VERSION = "0.10.33"

internal class DetectionCadence(targetFps: Int) {
    private val intervalNs: Long
    private var lastDetectionNs: Long? = null

    init {
        require(targetFps > 0) { "targetFps must be positive" }
        intervalNs = 1_000_000_000L / targetFps
    }

    fun shouldDetect(timestampNs: Long): Boolean {
        val last = lastDetectionNs
        if (last == null || timestampNs - last >= intervalNs) {
            lastDetectionNs = timestampNs
            return true
        }
        return false
    }
}

internal class DetectionCarryState(targetFps: Int) {
    private val cadence = DetectionCadence(targetFps)
    private var detections: List<VisionDetection> = emptyList()

    fun shouldDetect(timestampNs: Long): Boolean = cadence.shouldDetect(timestampNs)

    fun onSuccess(current: List<VisionDetection>) {
        detections = current
    }

    fun onFailure() {
        detections = emptyList()
    }

    fun carryToFrame(frameIdxInRun: Long): List<VisionDetection> =
        detections.mapIndexed { index, detection ->
            detection.copy(
                detectionId = frameIdxInRun * 10_000L + index,
                frameIdxInRun = frameIdxInRun,
            )
        }

    fun clear() {
        detections = emptyList()
    }
}

data class FaceEngineConfig(
    val enabled: Boolean,
    val modelAssetPath: String = FACE_ENGINE_MODEL_ASSET,
    val modelSha256: String?,
    val minDetectionConfidence: Float = 0.5f,
    val minSuppressionThreshold: Float = 0.3f,
)

class FaceEngine(
    context: Context,
    private val config: FaceEngineConfig,
) : VisionEngine, AutoCloseable {
    private val detector = FaceDetector.createFromOptions(
        context,
        FaceDetector.FaceDetectorOptions.builder()
            .setBaseOptions(
                BaseOptions.builder()
                    .setModelAssetPath(config.modelAssetPath)
                    .build()
            )
            .setRunningMode(RunningMode.VIDEO)
            .setMinDetectionConfidence(config.minDetectionConfidence)
            .setMinSuppressionThreshold(config.minSuppressionThreshold)
            .build()
    )
    private var bitmap: Bitmap? = null
    private var lastTimestampMs = Long.MIN_VALUE
    private val detectionState = DetectionCarryState(targetFps = 12)
    private var lastRawResult: FaceDetectorResult? = null

    override val metadata = VisionEngineMetadata(
        id = FACE_ENGINE_ID,
        type = "mediapipe_tasks",
        version = "face_detector",
        modelSha256 = config.modelSha256,
        providerVersion = "mediapipe-tasks-$MEDIAPIPE_TASKS_VISION_VERSION",
        requiredInputs = setOf(VisionInputFormat.ARGB8888),
        enabled = config.enabled,
    )

    override fun process(frame: VisionFrame): EngineFrameResult {
        if (!detectionState.shouldDetect(frame.tCameraArriveNs)) {
            return EngineFrameResult(
                FACE_ENGINE_ID,
                detectionState.carryToFrame(frame.frameIdxInRun),
                lastRawResult,
                System.nanoTime(),
            )
        }
        val argb = frame.buffers.argb8888()
        val bitmap = reusableBitmap(frame.nativeW, frame.nativeH)
        bitmap.setPixels(argb, 0, frame.nativeW, 0, 0, frame.nativeW, frame.nativeH)

        val timestampMs = nextTimestampMs(frame.tCameraArriveNs / 1_000_000L)
        return try {
            val result = detector.detectForVideo(BitmapImageBuilder(bitmap).build(), timestampMs)
            val detections = result.toVisionDetections(frame)
            detectionState.onSuccess(detections)
            lastRawResult = result
            EngineFrameResult(FACE_ENGINE_ID, detections, result, System.nanoTime())
        } catch (t: Throwable) {
            detectionState.onFailure()
            lastRawResult = null
            throw t
        }
    }

    override fun close() {
        detector.close()
        bitmap = null
        detectionState.clear()
        lastRawResult = null
    }

    private fun reusableBitmap(width: Int, height: Int): Bitmap {
        val current = bitmap
        if (current != null && current.width == width && current.height == height) return current
        return Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).also { bitmap = it }
    }

    private fun nextTimestampMs(candidate: Long): Long {
        val next = max(candidate, lastTimestampMs + 1)
        lastTimestampMs = next
        return next
    }

    private fun FaceDetectorResult.toVisionDetections(frame: VisionFrame): List<VisionDetection> =
        detections().mapIndexedNotNull { index, detection ->
            val bbox640 = nativeRectTo640(detection.boundingBox(), frame.crop)
            if (bbox640.area == 0) return@mapIndexedNotNull null
            VisionDetection(
                engineId = FACE_ENGINE_ID,
                detectionId = frame.frameIdxInRun * 10_000L + index,
                frameIdxInRun = frame.frameIdxInRun,
                classId = 0,
                className = "face",
                score = detection.confidence(),
                bbox640 = bbox640,
                landmarks640 = detection.landmarks640(frame.crop, frame.nativeW, frame.nativeH),
            )
        }

    private fun Detection.confidence(): Float {
        val categories = categories()
        return if (categories.isEmpty()) 1f else categories.maxOf { it.score() }
    }

    private fun Detection.landmarks640(
        crop: YuvAdapter.CropRect,
        nativeW: Int,
        nativeH: Int,
    ): List<PointI> {
        val keypoints = keypoints()
        if (!keypoints.isPresent) return emptyList()
        return keypoints.get().map { keypoint ->
            nativePointTo640(
                nativeX = (keypoint.x() * nativeW).roundToInt(),
                nativeY = (keypoint.y() * nativeH).roundToInt(),
                crop = crop,
            )
        }
    }

    companion object {
        fun nativeRectTo640(rect: RectF, crop: YuvAdapter.CropRect): RectI {
            return nativeRectTo640(rect.left, rect.top, rect.right, rect.bottom, crop)
        }

        fun nativeRectTo640(
            left: Float,
            top: Float,
            right: Float,
            bottom: Float,
            crop: YuvAdapter.CropRect,
        ): RectI {
            val x0 = nativeXTo640(left.roundToInt(), crop)
            val y0 = nativeYTo640(top.roundToInt(), crop)
            val x1 = nativeXTo640(right.roundToInt(), crop)
            val y1 = nativeYTo640(bottom.roundToInt(), crop)
            return RectI(x0, y0, x1 - x0, y1 - y0).clampTo(RectI(0, 0, 640, 480))
        }

        fun nativePointTo640(nativeX: Int, nativeY: Int, crop: YuvAdapter.CropRect): PointI =
            PointI(
                nativeXTo640(nativeX, crop).coerceIn(0, 639),
                nativeYTo640(nativeY, crop).coerceIn(0, 479),
            )

        private fun nativeXTo640(nativeX: Int, crop: YuvAdapter.CropRect): Int =
            (((nativeX - crop.x).toDouble() * 640.0) / crop.w).roundToInt()

        private fun nativeYTo640(nativeY: Int, crop: YuvAdapter.CropRect): Int =
            (((nativeY - crop.y).toDouble() * 480.0) / crop.h).roundToInt()
    }
}
