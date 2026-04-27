package com.tpv.bench.vision

data class TrackerConfig(
    val minHits: Int = 2,
    val maxAge: Int = 10,
    val iouThreshold: Float = 0.25f,
    val centerDistancePx: Float = 80f,
) {
    init {
        require(minHits >= 1) { "minHits must be >= 1" }
        require(maxAge >= 0) { "maxAge must be >= 0" }
        require(iouThreshold in 0f..1f) { "iouThreshold must be in 0..1" }
        require(centerDistancePx >= 0f) { "centerDistancePx must be >= 0" }
    }
}

interface VisionTracker {
    fun update(detections: List<VisionDetection>): List<TrackedDetection>
    fun reset()
}

class NoopObjectTracker : VisionTracker {
    override fun update(detections: List<VisionDetection>): List<TrackedDetection> =
        detections.map {
            TrackedDetection(
                detection = it,
                trackId = it.detectionId,
                state = TrackState.CONFIRMED,
                ageFrames = 1,
                hits = 1,
                misses = 0,
            )
        }

    override fun reset() = Unit
}

class MultiObjectTracker(
    private val config: TrackerConfig = TrackerConfig(),
) : VisionTracker {
    private val tracks = ArrayList<Track>()
    private var nextTrackId = 1L

    override fun update(detections: List<VisionDetection>): List<TrackedDetection> {
        val matchedDetections = HashSet<Int>()
        val matchedTracks = HashSet<Long>()

        val groups = detections.indices.groupBy {
            detections[it].engineId to detections[it].className
        }

        for ((key, detectionIndices) in groups) {
            val candidates = tracks.filter { it.engineId to it.className == key && it.misses <= config.maxAge }
            val matches = greedyMatches(candidates, detectionIndices, detections)
            for ((track, detectionIndex) in matches) {
                track.update(detections[detectionIndex], config.minHits)
                matchedTracks.add(track.id)
                matchedDetections.add(detectionIndex)
            }
        }

        for (track in tracks) {
            if (track.id !in matchedTracks) track.markMissed()
        }

        for (index in detections.indices) {
            if (index !in matchedDetections) {
                val detection = detections[index]
                tracks.add(Track(
                    id = nextTrackId++,
                    detection = detection,
                    engineId = detection.engineId,
                    className = detection.className,
                    state = if (config.minHits <= 1) TrackState.CONFIRMED else TrackState.TENTATIVE,
                ))
            }
        }

        tracks.removeAll { it.misses > config.maxAge }
        return tracks.map { it.toTrackedDetection() }
    }

    override fun reset() {
        tracks.clear()
        nextTrackId = 1L
    }

    fun snapshot(includeLost: Boolean = true): List<TrackedDetection> =
        tracks
            .filter { includeLost || it.state != TrackState.LOST }
            .map { it.toTrackedDetection() }

    private fun greedyMatches(
        candidateTracks: List<Track>,
        detectionIndices: List<Int>,
        detections: List<VisionDetection>,
    ): List<Pair<Track, Int>> {
        val edges = ArrayList<MatchEdge>()
        for (track in candidateTracks) {
            for (index in detectionIndices) {
                val detection = detections[index]
                val iou = track.detection.bbox640.iou(detection.bbox640)
                val distance = track.detection.bbox640.centerDistanceTo(detection.bbox640)
                if (iou >= config.iouThreshold || distance <= config.centerDistancePx) {
                    edges.add(MatchEdge(track, index, iou, distance))
                }
            }
        }
        edges.sortWith(
            compareByDescending<MatchEdge> { it.iou }
                .thenBy { it.distance }
                .thenBy { it.track.id }
                .thenBy { it.detectionIndex }
        )

        val usedTracks = HashSet<Long>()
        val usedDetections = HashSet<Int>()
        val matches = ArrayList<Pair<Track, Int>>()
        for (edge in edges) {
            if (edge.track.id in usedTracks || edge.detectionIndex in usedDetections) continue
            usedTracks.add(edge.track.id)
            usedDetections.add(edge.detectionIndex)
            matches.add(edge.track to edge.detectionIndex)
        }
        return matches
    }

    private data class MatchEdge(
        val track: Track,
        val detectionIndex: Int,
        val iou: Float,
        val distance: Float,
    )

    private class Track(
        val id: Long,
        var detection: VisionDetection,
        val engineId: String,
        val className: String,
        var state: TrackState,
        var ageFrames: Int = 1,
        var hits: Int = 1,
        var misses: Int = 0,
    ) {
        fun update(newDetection: VisionDetection, minHits: Int) {
            detection = newDetection
            ageFrames += 1
            hits += 1
            misses = 0
            if (hits >= minHits) state = TrackState.CONFIRMED
        }

        fun markMissed() {
            ageFrames += 1
            misses += 1
            if (misses > 0) state = TrackState.LOST
        }

        fun toTrackedDetection(): TrackedDetection =
            TrackedDetection(detection, id, state, ageFrames, hits, misses)
    }
}

