package com.tpv.bench

import org.json.JSONArray
import org.json.JSONObject
import com.tpv.bench.vision.TPV_BLOB_ENGINE_ID
import com.tpv.bench.vision.TrackState
import com.tpv.bench.vision.TrackedDetection
import java.io.BufferedWriter
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

data class FrameTiming(
    val frameIdxInRun: Long,
    val tpvStatus: Int,
    val tpvClassId: Int,
    val tCameraArriveNs: Long,
    val tJniEnterNs: Long,
    val tTpvEnterNs: Long,
    val tTpvExitNs: Long,
    val tJniReturnNs: Long,
)

interface VisionEngineRunParams {
    fun toJson(): JSONObject
}

data class TpvBlobRunParams(
    val binThreshold: Int,
    val darkObjectMode: Boolean,
    val roiX: Int,
    val roiY: Int,
    val roiW: Int,
    val roiH: Int,
) : VisionEngineRunParams {
    override fun toJson(): JSONObject =
        JSONObject()
            .put("bin_threshold", binThreshold)
            .put("dark_object_mode", darkObjectMode)
            .put("roi", JSONObject()
                .put("x", roiX).put("y", roiY)
                .put("w", roiW).put("h", roiH))
}

data class FaceRunParams(
    val modelAssetPath: String,
    val minDetectionConfidence: Float,
    val minSuppressionThreshold: Float,
    val runningMode: String = "VIDEO",
    val persistLandmarks: Boolean = false,
    val persistCrops: Boolean = false,
    val persistIdentity: Boolean = false,
) : VisionEngineRunParams {
    override fun toJson(): JSONObject =
        JSONObject()
            .put("model_asset_path", modelAssetPath)
            .put("min_detection_confidence", minDetectionConfidence.toDouble())
            .put("min_suppression_threshold", minSuppressionThreshold.toDouble())
            .put("running_mode", runningMode)
            .put("persist_landmarks", persistLandmarks)
            .put("persist_crops", persistCrops)
            .put("persist_identity", persistIdentity)
}

data class VisionEngineRunConfig(
    val id: String,
    val type: String,
    val version: String,
    val modelSha256: String?,
    val providerVersion: String?,
    val requiredInputs: List<String>,
    val params: VisionEngineRunParams?,
    val enabled: Boolean,
)

data class VisionEventPolicyRunConfig(
    val mode: String,
    val primaryEventEngine: String,
    val enabledCommitEngines: List<String>,
)

data class VisionTrackerRunConfig(
    val type: String,
    val enabled: Boolean,
    val minHits: Int,
    val maxAge: Int,
    val iouThreshold: Float,
    val centerDistancePx: Float,
)

data class VisionRunConfig(
    val schemaVersion: Int,
    val engines: List<VisionEngineRunConfig>,
    val eventPolicy: VisionEventPolicyRunConfig,
    val tracker: VisionTrackerRunConfig,
)

data class MetaInfo(
    val runId: String,
    val deviceModel: String, val androidLevel: Int, val abi: String, val cpuMaxFreqKhz: Long,
    val soSha256: String, val modelDataSha256: String,
    val nClasses: Int, val binThreshold: Int,
    // v2 additions
    val darkObjectMode: Boolean,
    val roiX: Int, val roiY: Int, val roiW: Int, val roiH: Int,
    // unchanged tail
    val nStable: Int, val kEmpty: Int, val mDriftPx: Int,
    val requestedW: Int, val requestedH: Int,
    val nativeW: Int, val nativeH: Int,
    val cameraLensFacing: String,
    val cropX: Int, val cropY: Int, val cropW: Int, val cropH: Int,
    val downsampleRatioX: Double, val downsampleRatioY: Double,
    val vision: VisionRunConfig,
)

