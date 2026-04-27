package com.tpv.bench.vision

data class PipelineError(
    val engineId: String,
    val throwable: Throwable,
)

data class VisionPipelineResult(
    val engineResults: List<EngineFrameResult>,
    val detections: List<VisionDetection>,
    val trackedDetections: List<TrackedDetection>,
    val primaryDetections: List<VisionDetection>,
    val primaryTracks: List<TrackedDetection>,
    val errors: List<PipelineError> = emptyList(),
) {
    fun engineResult(engineId: String): EngineFrameResult? =
        engineResults.firstOrNull { it.engineId == engineId }
}

class VisionPipeline(
    engines: List<VisionEngine>,
    private val tracker: VisionTracker,
    policyConfig: EventPolicyConfig = EventPolicyConfig(),
) : AutoCloseable {
    private val enabledEngines = engines.filter { it.metadata.enabled }.sortedBy { it.metadata.id }
    val engineMetadata: List<VisionEngineMetadata> = enabledEngines.map { it.metadata }
    private val eventPolicy = EventPolicy(policyConfig, engineMetadata.map { it.id }.toSet())

    fun process(frame: VisionFrame): VisionPipelineResult {
        val engineResults = ArrayList<EngineFrameResult>()
        val detections = ArrayList<VisionDetection>()
        val nativeTracks = ArrayList<TrackedDetection>()
        val errors = ArrayList<PipelineError>()

        for (engine in enabledEngines) {
            try {
                val result = engine.process(frame)
                engineResults.add(result)
                detections.addAll(result.detections)
                nativeTracks.addAll(result.nativeTracks)
            } catch (t: Throwable) {
                errors.add(PipelineError(engine.metadata.id, t))
            }
        }

        val nativeTrackedKeys = nativeTracks.map { DetectionKey.from(it.detection) }.toSet()
        val trackerInput = detections.filter { DetectionKey.from(it) !in nativeTrackedKeys }
        val tracks = nativeTracks + tracker.update(trackerInput)
        return VisionPipelineResult(
            engineResults = engineResults,
            detections = detections,
            trackedDetections = tracks,
            primaryDetections = eventPolicy.primaryDetections(detections),
            primaryTracks = eventPolicy.primaryTracks(tracks),
            errors = errors,
        )
    }

    fun reset() {
        tracker.reset()
    }

    override fun close() {
        for (engine in enabledEngines) {
            if (engine is AutoCloseable) engine.close()
        }
    }

    private data class DetectionKey(
        val engineId: String,
        val detectionId: Long,
        val frameIdxInRun: Long,
    ) {
        companion object {
            fun from(detection: VisionDetection) = DetectionKey(
                detection.engineId,
                detection.detectionId,
                detection.frameIdxInRun,
            )
        }
    }
}
