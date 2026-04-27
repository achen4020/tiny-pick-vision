package com.tpv.bench.vision

enum class CommitMode { PRIMARY_ONLY }

data class EventPolicyConfig(
    val primaryEventEngine: String = TPV_BLOB_ENGINE_ID,
    val enabledCommitEngines: Set<String> = setOf(TPV_BLOB_ENGINE_ID),
    val mode: CommitMode = CommitMode.PRIMARY_ONLY,
)

class EventPolicy(
    private val config: EventPolicyConfig,
    engineIds: Set<String>,
) {
    init {
        require(config.primaryEventEngine.isNotBlank()) { "primaryEventEngine must not be blank" }
        require(config.primaryEventEngine in engineIds) {
            "unknown primaryEventEngine ${config.primaryEventEngine}"
        }
        when (config.mode) {
            CommitMode.PRIMARY_ONLY -> require(config.enabledCommitEngines == setOf(config.primaryEventEngine)) {
                "PRIMARY_ONLY requires enabledCommitEngines == setOf(primaryEventEngine)"
            }
        }
    }

    val primaryEngineId: String get() = config.primaryEventEngine

    fun primaryDetections(detections: List<VisionDetection>): List<VisionDetection> =
        detections.filter { it.engineId == config.primaryEventEngine }

    fun primaryTracks(tracks: List<TrackedDetection>): List<TrackedDetection> =
        tracks.filter { it.detection.engineId == config.primaryEventEngine }
}

const val TPV_BLOB_ENGINE_ID = "tpv_blob"