class RunRecorder(
    private val runDir: File,
    private val meta: MetaInfo,
) {
    private var logStream: FileOutputStream? = null
    private var logWriter: BufferedWriter? = null
    private var timingFile: FileOutputStream? = null
    private val timingBatch = ByteBuffer.allocate(48 * 100).order(ByteOrder.LITTLE_ENDIAN)

    fun start() {
        runDir.mkdirs()
        File(runDir, "meta.json").writeText(metaToJson().toString(2))

        logStream = FileOutputStream(File(runDir, "log.jsonl"))
        logWriter = logStream!!.bufferedWriter()

        val tf = FileOutputStream(File(runDir, "timing.bin"))
        timingFile = tf
        val header = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN)
        header.put('T'.code.toByte()); header.put('T'.code.toByte())
        header.put('M'.code.toByte()); header.put('L'.code.toByte())
        header.putShort(1)             // version
        header.putShort(48)            // record_size
        header.putLong(System.nanoTime())  // run_start_ns
        // last 16 bytes remain zero
        tf.write(header.array())
    }

    private fun metaToJson(): JSONObject {
        val tpvEngine = meta.vision.engines.first { it.id == TPV_BLOB_ENGINE_ID }
        val tpvParams = requireNotNull(tpvEngine.params as? TpvBlobRunParams) {
            "vision.engines[id=tpv_blob].params must exist"
        }
        val device = JSONObject()
            .put("model", meta.deviceModel)
            .put("android", meta.androidLevel)
            .put("abi", meta.abi)
            .put("cpu_max_freq_khz", meta.cpuMaxFreqKhz)
        val tpv = JSONObject()
            .put("so_sha256", meta.soSha256)
            .put("model_data_sha256", tpvEngine.modelSha256 ?: meta.modelDataSha256)
            .put("n_classes", meta.nClasses)
            .put("bin_threshold", tpvParams.binThreshold)
            .put("dark_object_mode", tpvParams.darkObjectMode)
            .put("roi", JSONObject()
                .put("x", tpvParams.roiX).put("y", tpvParams.roiY)
                .put("w", tpvParams.roiW).put("h", tpvParams.roiH))
        val trigger = JSONObject()
            .put("n_stable", meta.nStable)
            .put("k_empty", meta.kEmpty)
            .put("m_drift_px", meta.mDriftPx)
        val crop = JSONObject()
            .put("x", meta.cropX).put("y", meta.cropY)
            .put("w", meta.cropW).put("h", meta.cropH)
        val camera = JSONObject()
            .put("requested_w", meta.requestedW).put("requested_h", meta.requestedH)
            .put("native_w", meta.nativeW).put("native_h", meta.nativeH)
            .put("lens_facing", meta.cameraLensFacing)
            .put("crop", crop)
            .put("downsample_ratio_x", meta.downsampleRatioX)
            .put("downsample_ratio_y", meta.downsampleRatioY)
        val uiVersion = if (meta.vision.engines.any { it.id != TPV_BLOB_ENGINE_ID && it.enabled }) {
            "v3"
        } else {
            "v2"
        }
        return JSONObject()
            .put("run_id", meta.runId)
            .put("ui_version", uiVersion)
            .put("device", device)
            .put("tpv", tpv)
            .put("vision", visionToJson(meta.vision))
            .put("trigger", trigger)
            .put("camera", camera)
    }

    private fun visionToJson(vision: VisionRunConfig): JSONObject {
        val engines = JSONArray()
        for (engine in vision.engines) {
            val engineJson = JSONObject()
                .put("id", engine.id)
                .put("type", engine.type)
                .put("version", engine.version)
                .put("model_sha256", engine.modelSha256 ?: JSONObject.NULL)
                .put("provider_version", engine.providerVersion ?: JSONObject.NULL)
                .put("required_inputs", JSONArray(engine.requiredInputs))
                .put("enabled", engine.enabled)
            engineJson.put("params", engine.params?.toJson() ?: JSONObject.NULL)
            engines.put(engineJson)
        }
        val eventPolicy = JSONObject()
            .put("mode", vision.eventPolicy.mode)
            .put("primary_event_engine", vision.eventPolicy.primaryEventEngine)
            .put("enabled_commit_engines", JSONArray(vision.eventPolicy.enabledCommitEngines))
        val tracker = JSONObject()
            .put("type", vision.tracker.type)
            .put("enabled", vision.tracker.enabled)
            .put("min_hits", vision.tracker.minHits)
            .put("max_age", vision.tracker.maxAge)
            .put("iou_threshold", vision.tracker.iouThreshold.toDouble())
            .put("center_distance_px", vision.tracker.centerDistancePx.toDouble())
        return JSONObject()
            .put("schema_version", vision.schemaVersion)
            .put("engines", engines)
            .put("event_policy", eventPolicy)
            .put("tracker", tracker)
    }

    fun recordEvent(
        event: CommittedEvent, triggerTsMs: Long,
        rawY: ByteArray, overlayJpeg: ByteArray,
        mask: ByteArray,            // size must be 640*480/8 = 38400
        tracks: List<TrackedDetection> = emptyList(),
    ) {
        val w = logWriter ?: error("RunRecorder.start() not called")
        val ls = logStream ?: error("RunRecorder.start() not called")
        val name = "%06d".format(event.eventIdx)
        File(runDir, "$name.y").writeBytes(rawY)
        File(runDir, "$name.jpg").writeBytes(overlayJpeg)
        File(runDir, "$name.mask").writeBytes(mask)

        val d = event.triggerFrameDebug
        val det = JSONObject()
            .put("status", d.det.status)
            .put("class_id", d.det.classId)
            .put("x", d.det.x).put("y", d.det.y)
            .put("theta_x10", d.det.thetaX10)
            .put("confidence_q8", d.det.confidenceQ8)
            .put("bbox", JSONObject()
                .put("x", d.bbox.x).put("y", d.bbox.y)
                .put("w", d.bbox.w).put("h", d.bbox.h))
            .put("area_px", d.areaPx)
            .put("grid_8x8", d.grid8x8)

        val huArr = JSONArray()
        for (v in d.features.hu) huArr.put("0x%08x".format(v))
        val feat = JSONObject()
            .put("hu", huArr)
            .put("perim_ratio",  "0x%08x".format(d.features.perimRatio))
            .put("eccentricity", "0x%08x".format(d.features.eccentricity))
            .put("m3_axis_sign", d.features.m3AxisSign)

        val dsq = JSONArray()
        for (v in d.distancesSq) dsq.put(v)

        val hist = JSONObject()
        for ((k, v) in event.classIdHistogram) hist.put(k.toString(), v)

        val artifacts = JSONObject()
            .put("raw_y", "$name.y")
            .put("overlay", "$name.jpg")
            .put("mask", "$name.mask")

        val tracksJson = JSONArray()
        for (track in tracks) {
            val detection = track.detection
            tracksJson.put(JSONObject()
                .put("track_id", track.trackId)
                .put("engine", detection.engineId)
                .put("class_name", detection.className)
                .put("score", detection.score.toDouble())
                .put("bbox", JSONObject()
                    .put("x", detection.bbox640.x).put("y", detection.bbox640.y)
                    .put("w", detection.bbox640.w).put("h", detection.bbox640.h))
                .put("state", track.state.toJsonName()))
        }

        val line = JSONObject()
            .put("event_idx", event.eventIdx)
            .put("trigger_ts_ms", triggerTsMs)
            .put("frame_idx_in_run", event.triggerFrameIdx)
            .put("detection", det)
            .put("event_class_id", event.eventClassId)
            .put("class_id_histogram", hist)
            .put("flicker", event.flicker)
            .put("features", feat)
            .put("distances_sq", dsq)
            .put("tracks", tracksJson)
            .put("artifacts", artifacts)

        w.write(line.toString())
        w.newLine()
        w.flush()                 // push BufferedWriter -> FileOutputStream
        ls.fd.sync()              // durable disk commit; spec §10.4
    }

    private fun TrackState.toJsonName(): String = name.lowercase(Locale.US)

    fun recordFrameTiming(t: FrameTiming) {
        // If batch would overflow, flush first
        if (timingBatch.position() + 48 > timingBatch.capacity()) {
            flushTimingBatch()
        }
        val startPos = timingBatch.position()
        timingBatch.putInt(t.frameIdxInRun.toInt())
        timingBatch.putInt(t.tpvStatus)
        timingBatch.putInt(t.tpvClassId)
        timingBatch.putLong(t.tCameraArriveNs)
        timingBatch.putLong(t.tJniEnterNs)
        timingBatch.putLong(t.tTpvEnterNs)
        timingBatch.putLong(t.tTpvExitNs)
        val delta = t.tJniReturnNs - t.tTpvExitNs
        val delta32 = if (delta < 0 || delta >= 0xFFFFFFFFL) 0xFFFFFFFF.toInt()
                      else delta.toInt()
        timingBatch.putInt(delta32)
        check(timingBatch.position() == startPos + 48) {
            "record size != 48, wrote ${timingBatch.position() - startPos}"
        }
    }

    private fun flushTimingBatch() {
        if (timingBatch.position() == 0) return
        val f = timingFile ?: error("RunRecorder.start() not called")
        f.write(timingBatch.array(), 0, timingBatch.position())
        f.fd.sync()               // durable disk commit; spec §10.4
        timingBatch.clear()
    }

    fun close() {
        flushTimingBatch()
        logWriter?.close() ; logWriter = null
        logStream?.close() ; logStream = null
        timingFile?.close() ; timingFile = null
    }

    /**
     * Rewrites meta.json to include a `runtime` section with post-run
     * statistics (skipped_frames, total_frames). Must be called before
     * stopAndZip() so the zip contains the updated meta.
     *
     * Rationale: meta.json is originally written at start time with
     * settings + device info, but skipped/total counts are only known
     * after the run ends. Instead of writing them to a separate file,
     * we extend the same meta.json so downstream analysis only needs one
     * metadata source per run.
     */
    fun finalizeMeta(skippedFrames: Long, totalFrames: Long) {
        val j = metaToJson().put(
            "runtime",
            JSONObject()
                .put("skipped_frames", skippedFrames)
                .put("total_frames", totalFrames)
        )
        File(runDir, "meta.json").writeText(j.toString(2))
    }

    fun stopAndZip(): File {
        close()
        val zipFile = File(runDir.parentFile, "${runDir.name}.zip")
        ZipOutputStream(FileOutputStream(zipFile)).use { zos ->
            runDir.listFiles()
                ?.filter { !it.name.startsWith(".") }
                ?.sortedBy { it.name }
                ?.forEach { f ->
                    zos.putNextEntry(ZipEntry(f.name))
                    f.inputStream().use { it.copyTo(zos) }
                    zos.closeEntry()
                }
        }
        return zipFile
    }
}
