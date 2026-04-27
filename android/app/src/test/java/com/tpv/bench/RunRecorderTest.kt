package com.tpv.bench

import com.tpv.bench.vision.RectI
import com.tpv.bench.vision.FACE_ENGINE_ID
import com.tpv.bench.vision.TPV_BLOB_ENGINE_ID
import com.tpv.bench.vision.TrackState
import com.tpv.bench.vision.TrackedDetection
import com.tpv.bench.vision.VisionDetection
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipFile

class RunRecorderTest {

    @get:Rule val tmp = TemporaryFolder()

    private fun meta() = MetaInfo(
        runId = "run_test", deviceModel = "Pixel-test", androidLevel = 34,
        abi = "arm64-v8a", cpuMaxFreqKhz = 2_800_000,
        soSha256 = "a".repeat(64), modelDataSha256 = "b".repeat(64),
        nClasses = 5, binThreshold = 137,
        darkObjectMode = true,
        roiX = 0, roiY = 0, roiW = 640, roiH = 480,
        nStable = 3, kEmpty = 5, mDriftPx = 30,
        requestedW = 640, requestedH = 480,
        nativeW = 1280, nativeH = 720,
        cameraLensFacing = "back",
        cropX = 160, cropY = 0, cropW = 960, cropH = 720,
        downsampleRatioX = 1.5, downsampleRatioY = 1.5,
        vision = visionConfig(),
    )

    private fun visionConfig(
        binThreshold: Int = 137,
        darkObjectMode: Boolean = true,
        trackerEnabled: Boolean = true,
        includeFace: Boolean = false,
        faceEnabled: Boolean = includeFace,
        tpvEnabled: Boolean = true,
        primaryEngine: String = TPV_BLOB_ENGINE_ID,
    ) = VisionRunConfig(
        schemaVersion = 1,
        engines = listOfNotNull(
            VisionEngineRunConfig(
                id = TPV_BLOB_ENGINE_ID,
                type = "native_c",
                version = "v2",
                modelSha256 = "b".repeat(64),
                providerVersion = null,
                requiredInputs = listOf("Y640"),
                params = TpvBlobRunParams(
                    binThreshold = binThreshold,
                    darkObjectMode = darkObjectMode,
                    roiX = 0,
                    roiY = 0,
                    roiW = 640,
                    roiH = 480,
                ),
                enabled = tpvEnabled,
            ),
            if (includeFace) VisionEngineRunConfig(
                id = FACE_ENGINE_ID,
                type = "mediapipe_tasks",
                version = "face_detector",
                modelSha256 = "c".repeat(64),
                providerVersion = "mediapipe-tasks-test",
                requiredInputs = listOf("ARGB8888"),
                params = FaceRunParams(
                    modelAssetPath = "blaze_face_short_range.tflite",
                    minDetectionConfidence = 0.5f,
                    minSuppressionThreshold = 0.3f,
                ),
                enabled = faceEnabled,
            ) else null,
        ),
        eventPolicy = VisionEventPolicyRunConfig(
            mode = "PRIMARY_ONLY",
            primaryEventEngine = primaryEngine,
            enabledCommitEngines = listOf(primaryEngine),
        ),
        tracker = VisionTrackerRunConfig(
            type = if (trackerEnabled) "sort_like" else "noop",
            enabled = trackerEnabled,
            minHits = 2,
            maxAge = 10,
            iouThreshold = 0.25f,
            centerDistancePx = 80f,
        ),
    )

    private fun dummyDebug(classId: Int) = TpvDetectionDebugV2(
        det = TpvDetection(0, classId, 320, 240, -450, 200),
        features = TpvFeatures(IntArray(7) { 0x0001_0000 * it }, 0x1a2b4, 0xdd74, 0),
        distancesSq = intArrayOf(12345678, 987654, 456789, 111111, 999999),
        bbox = TpvBbox(265, 180, 110, 139),
        areaPx = 15290,
        grid8x8 = 150,
        bin = ByteArray(640 * 480 / 8),
        allBlobsMask = ByteArray(640 * 480 / 8),
        mask = ByteArray(640 * 480 / 8).also { it[0] = 0x0F },  // 4 bits set for assertions
    )

