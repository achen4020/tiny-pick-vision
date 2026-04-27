package com.tpv.bench.vision

data class VisionEngineMetadata(
    val id: String,
    val type: String,
    val version: String,
    val modelSha256: String?,
    val providerVersion: String?,
    val requiredInputs: Set<VisionInputFormat>,
    val enabled: Boolean,
)

data class EngineFrameResult(
    val engineId: String,
    val detections: List<VisionDetection>,
    val raw: Any? = null,
    val tReturnNs: Long = 0L,
    val nativeTracks: List<TrackedDetection> = emptyList(),
)

inline fun <reified T> EngineFrameResult.requireRaw(): T =
    raw as? T ?: error("Engine $engineId raw result is ${raw?.javaClass?.name ?: "null"}, expected ${T::class.java.name}")

interface VisionEngine {
    val metadata: VisionEngineMetadata
    fun process(frame: VisionFrame): EngineFrameResult
}
