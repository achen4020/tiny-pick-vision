package com.tpv.bench

import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedWriter
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
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

data class MetaInfo(
    val runId: String,
    val deviceModel: String, val androidLevel: Int, val abi: String, val cpuMaxFreqKhz: Long,
    val soSha256: String, val modelDataSha256: String,
    val nClasses: Int, val binThreshold: Int,
    val nStable: Int, val kEmpty: Int, val mDriftPx: Int,
    val requestedW: Int, val requestedH: Int,
    val nativeW: Int, val nativeH: Int,
    val cropX: Int, val cropY: Int, val cropW: Int, val cropH: Int,
    val downsampleRatioX: Double, val downsampleRatioY: Double,
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
        val device = JSONObject()
            .put("model", meta.deviceModel)
            .put("android", meta.androidLevel)
            .put("abi", meta.abi)
            .put("cpu_max_freq_khz", meta.cpuMaxFreqKhz)
        val tpv = JSONObject()
            .put("so_sha256", meta.soSha256)
            .put("model_data_sha256", meta.modelDataSha256)
            .put("n_classes", meta.nClasses)
            .put("bin_threshold", meta.binThreshold)
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
            .put("crop", crop)
            .put("downsample_ratio_x", meta.downsampleRatioX)
            .put("downsample_ratio_y", meta.downsampleRatioY)
        return JSONObject()
            .put("run_id", meta.runId)
            .put("device", device)
            .put("tpv", tpv)
            .put("trigger", trigger)
            .put("camera", camera)
    }

    fun recordEvent(
        event: CommittedEvent, triggerTsMs: Long,
        rawY: ByteArray, overlayJpeg: ByteArray,
    ) {
        val w = logWriter ?: error("RunRecorder.start() not called")
        val ls = logStream ?: error("RunRecorder.start() not called")
        val name = "%06d".format(event.eventIdx)
        File(runDir, "$name.y").writeBytes(rawY)
        File(runDir, "$name.jpg").writeBytes(overlayJpeg)

        val d = event.triggerFrameDebug
        val det = JSONObject()
            .put("status", d.det.status)
            .put("class_id", d.det.classId)
            .put("x", d.det.x).put("y", d.det.y)
            .put("theta_x10", d.det.thetaX10)
            .put("confidence_q8", d.det.confidenceQ8)

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
            .put("artifacts", artifacts)

        w.write(line.toString())
        w.newLine()
        w.flush()                 // push BufferedWriter -> FileOutputStream
        ls.fd.sync()              // durable disk commit; spec §10.4
    }

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