    @Test
    fun `start writes meta json with all spec fields`() {
        val rec = RunRecorder(tmp.root, meta())
        rec.start()
        val metaFile = File(tmp.root, "meta.json")
        assertTrue(metaFile.exists())
        val j = JSONObject(metaFile.readText())
        assertEquals("run_test", j.getString("run_id"))
        assertEquals("Pixel-test", j.getJSONObject("device").getString("model"))
        assertEquals(5, j.getJSONObject("tpv").getInt("n_classes"))
        assertEquals(137, j.getJSONObject("tpv").getInt("bin_threshold"))
        assertEquals(3, j.getJSONObject("trigger").getInt("n_stable"))
        val crop = j.getJSONObject("camera").getJSONObject("crop")
        assertEquals("back", j.getJSONObject("camera").getString("lens_facing"))
        assertEquals(160, crop.getInt("x"))
        assertEquals(960, crop.getInt("w"))
        assertEquals("v2", j.getString("ui_version"))
        assertEquals(true, j.getJSONObject("tpv").getBoolean("dark_object_mode"))
        val roi = j.getJSONObject("tpv").getJSONObject("roi")
        assertEquals(0, roi.getInt("x"))
        assertEquals(640, roi.getInt("w"))
        val vision = j.getJSONObject("vision")
        assertEquals(1, vision.getInt("schema_version"))
        val engine = vision.getJSONArray("engines").getJSONObject(0)
        assertEquals(TPV_BLOB_ENGINE_ID, engine.getString("id"))
        assertEquals("Y640", engine.getJSONArray("required_inputs").getString(0))
        assertEquals(137, engine.getJSONObject("params").getInt("bin_threshold"))
        assertEquals(
            j.getJSONObject("tpv").getInt("bin_threshold"),
            engine.getJSONObject("params").getInt("bin_threshold"),
        )
    }

    @Test
    fun `meta json writes face engine config without privacy exports`() {
        val rec = RunRecorder(
            tmp.root,
            meta().copy(vision = visionConfig(
                includeFace = true,
                tpvEnabled = false,
                primaryEngine = FACE_ENGINE_ID,
            )),
        )
        rec.start()

        val metaJson = JSONObject(File(tmp.root, "meta.json").readText())
        assertEquals("v3", metaJson.getString("ui_version"))
        val vision = metaJson.getJSONObject("vision")
        val face = vision.getJSONArray("engines").getJSONObject(1)
        assertEquals(FACE_ENGINE_ID, face.getString("id"))
        assertEquals("mediapipe_tasks", face.getString("type"))
        assertEquals("ARGB8888", face.getJSONArray("required_inputs").getString(0))
        val params = face.getJSONObject("params")
        assertEquals(false, params.getBoolean("persist_landmarks"))
        assertEquals(false, params.getBoolean("persist_crops"))
        assertEquals(false, params.getBoolean("persist_identity"))
    }

    @Test
    fun `meta json records object mode with face engine disabled`() {
        val rec = RunRecorder(
            tmp.root,
            meta().copy(vision = visionConfig(
                includeFace = true,
                faceEnabled = false,
                tpvEnabled = true,
                primaryEngine = TPV_BLOB_ENGINE_ID,
            )),
        )
        rec.start()

        val metaJson = JSONObject(File(tmp.root, "meta.json").readText())
        val vision = metaJson.getJSONObject("vision")
        val engines = vision.getJSONArray("engines")
        val objectEngine = engines.getJSONObject(0)
        val faceEngine = engines.getJSONObject(1)
        assertEquals(TPV_BLOB_ENGINE_ID, objectEngine.getString("id"))
        assertEquals(true, objectEngine.getBoolean("enabled"))
        assertEquals(FACE_ENGINE_ID, faceEngine.getString("id"))
        assertEquals(false, faceEngine.getBoolean("enabled"))
        assertEquals(TPV_BLOB_ENGINE_ID, vision.getJSONObject("event_policy").getString("primary_event_engine"))
    }

