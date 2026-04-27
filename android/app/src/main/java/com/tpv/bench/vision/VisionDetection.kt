package com.tpv.bench.vision

data class VisionDetection(
    val engineId: String,
    val detectionId: Long,
    val frameIdxInRun: Long,
    val classId: Int,
    val className: String,
    val score: Float,
    val bbox640: RectI,
    val mask: ByteArray? = null,
    val landmarks640: List<PointI> = emptyList(),
    val rawStatus: Int = 0,
    val attributes: Map<String, String> = emptyMap(),
) {
    override fun equals(other: Any?): Boolean {
        return other is VisionDetection &&
            engineId == other.engineId &&
            detectionId == other.detectionId &&
            frameIdxInRun == other.frameIdxInRun &&
            classId == other.classId &&
            className == other.className &&
            score == other.score &&
            bbox640 == other.bbox640 &&
            mask.contentEqualsNullable(other.mask) &&
            landmarks640 == other.landmarks640 &&
            rawStatus == other.rawStatus &&
            attributes == other.attributes
    }

    override fun hashCode(): Int {
        var result = engineId.hashCode()
        result = 31 * result + detectionId.hashCode()
        result = 31 * result + frameIdxInRun.hashCode()
        result = 31 * result + classId
        result = 31 * result + className.hashCode()
        result = 31 * result + score.hashCode()
        result = 31 * result + bbox640.hashCode()
        result = 31 * result + (mask?.contentHashCode() ?: 0)
        result = 31 * result + landmarks640.hashCode()
        result = 31 * result + rawStatus
        result = 31 * result + attributes.hashCode()
        return result
    }
}

enum class TrackState { TENTATIVE, CONFIRMED, LOST }

data class TrackedDetection(
    val detection: VisionDetection,
    val trackId: Long,
    val state: TrackState,
    val ageFrames: Int,
    val hits: Int,
    val misses: Int,
)

private fun ByteArray?.contentEqualsNullable(other: ByteArray?): Boolean =
    when {
        this === other -> true
        this == null || other == null -> false
        else -> contentEquals(other)
    }