    @Test
    fun `meta json records face mode with object engine disabled`() {
        val rec = RunRecorder(
            tmp.root,
            meta().copy(vision = visionConfig(
                includeFace = true,
                tpvEnabled = false,
                primaryEngine = FACE_ENGINE_ID,
            )),
        )
        rec.start()

        val metaJson = JSONObject(File(tmp.root, "meta.json").readText())
        val vision = metaJson.getJSONObject("vision")
        val engines = vision.getJSONArray("engines")
        val objectEngine = engines.getJSONObject(0)
        val faceEngine = engines.getJSONObject(1)
        assertEquals(TPV_BLOB_ENGINE_ID, objectEngine.getString("id"))
        assertEquals(false, objectEngine.getBoolean("enabled"))
        assertEquals(FACE_ENGINE_ID, faceEngine.getString("id"))
        assertEquals(true, faceEngine.getBoolean("enabled"))
        assertEquals(FACE_ENGINE_ID, vision.getJSONObject("event_policy").getString("primary_event_engine"))
    }

    @Test
    fun `recordEvent writes y file, jpg file, mask file, and jsonl line`() {
        val rec = RunRecorder(tmp.root, meta())
        rec.start()
        val rawY = ByteArray(640 * 480) { (it and 0xFF).toByte() }
        val jpg = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte(), 0xD9.toByte())
        val mask = ByteArray(640 * 480 / 8).also { it[100] = 0x5A; it[200] = 0x7F }
        val ev = CommittedEvent(
            eventIdx = 1,
            triggerFrameIdx = 42,
            triggerFrameDebug = dummyDebug(254),
            eventClassId = 2,
            classIdHistogram = mapOf(2 to 2, 254 to 1),
            flicker = true
        )
        rec.recordEvent(ev, triggerTsMs = 1_745_394_128_012L,
            rawY = rawY, overlayJpeg = jpg, mask = mask)
        rec.close()

        val yFile = File(tmp.root, "000001.y")
        assertEquals(640 * 480L, yFile.length())
        assertArrayEquals(rawY, yFile.readBytes())

        val jpgFile = File(tmp.root, "000001.jpg")
        assertArrayEquals(jpg, jpgFile.readBytes())

        val maskFile = File(tmp.root, "000001.mask")
        assertEquals((640 * 480 / 8).toLong(), maskFile.length())
        assertArrayEquals(mask, maskFile.readBytes())

        val line = File(tmp.root, "log.jsonl").readLines().single()
        val j = JSONObject(line)
        assertEquals(1, j.getInt("event_idx"))
        assertEquals(42, j.getInt("frame_idx_in_run"))
        assertEquals(2, j.getInt("event_class_id"))
        assertEquals(true, j.getBoolean("flicker"))
        val det = j.getJSONObject("detection")
        assertEquals(254, det.getInt("class_id"))
        assertEquals(15290, det.getInt("area_px"))
        assertEquals(150, det.getInt("grid_8x8"))
        val bbox = det.getJSONObject("bbox")
        assertEquals(265, bbox.getInt("x"))
        assertEquals(180, bbox.getInt("y"))
        assertEquals(110, bbox.getInt("w"))
        assertEquals(139, bbox.getInt("h"))
        val artifacts = j.getJSONObject("artifacts")
        assertEquals("000001.y", artifacts.getString("raw_y"))
        assertEquals("000001.jpg", artifacts.getString("overlay"))
        assertEquals("000001.mask", artifacts.getString("mask"))
        val hist = j.getJSONObject("class_id_histogram")
        assertEquals(2, hist.getInt("2"))
        assertEquals(1, hist.getInt("254"))
        val dsq = j.getJSONArray("distances_sq")
        assertEquals(5, dsq.length())
        assertEquals(111111, dsq.getInt(3))
        assertEquals(0, j.getJSONArray("tracks").length())
    }

    @Test
    fun `recordEvent writes tracks array`() {
        val rec = RunRecorder(tmp.root, meta())
        rec.start()
        val track = TrackedDetection(
            detection = VisionDetection(
                engineId = TPV_BLOB_ENGINE_ID,
                detectionId = 99,
                frameIdxInRun = 42,
                classId = 0xFF,
                className = "tpv_rejected",
                score = 0f,
                bbox640 = RectI(254, 157, 268, 153),
            ),
            trackId = 3,
            state = TrackState.CONFIRMED,
            ageFrames = 2,
            hits = 2,
            misses = 0,
        )
        rec.recordEvent(
            CommittedEvent(1, 5, dummyDebug(0xFF), 0xFF, mapOf(0xFF to 1), false),
            1L,
            rawY = ByteArray(640 * 480),
            overlayJpeg = ByteArray(4),
            mask = ByteArray(640 * 480 / 8),
            tracks = listOf(track),
        )
        rec.close()

        val tracks = JSONObject(File(tmp.root, "log.jsonl").readLines().single())
            .getJSONArray("tracks")
        val j = tracks.getJSONObject(0)
        assertEquals(3, j.getInt("track_id"))
        assertEquals(TPV_BLOB_ENGINE_ID, j.getString("engine"))
        assertEquals("tpv_rejected", j.getString("class_name"))
        assertEquals("confirmed", j.getString("state"))
        assertEquals(254, j.getJSONObject("bbox").getInt("x"))
    }

    @Test
    fun `timing bin has correct header and record layout`() {
        val rec = RunRecorder(tmp.root, meta())
        rec.start()
        rec.recordFrameTiming(FrameTiming(
            frameIdxInRun = 1, tpvStatus = 0, tpvClassId = 2,
            tCameraArriveNs = 1_000_000, tJniEnterNs = 1_050_000,
            tTpvEnterNs = 1_100_000, tTpvExitNs = 1_500_000,
            tJniReturnNs = 1_520_000))
        rec.close()

        val bin = File(tmp.root, "timing.bin").readBytes()
        val bb = ByteBuffer.wrap(bin).order(ByteOrder.LITTLE_ENDIAN)
        // Header
        assertEquals(0x54, bin[0].toInt())  // 'T'
        assertEquals(0x54, bin[1].toInt())  // 'T'
        assertEquals(0x4D, bin[2].toInt())  // 'M'
        assertEquals(0x4C, bin[3].toInt())  // 'L'
        assertEquals(1, bb.getShort(4).toInt())   // version
        assertEquals(48, bb.getShort(6).toInt())  // record_size
        // First record at offset 32
        assertEquals(1, bb.getInt(32))            // frame_idx
        assertEquals(0, bb.getInt(36))            // tpv_status
        assertEquals(2, bb.getInt(40))            // tpv_class_id
        assertEquals(1_000_000L, bb.getLong(44))  // t_camera_arrive_ns
    }

    @Test
    fun `finalizeMeta appends runtime section with skipped and total counts`() {
        val rec = RunRecorder(tmp.root, meta())
        rec.start()
        rec.finalizeMeta(skippedFrames = 17, totalFrames = 1234)
        rec.close()
        val j = JSONObject(File(tmp.root, "meta.json").readText())
        val runtime = j.getJSONObject("runtime")
        assertEquals(17, runtime.getInt("skipped_frames"))
        assertEquals(1234, runtime.getInt("total_frames"))
        // All the original fields are still there.
        assertEquals("run_test", j.getString("run_id"))
        assertEquals(5, j.getJSONObject("tpv").getInt("n_classes"))
    }

    @Test
    fun `stopAndZip produces a zip containing all run files`() {
        val rec = RunRecorder(tmp.root, meta())
        rec.start()
        val rawY = ByteArray(640 * 480)
        val jpg = ByteArray(10)
        rec.recordEvent(
            CommittedEvent(1, 5, dummyDebug(0), 0, mapOf(0 to 3), false),
            1L, rawY, jpg, mask = ByteArray(640 * 480 / 8)
        )
        rec.recordFrameTiming(FrameTiming(1, 0, 0, 1, 2, 3, 4, 5))
        rec.close()
        val zip = rec.stopAndZip()
        assertTrue(zip.exists())
        assertTrue(zip.name.endsWith(".zip"))

        val entries = ZipFile(zip).use { zf ->
            zf.entries().toList().map { it.name }.toSet()
        }
        assertTrue(entries.contains("meta.json"))
        assertTrue(entries.contains("log.jsonl"))
        assertTrue(entries.contains("timing.bin"))
        assertTrue(entries.contains("000001.y"))
        assertTrue(entries.contains("000001.jpg"))
        assertTrue(entries.contains("000001.mask"))
    }
}
