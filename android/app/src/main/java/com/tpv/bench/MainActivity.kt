package com.tpv.bench

import android.Manifest
import android.app.AlertDialog
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ImageFormat
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.YuvImage
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.text.InputType
import android.util.Log
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import com.tpv.bench.vision.EventPolicyConfig
import com.tpv.bench.vision.CommitMode
import com.tpv.bench.vision.FACE_ENGINE_ID
import com.tpv.bench.vision.FACE_ENGINE_MODEL_ASSET
import com.tpv.bench.vision.FaceEngine
import com.tpv.bench.vision.FaceEngineConfig
import com.tpv.bench.vision.FrameScopedBufferProvider
import com.tpv.bench.vision.MEDIAPIPE_TASKS_VISION_VERSION
import com.tpv.bench.vision.MultiObjectTracker
import com.tpv.bench.vision.NoopObjectTracker
import com.tpv.bench.vision.TPV_BLOB_ENGINE_ID
import com.tpv.bench.vision.TpvBlobConfig
import com.tpv.bench.vision.TpvBlobEngine
import com.tpv.bench.vision.TrackState
import com.tpv.bench.vision.TrackerConfig
import com.tpv.bench.vision.VisionEngine
import com.tpv.bench.vision.VisionFrame
import com.tpv.bench.vision.VisionPipeline
import com.tpv.bench.vision.requireRaw
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Arrays
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference
import kotlin.math.cos
import kotlin.math.sin

class MainActivity : AppCompatActivity() {

    private lateinit var settings: SettingsState
    private lateinit var camera: CameraAdapter
    private lateinit var yuv: YuvAdapter
    private lateinit var preview: PreviewView
    private lateinit var overlay: OverlayView
    private lateinit var hud: TextView
    private lateinit var statusLine: TextView
    private lateinit var diagView: DiagnosticsView
    private lateinit var btnStart: Button
    private lateinit var btnMode: Button
    private lateinit var btnStop: Button
    private lateinit var btnExport: Button
    private lateinit var btnSettings: Button
    private lateinit var btnCamera: Button
    private lateinit var btnDiag: Button
    private lateinit var btnRoi: Button
    private lateinit var btnClear: Button

    private var recorder: RunRecorder? = null
    private var trigger: TriggerMachine? = null
    @Volatile private var visionPipeline: VisionPipeline? = null
    @Volatile private var selectedCameraLens: BenchCameraLens = BenchCameraLens.BACK
    private val running = AtomicBoolean(false)
    private val frameCounter = AtomicInteger(0)
    private val eventCounter = AtomicInteger(0)

    // FPS: timestamps of recent frame arrivals (camera callback ns clock).
    private val fpsWin = ArrayDeque<Long>()
    private val fpsWindowSize = 30

    // Skipped-frame estimation. CameraX's KEEP_ONLY_LATEST doesn't report
    // drops, so we infer from arrival gaps on the consumer side.
    //   - recentGaps: sliding window of inter-frame deltas (last 30 frames)
    //   - On each new frame: if (current_gap) > 1.5 × median(recentGaps),
    //     we consider round(gap / median) - 1 frames to have been dropped.
    // This is a coarse estimate for the HUD + meta.json runtime section;
    // analyze_timing.py does a more precise post-hoc pass on timing.bin.
    private val skippedCount = AtomicLong(0)
    @Volatile private var lastArrivalNs = 0L
    private val recentGaps = ArrayDeque<Long>()

    /** Set by the currently-running start sequence; the first frame calls it to
     *  construct `recorder` with the real crop rectangle, then clears itself. */
    private val pendingInit = AtomicReference<((Int, Int, YuvAdapter.CropRect) -> Unit)?>(null)

    /** Rolling "last committed event" — HUD and OverlayView line 2 read from this,
     *  not from the current frame's detection. */
    private val lastCommittedEvent = AtomicReference<CommittedEvent?>(null)
    @Volatile private var lastTriggerTsMs: Long = 0L

    /**
     * Rolling latest raw Y frame (640×480), exposed for diagnostics. Written by
     * the camera executor on every frame; read by DiagnosticsRenderer when the
     * panel is visible. Volatile + array reference swap is safe enough for a
     * snapshot-style consumer (we never mutate the array in place).
     */
    @Volatile private var lastRawY: ByteArray? = null

    /**
     * Snapshot of the active ROI (in 640×480 coords) for the current run. Set
     * in onStartClicked from the SettingsSnapshot, read by OverlayView.updateLive
     * and renderOverlayJpeg without re-touching SharedPreferences per frame.
     */
    @Volatile private var lastRoi: YuvAdapter.CropRect = YuvAdapter.CropRect(0, 0, 640, 480)

    /**
     * Mask payload of the most recently committed event, for the 6th
     * DiagnosticsRenderer tile. Cleared on Clear/Start.
     */
    // Safe to alias: tpv_jni.c does NewByteArray() per processFrameDebugV2 call, never reuses.
    @Volatile private var lastEventMask: ByteArray? = null

    /**
     * Run-level snapshot of all v2 settings (binThreshold, darkObjectMode, ROI,
     * trigger triple). Set on Start, consumed in onFrame and buildMeta. Null
     * outside of a run.
     */
    @Volatile private var activeSnapshot: SettingsSnapshot? = null

    @Volatile private var lastZip: File? = null

    // Per-frame scratch buffers — owned by the camera.executor thread (onFrame
    // is single-threaded on that executor), so plain fields are safe.
    //   - yScratch: Y-plane copy buffer (640×480 = 307 200 B)
    //   - argbScratch: full-frame ARGB for face detector, allocated only when enabled
    //   - timingScratch: 3-slot LongArray for JNI timing handoff (24 B)
    //   - gapSortScratch: in-place sort buffer for recentGaps (≤30 entries, 240 B at cap)
    // Replaces three per-frame allocations (~310 KB total) with one-shot ones.
    private var yScratch: ByteArray = ByteArray(640 * 480)
    private var argbScratch: IntArray = IntArray(0)
    private val timingScratch: LongArray = LongArray(3)
    private val emptyTpvDebug = TpvDetectionDebugV2(
        det = TpvDetection(1, 0, 0, 0, 0, 0),
        features = TpvFeatures(IntArray(7), 0, 0, 0),
        distancesSq = IntArray(0),
        bbox = TpvBbox(0, 0, 0, 0),
        areaPx = 0,
        grid8x8 = 0,
        bin = ByteArray(640 * 480 / 8),
        allBlobsMask = ByteArray(640 * 480 / 8),
        mask = ByteArray(640 * 480 / 8),
    )
    private var gapSortScratch: LongArray = LongArray(64)

    // Throttle status_line String.format to ~10 Hz (HUD line is debug-only at
    // 30 fps; full-rate updates are ~1 % CPU on lower-tier devices).
    private var lastStatusLineMs: Long = 0L

    // Pre-computed in onStartClicked (UI thread) so the first onFrame()
    // callback isn't taxed with ~10ms of file+hash work — that pollutes
    // A2 timing and can trip the skipped-gap estimator on the first frame.
    @Volatile private var soSha256: String = ""
    @Volatile private var modelSha256: String = ""
    @Volatile private var faceModelSha256: String? = null

    private val cameraPermLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (!granted) toast("Camera permission required") else onStartClicked()
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        settings = SettingsState(this)
        camera = CameraAdapter(this)
        yuv = YuvAdapter(640, 480)
        preview = findViewById(R.id.preview)
        overlay = findViewById(R.id.overlay)
        hud = findViewById(R.id.hud)
        statusLine = findViewById(R.id.status_line)
        diagView = findViewById(R.id.diag)
        btnStart = findViewById(R.id.btn_start)
        btnMode = findViewById(R.id.btn_mode)
        btnStop = findViewById(R.id.btn_stop)
        btnExport = findViewById(R.id.btn_export)
        btnSettings = findViewById(R.id.btn_settings)
        btnCamera = findViewById(R.id.btn_camera)
        btnDiag = findViewById(R.id.btn_diag)
        btnRoi = findViewById(R.id.btn_roi)
        btnClear = findViewById(R.id.btn_clear)

        btnStart.setOnClickListener { requestCameraAndStart() }
        btnMode.setOnClickListener { onModeClicked() }
        btnStop.setOnClickListener { onStopClicked() }
        btnExport.setOnClickListener { onExportClicked() }
        btnSettings.setOnClickListener { showSettingsDialog() }
        btnCamera.setOnClickListener { onCameraSwitchClicked() }
        btnDiag.setOnClickListener { toggleDiagView() }
        btnRoi.setOnClickListener { showSettingsDialog() }
        btnClear.setOnClickListener { onClearClicked() }
        updateModeButton()
        updateCameraButton()
    }

    private fun onModeClicked() {
        if (running.get()) {
            toast("Stop the run before switching recognition mode")
            return
        }
        val nextMode = if (settings.recognitionMode == RecognitionMode.FACE) {
            RecognitionMode.OBJECT
        } else {
            RecognitionMode.FACE
        }
        applyRecognitionMode(nextMode)
        toast("Recognition mode: ${nextMode.displayName}")
    }

    private fun applyRecognitionMode(mode: RecognitionMode) {
        settings.recognitionMode = mode
        overlay.reset()
        diagView.clear()
        updateModeButton()
    }

    private fun updateModeButton() {
        if (!::btnMode.isInitialized || !::btnStart.isInitialized) return
        val faceMode = settings.recognitionMode == RecognitionMode.FACE
        btnMode.text = if (faceMode) "Mode: Face" else "Mode: Object"
        btnStart.text = if (faceMode) "Start Face" else "Start Object"
    }

    private fun onCameraSwitchClicked() {
        if (running.get()) {
            toast("Stop the run before switching camera")
            return
        }
        selectedCameraLens = selectedCameraLens.toggled()
        overlay.reset()
        diagView.clear()
        updateCameraButton()
    }

    private fun updateCameraButton() {
        if (::btnCamera.isInitialized) btnCamera.text = selectedCameraLens.buttonLabel
    }

    private fun toggleDiagView() {
        diagView.visibility = if (diagView.visibility == View.VISIBLE) View.GONE else View.VISIBLE
    }

    /** Wipe transient state (last event + diag tiles + overlay layers) without
     *  touching the active run. Always enabled — useful both during a run (e.g.
     *  to clear a stale flash) and idle. */
    private fun onClearClicked() {
        lastCommittedEvent.set(null)
        lastEventMask = null
        overlay.reset()
        diagView.clear()
        runOnUiThread {
            hud.text = "State: ${trigger?.state ?: MachineState.IDLE}    Events: ${eventCounter.get()}\n" +
                       "Last (no committed event yet)"
        }
    }

    private fun requestCameraAndStart() {
        val perm = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA)
        if (perm == PackageManager.PERMISSION_GRANTED) onStartClicked()
        else cameraPermLauncher.launch(Manifest.permission.CAMERA)
    }

    private fun onStartClicked() {
        if (running.get()) return
        val freeBytes = filesDir.usableSpace
        if (freeBytes < 1_000_000_000L) {
            toast("Less than 1 GB free; consider cleaning old runs")
        }

        val sdf = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
            .apply { timeZone = TimeZone.getTimeZone("UTC") }
        val runId = "run_${sdf.format(Date())}"
        val runDir = File(filesDir, "runs/$runId").apply { mkdirs() }

        // Cross-field ROI validation (per-field clamps in SettingsState already
        // enforce non-negative / ≤640 / ≤480; only the sum needs gating here).
        val rX = settings.roiX ; val rY = settings.roiY
        val rW = settings.roiW ; val rH = settings.roiH
        if (rX + rW > 640 || rY + rH > 480) {
            Toast.makeText(
                this,
                "ROI out of frame: x+w must ≤640, y+h must ≤480",
                Toast.LENGTH_LONG
            ).show()
            return
        }

        // Snapshot ALL v2 settings at the run boundary. Replays / analysis
        // only need this single snapshot (recorded into meta.json) — no
        // per-event ROI/threshold/dark_mode fields needed.
        val snap = SettingsSnapshot(
            n = settings.nStable, k = settings.kEmpty, m = settings.mDriftPx,
            binThreshold = settings.binThreshold,
            darkObjectMode = settings.darkObjectMode,
            roiX = rX, roiY = rY, roiW = rW, roiH = rH,
            cameraLens = selectedCameraLens,
            trackerEnabled = settings.trackerEnabled,
            trackerMinHits = settings.trackerMinHits,
            trackerMaxAge = settings.trackerMaxAge,
            trackerIouThresholdPct = settings.trackerIouThresholdPct,
            trackerCenterDistancePx = settings.trackerCenterDistancePx,
            recognitionMode = settings.recognitionMode,
            faceEnabled = settings.faceEnabled,
            faceMinDetectionConfidencePct = settings.faceMinDetectionConfidencePct,
            faceMinSuppressionThresholdPct = settings.faceMinSuppressionThresholdPct,
        )
        activeSnapshot = snap
        lastRoi = YuvAdapter.CropRect(snap.roiX, snap.roiY, snap.roiW, snap.roiH)

        // Pre-compute once on UI thread so the first frame callback is fast.
        soSha256 = sha256Of(File(applicationInfo.nativeLibraryDir, "libtpv.so").readBytes())
        modelSha256 = try {
            assets.open("tpv_model_sha.txt").bufferedReader().readText().trim()
        } catch (e: Exception) { "unknown" }
        faceModelSha256 = if (snap.recognitionMode == RecognitionMode.FACE) {
            try {
                assets.open(FACE_ENGINE_MODEL_ASSET).use { sha256Of(it.readBytes()) }
            } catch (e: Exception) {
                Log.w(TAG, "Cannot hash face model asset", e)
                null
            }
        } else {
            null
        }
        visionPipeline = try {
            buildVisionPipeline(snap)
        } catch (t: Throwable) {
            Log.e(TAG, "Vision pipeline init failed", t)
            activeSnapshot = null
            toast("Vision init failed: ${t.javaClass.simpleName}")
            return
        }

        pendingInit.set { nativeW, nativeH, crop ->
            val meta = buildMeta(runId, snap, nativeW, nativeH, crop)
            recorder = RunRecorder(runDir, meta).also { it.start() }
        }
        trigger = TriggerMachine(snap.n, snap.k, snap.m)
        frameCounter.set(0) ; eventCounter.set(0) ; fpsWin.clear()
        skippedCount.set(0) ; lastArrivalNs = 0L ; recentGaps.clear()
        lastCommittedEvent.set(null) ; lastEventMask = null
        overlay.reset() ; diagView.clear()

        running.set(true)
        lockUi(true)
        camera.start(
            lifecycleOwner = this,
            previewView = preview,
            lens = snap.cameraLens,
            onError = { onCameraStartError(it) },
        ) { proxy -> onFrame(proxy) }
        Log.i(TAG, "Started run $runId (mode=${snap.recognitionMode}, dark_mode=${snap.darkObjectMode}, " +
                   "thr=${snap.binThreshold}, roi=${snap.roiX},${snap.roiY}," +
                   "${snap.roiW},${snap.roiH})")
    }

    private fun onCameraStartError(t: Throwable) {
        Log.e(TAG, "Camera start failed for ${selectedCameraLens.jsonName}", t)
        running.set(false)
        camera.stop()
        recorder?.close()
        recorder = null
        trigger = null
        visionPipeline?.close()
        visionPipeline = null
        pendingInit.set(null)
        activeSnapshot = null
        runOnUiThread {
            lockUi(false)
            updateCameraButton()
            toast("Cannot open ${selectedCameraLens.jsonName} camera")
        }
    }

    private fun onStopClicked() {
        if (!running.get()) return
        running.set(false)
        camera.stop()   // unbindAll — CameraX stops dispatching NEW frames

        // Spec §3 requires the whole camera → JNI → state machine → recorder
        // chain to stay on a single thread. camera.stop() alone doesn't wait
        // for an in-flight analyzer callback; if we called finalizeMeta /
        // stopAndZip directly on the UI thread, meta.json rewrites and zip
        // enumeration could race with the last onFrame()'s recordEvent /
        // recordFrameTiming. We submit the teardown to the SAME
        // single-thread camera executor: FIFO ordering guarantees any
        // still-running or queued analyzer task completes before the
        // teardown task starts.
        camera.executor.submit {
            recorder?.finalizeMeta(
                skippedFrames = skippedCount.get(),
                totalFrames = frameCounter.get().toLong(),
            )
            val zip = recorder?.stopAndZip()
            visionPipeline?.close()
            recorder = null ; trigger = null
            visionPipeline = null
            pendingInit.set(null)
            activeSnapshot = null
            lastZip = zip
            runOnUiThread {
                lockUi(false)
                btnExport.isEnabled = zip != null
                toast("Stopped. Zip: ${zip?.name ?: "(no frames captured)"}")
            }
        }
    }

    private fun onExportClicked() {
        val zip = lastZip ?: return
        toast("Zip path: ${zip.absolutePath}\nUse: adb exec-out run-as ${packageName} cat files/runs/${zip.name} > run.zip")
    }

    private fun lockUi(runActive: Boolean) = runOnUiThread {
        btnStart.isEnabled = !runActive
        btnMode.isEnabled = !runActive
        btnStop.isEnabled = runActive
        btnSettings.isEnabled = !runActive
        btnCamera.isEnabled = !runActive
        btnRoi.isEnabled = !runActive   // ROI also opens the run-locked dialog
        // btnDiag, btnClear, btnExport: always enabled (Export gated separately)
        updateModeButton()
    }

    /** Pipeline: camera frame → YuvAdapter (Y) → VisionPipeline
     *              → TriggerMachine
     *              → RunRecorder (timing always, event on commit)
     *              → OverlayView (live + persistent commit state)
     *              → DiagnosticsView (when visible) */
    private fun onFrame(proxy: androidx.camera.core.ImageProxy) {
        val tCamArrive = System.nanoTime()
        try {
            // Update skipped-frame estimate from arrival gap vs. sliding median.
            if (lastArrivalNs != 0L) {
                val gap = tCamArrive - lastArrivalNs
                recentGaps.addLast(gap)
                while (recentGaps.size > 30) recentGaps.removeFirst()
                if (recentGaps.size >= 5) {
                    // Reuse gapSortScratch instead of allocating a fresh
                    // LongArray + sortedArray() copy each frame.
                    val n = recentGaps.size
                    if (gapSortScratch.size < n) gapSortScratch = LongArray(n)
                    var i = 0
                    for (g in recentGaps) { gapSortScratch[i] = g ; i++ }
                    Arrays.sort(gapSortScratch, 0, n)
                    val median = gapSortScratch[n / 2]
                    if (median > 0 && gap > median * SKIP_GAP_RATIO_NUM / SKIP_GAP_RATIO_DEN) {
                        val missed = (gap + median / 2) / median - 1
                        if (missed > 0) skippedCount.addAndGet(missed)
                    }
                }
            }
            lastArrivalNs = tCamArrive

            val nativeW = proxy.width ; val nativeH = proxy.height

            // Extract the Y plane for tpv. Full NV21 stays lazy and is only
            // requested from the frame buffer provider on commit.
            val yPlane = proxy.planes[0]
            val yBuf = yPlane.buffer
            val yLen = yBuf.remaining()
            // Reuse yScratch (640×480 = 307 200 B) across frames; resize only
            // if the plane unexpectedly grows (e.g. resolution renegotiation).
            if (yScratch.size < yLen) yScratch = ByteArray(yLen)
            yBuf.get(yScratch, 0, yLen)
            val adapted = yuv.extract(yScratch, yPlane.rowStride, nativeW, nativeH)
            lastRawY = adapted.y

            // First-frame lazy init: now that we know nativeW/H + crop, write meta.json.
            pendingInit.getAndSet(null)?.invoke(nativeW, nativeH, adapted.crop)

            val frameIdx = frameCounter.incrementAndGet().toLong()

            // Snapshot guard. activeSnapshot is set in onStartClicked and
            // cleared in onStopClicked. If we somehow arrive here with no
            // snapshot (e.g. between Stop completion and a stray late frame),
            // bail — the `finally` block still closes the proxy.
            // Safety here is by serial dispatch (camera.executor serializes onFrame
            // and the teardown null-write submitted via executor.submit), NOT @Volatile.
            if (activeSnapshot == null) return
            val pipeline = visionPipeline ?: return

            // Reuse timingScratch (3-slot LongArray) instead of per-frame alloc.
            val timingBuf = timingScratch
            lateinit var frameBuffers: FrameScopedBufferProvider
            frameBuffers = FrameScopedBufferProvider(
                nv21Supplier = {
                    Yuv420ToNv21.convert(proxy)
                },
                argbSupplier = {
                    val pixelCount = nativeW * nativeH
                    if (argbScratch.size < pixelCount) argbScratch = IntArray(pixelCount)
                    Nv21ToArgb.convert(frameBuffers.nv21(), nativeW, nativeH, argbScratch)
                },
            )
            val frame = VisionFrame(
                frameIdxInRun = frameIdx,
                tCameraArriveNs = tCamArrive,
                nativeW = nativeW,
                nativeH = nativeH,
                crop = adapted.crop,
                y640 = adapted.y,
                rotationDegrees = proxy.imageInfo.rotationDegrees,
                buffers = frameBuffers,
                tpvTimingNs = timingBuf,
            )
            val pipelineResult = pipeline.process(frame)
            if (pipelineResult.errors.isNotEmpty()) {
                Log.w(TAG, "VisionPipeline errors: ${pipelineResult.errors.map { it.engineId }}")
            }
            if (activeSnapshot?.recognitionMode == RecognitionMode.FACE) {
                overlay.updateLive(
                    emptyTpvDebug, lastRoi, adapted.crop, nativeW, nativeH,
                    tracks = pipelineResult.trackedDetections,
                    mirrorX = selectedCameraLens.mirrorPreviewX,
                )
                if (diagView.visibility == View.VISIBLE) diagView.clear()
                updateFaceHud(
                    detectionCount = pipelineResult.primaryDetections.size,
                    trackCount = pipelineResult.primaryTracks.count {
                        it.state == TrackState.CONFIRMED
                    },
                )
                return
            }
            val tpvEngineResult = pipelineResult.engineResult(TPV_BLOB_ENGINE_ID) ?: return
            val result = tpvEngineResult.requireRaw<TpvDetectionDebugV2>()
            val tJniReturn = tpvEngineResult.tReturnNs

            // Compute presence BEFORE recordFrameTiming so we can honour the
            // FrameTiming contract: tpvClassId must be -1 for DROP frames
            // (spec §5.6 / plan type header). For TPV_OK/TPV_EMPTY the classId
            // is meaningful; for SCENE_ERROR/BAD_INPUT tpv never looked at
            // pixels, so classId has no semantics.
            val presence = when {
                result.det.status == 0 -> FramePresence.PRESENT       // TPV_OK (any class_id)
                result.det.status == 1 -> FramePresence.EMPTY         // TPV_EMPTY
                else -> FramePresence.DROP                            // SCENE_ERROR, BAD_INPUT
            }
            val timingClassId = if (presence == FramePresence.DROP) -1 else result.det.classId

            recorder?.recordFrameTiming(FrameTiming(
                frameIdxInRun = frameIdx,
                tpvStatus = result.det.status, tpvClassId = timingClassId,
                tCameraArriveNs = tCamArrive,
                tJniEnterNs  = timingBuf[0],    // JNI entry, C side
                tTpvEnterNs  = timingBuf[1],    // immediately before tpv_vision_process
                tTpvExitNs   = timingBuf[2],    // after TPV + native tracker/policy
                tJniReturnNs = tJniReturn       // back in Kotlin
            ))
            val obs = FrameObservation(
                presence = presence,
                x = if (presence == FramePresence.PRESENT) result.det.x else 0,
                y = if (presence == FramePresence.PRESENT) result.det.y else 0,
                classId = if (presence == FramePresence.PRESENT) result.det.classId else -1,
                frameIdxInRun = frameIdx,
                detection = if (presence == FramePresence.PRESENT) result else null,
            )
            val out = trigger?.onFrame(obs) ?: StateMachineOutput.None

            if (out is StateMachineOutput.Commit) {
                val triggerTsMs = System.currentTimeMillis()
                val jpg = renderOverlayJpeg(
                    d = out.event.triggerFrameDebug, crop = adapted.crop,
                    nativeW = nativeW, nativeH = nativeH,
                    eventClassId = out.event.eventClassId, flicker = out.event.flicker,
                    nv21 = frame.buffers.nv21(),
                )
                recorder?.recordEvent(
                    out.event, triggerTsMs = triggerTsMs,
                    rawY = adapted.y, overlayJpeg = jpg,
                    mask = out.event.triggerFrameDebug.mask,
                    tracks = pipelineResult.primaryTracks,
                )
                eventCounter.incrementAndGet()
                lastCommittedEvent.set(out.event)
                lastEventMask = out.event.triggerFrameDebug.mask
                lastTriggerTsMs = triggerTsMs
                overlay.onCommit(out.event.eventClassId, out.event.flicker)
            }

            // OverlayView itself gates TPV mask/dot/axis on TPV_OK. We still
            // push every frame so non-primary engines (face) can draw live
            // tracks even when the TPV primary reports EMPTY.
            overlay.updateLive(
                result, lastRoi, adapted.crop, nativeW, nativeH,
                tracks = pipelineResult.trackedDetections,
                mirrorX = selectedCameraLens.mirrorPreviewX,
            )

            // Diagnostics panel: only render when actually visible. The
            // renderer + 6 IntArrays are not free (~250 KB / call); skipping
            // when hidden saves CPU while preserving the same per-frame data
            // path. DiagnosticsView itself further throttles to 10 Hz.
            if (diagView.visibility == View.VISIBLE) {
                val panels = DiagnosticsRenderer.render(
                    adapted.y, result, lastRoi, lastEventMask,
                )
                diagView.update(panels)
            }

            updateHud(result, presence)
        } finally {
            proxy.close()
        }
    }

    /**
     * Real RGB overlay JPEG, built from the trigger frame's NV21. Path:
     * NV21 → YuvImage.compressToJpeg() → Bitmap (ARGB) → Canvas draw
     * mask + ROI + center dot + axis + 2-line text → JPEG-85.
     *
     * v2 (T-v2.6): paints the same primitives as OverlayView v2 — green mask
     * fill, yellow ROI rectangle, red center dot, short axis line. The two
     * legacy text lines are kept so the saved JPG is self-describing without
     * needing the matching log.jsonl entry. v1's class-coloured circle is
     * dropped in favour of the v2 red dot for parity with the on-screen view.
     */
    private fun renderOverlayJpeg(
        d: TpvDetectionDebugV2, crop: YuvAdapter.CropRect,
        nativeW: Int, nativeH: Int,
        eventClassId: Int, flicker: Boolean, nv21: ByteArray,
    ): ByteArray {
        // Step 1: NV21 → JPEG bytes (full frame, no sub-region)
        val baos0 = ByteArrayOutputStream(nativeW * nativeH / 2)
        YuvImage(nv21, ImageFormat.NV21, nativeW, nativeH, null)
            .compressToJpeg(Rect(0, 0, nativeW, nativeH), 85, baos0)
        val rgbJpg = baos0.toByteArray()

        // Step 2: decode to an ARGB bitmap we can draw on
        val bmp = BitmapFactory.decodeByteArray(rgbJpg, 0, rgbJpg.size)
            ?.copy(Bitmap.Config.ARGB_8888, /*mutable=*/true)
            ?: // Defensive: if decode somehow fails, fall back to black so the
               // pipeline still writes a file rather than dropping the event.
               Bitmap.createBitmap(nativeW, nativeH, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)

        // Step 3a: green mask fill (v2). One-shot allocations are fine here:
        // renderOverlayJpeg only runs on commits, not per frame.
        val maskPixels = OverlayPainter.decodeMaskToArgb(
            d.mask, 640, 480, OverlayPainter.GREEN_MASK_ARGB)
        val maskBmp = Bitmap.createBitmap(maskPixels, 640, 480, Bitmap.Config.ARGB_8888)
        canvas.drawBitmap(
            maskBmp, null,
            Rect(crop.x, crop.y, crop.x + crop.w, crop.y + crop.h), null
        )

        // Step 3b: yellow ROI rectangle, mapped from 640×480 → native coords.
        val roiPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE ; strokeWidth = 4f
            this.color = OverlayPainter.YELLOW_ROI_ARGB
        }
        val (rnx0, rny0) = OverlayPainter.mapCoord(lastRoi.x, lastRoi.y, crop)
        val (rnx1, rny1) = OverlayPainter.mapCoord(
            lastRoi.x + lastRoi.w, lastRoi.y + lastRoi.h, crop)
        canvas.drawRect(
            rnx0.toFloat(), rny0.toFloat(), rnx1.toFloat(), rny1.toFloat(), roiPaint
        )

        // Step 3c: red center dot + short axis line — mirror OverlayView v2.
        val centerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.FILL
            this.color = OverlayPainter.RED_CENTER_ARGB
        }
        val axisPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE ; strokeWidth = 3f
            this.color = OverlayPainter.RED_CENTER_ARGB
        }
        val (nx, ny) = OverlayPainter.mapCoord(d.det.x, d.det.y, crop)
        val cx = nx.toFloat() ; val cy = ny.toFloat()
        val dotR = (crop.w * 0.015f).coerceAtLeast(4f)
        canvas.drawCircle(cx, cy, dotR, centerPaint)
        val axisLen = (crop.w * 0.04f).coerceAtLeast(8f)
        val thetaRad = Math.toRadians(d.det.thetaX10 / 10.0)
        canvas.drawLine(
            cx, cy,
            (cx + axisLen * cos(thetaRad)).toFloat(),
            (cy + axisLen * sin(thetaRad)).toFloat(),
            axisPaint
        )

        // Step 3d: two HUD-like text lines so the JPG is self-describing
        // even if the matching log.jsonl entry is unavailable.
        val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 48f ; color = OverlayPainter.colorFor(d.det.classId)
        }
        canvas.drawText(OverlayPainter.textLine1(d), 16f, 64f, textPaint)
        textPaint.color = OverlayPainter.GREY_NEUTRAL
        canvas.drawText(OverlayPainter.textLine2(eventClassId, flicker), 16f, 128f, textPaint)

        // Step 4: recompress to JPEG-85
        val baos = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.JPEG, 85, baos)
        return baos.toByteArray()
    }

    /**
     * HUD + status_line rendering. The "Last" lines read from `lastCommittedEvent`
     * and persist across frames that aren't themselves commits — per spec §9
     * "最近一次 COMMITTED 事件的摘要".
     *
     * v2 (T-v2.6): status_line shows live per-frame metrics (size/bbox/grid/θ
     * + FPS + skipped); HUD condenses to State + Events + Last event summary.
     */
    private fun updateHud(live: TpvDetectionDebugV2, presence: FramePresence) {
        val now = System.nanoTime()
        fpsWin.addLast(now)
        while (fpsWin.size > fpsWindowSize) fpsWin.removeFirst()
        val fps = if (fpsWin.size >= 2) {
            (fpsWin.size - 1).toDouble() * 1_000_000_000.0 / (fpsWin.last() - fpsWin.first())
        } else 0.0

        val st = trigger?.state ?: MachineState.IDLE

        // status_line is throttled to ~10 Hz: at 30 fps the per-frame
        // String.format() + UI write is wasted CPU (eyes can't read at
        // 30 Hz anyway). HUD line uses simple concatenation so it stays
        // per-frame.
        val nowMs = SystemClock.elapsedRealtime()
        val statusText: String? = if (nowMs - lastStatusLineMs >= 100L) {
            lastStatusLineMs = nowMs
            if (presence == FramePresence.PRESENT) {
                val theta = live.det.thetaX10 / 10.0
                "size:%d [%d×%d] grid:%d rotation:%.1f°    FPS:%.1f skipped:%d".format(
                    live.areaPx, live.bbox.w, live.bbox.h, live.grid8x8,
                    theta, fps, skippedCount.get(),
                )
            } else {
                "size:- [-×-] grid:- rotation:-°    FPS:%.1f skipped:%d".format(
                    fps, skippedCount.get(),
                )
            }
        } else null

        // HUD — State + Events + Last event summary.
        val ev = lastCommittedEvent.get()
        val hudMsg = buildString {
            append("State: $st    Events: ${eventCounter.get()}\n")
            if (ev != null) {
                val d = ev.triggerFrameDebug
                append("Last event#${ev.eventIdx}: cls=${ev.eventClassId} ")
                append("flicker=${ev.flicker} size=${d.areaPx} ")
                append("θ=%.1f°".format(d.det.thetaX10 / 10.0))
            } else {
                append("Last (no committed event yet)")
            }
        }
        runOnUiThread {
            if (statusText != null) statusLine.text = statusText
            hud.text = hudMsg
        }
    }

    private fun updateFaceHud(detectionCount: Int, trackCount: Int) {
        val now = System.nanoTime()
        fpsWin.addLast(now)
        while (fpsWin.size > fpsWindowSize) fpsWin.removeFirst()
        val fps = if (fpsWin.size >= 2) {
            (fpsWin.size - 1).toDouble() * 1_000_000_000.0 / (fpsWin.last() - fpsWin.first())
        } else 0.0

        val nowMs = SystemClock.elapsedRealtime()
        val statusText = if (nowMs - lastStatusLineMs >= 100L) {
            lastStatusLineMs = nowMs
            "mode:FACE faces:%d tracks:%d    FPS:%.1f skipped:%d".format(
                detectionCount, trackCount, fps, skippedCount.get(),
            )
        } else null

        runOnUiThread {
            if (statusText != null) statusLine.text = statusText
            hud.text = "Mode: FACE    Faces: $detectionCount    Tracks: $trackCount\nObject recognition disabled"
        }
    }

    /**
     * v2 settings dialog with three sections (Trigger / Pipeline / ROI). All
     * fields are run-locked: in-run invocations bail with a toast.
     */
    private fun showSettingsDialog() {
        if (running.get()) { toast("Stop the run before changing settings") ; return }

        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL ; setPadding(32, 16, 32, 16)
        }
        fun sectionHeader(title: String) = TextView(this).apply {
            text = title ; textSize = 16f ; setPadding(0, 16, 0, 8)
        }

        // ---- Trigger ----
        container.addView(sectionHeader("Trigger"))
        val nEt = EditText(this).apply {
            setText(settings.nStable.toString()) ; hint = "N_stable"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val kEt = EditText(this).apply {
            setText(settings.kEmpty.toString()) ; hint = "K_empty"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val mEt = EditText(this).apply {
            setText(settings.mDriftPx.toString()) ; hint = "M_drift_px"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        container.addView(nEt) ; container.addView(kEt) ; container.addView(mEt)

        // ---- Pipeline ----
        container.addView(sectionHeader("Pipeline"))
        val faceModeCb = CheckBox(this).apply {
            text = "Face recognition mode (object recognition off)"
            isChecked = settings.recognitionMode == RecognitionMode.FACE
        }
        val modeNote = TextView(this).apply {
            text = "Only one recognition engine runs per Start."
            textSize = 12f
            setPadding(0, 0, 0, 8)
        }
        val darkCb = CheckBox(this).apply {
            text = "Dark object mode (Y < threshold = fg)"
            isChecked = settings.darkObjectMode
        }
        val thrEt = EditText(this).apply {
            setText(settings.binThreshold.toString()) ; hint = "bin_threshold (0-255)"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        container.addView(faceModeCb)
        container.addView(modeNote)
        container.addView(darkCb) ; container.addView(thrEt)

        // ---- ROI (640×480 coords; defaults = full frame) ----
        container.addView(sectionHeader("ROI"))
        val roiXEt = EditText(this).apply {
            setText(settings.roiX.toString()) ; hint = "x (0-639)"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val roiYEt = EditText(this).apply {
            setText(settings.roiY.toString()) ; hint = "y (0-479)"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val roiWEt = EditText(this).apply {
            setText(settings.roiW.toString()) ; hint = "w (1-640)"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val roiHEt = EditText(this).apply {
            setText(settings.roiH.toString()) ; hint = "h (1-480)"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        container.addView(roiXEt) ; container.addView(roiYEt)
        container.addView(roiWEt) ; container.addView(roiHEt)

        // ---- Tracker ----
        container.addView(sectionHeader("Tracker"))
        val trackerEnabledCb = CheckBox(this).apply {
            text = "Enable tracker"
            isChecked = settings.trackerEnabled
        }
        val trackerMinHitsEt = EditText(this).apply {
            setText(settings.trackerMinHits.toString()) ; hint = "min_hits"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val trackerMaxAgeEt = EditText(this).apply {
            setText(settings.trackerMaxAge.toString()) ; hint = "max_age"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val trackerIouEt = EditText(this).apply {
            setText(settings.trackerIouThresholdPct.toString()) ; hint = "iou_threshold_pct"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val trackerDistanceEt = EditText(this).apply {
            setText(settings.trackerCenterDistancePx.toString()) ; hint = "center_distance_px"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        container.addView(trackerEnabledCb)
        container.addView(trackerMinHitsEt) ; container.addView(trackerMaxAgeEt)
        container.addView(trackerIouEt) ; container.addView(trackerDistanceEt)

        // ---- Face Detection ----
        container.addView(sectionHeader("Face Detection"))
        val faceConfidenceEt = EditText(this).apply {
            setText(settings.faceMinDetectionConfidencePct.toString())
            hint = "min_detection_confidence_pct"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        val faceSuppressionEt = EditText(this).apply {
            setText(settings.faceMinSuppressionThresholdPct.toString())
            hint = "min_suppression_threshold_pct"
            inputType = InputType.TYPE_CLASS_NUMBER
        }
        container.addView(faceConfidenceEt) ; container.addView(faceSuppressionEt)

        AlertDialog.Builder(this)
            .setTitle("Settings (run-locked)")
            .setView(container)
            .setPositiveButton("OK") { _, _ ->
                // Null-coalesce preserves the current value on parse failure
                // (blank field, non-numeric text). In-field clamps in
                // SettingsState are still the safety net for out-of-range ints.
                settings.nStable        = nEt.text.toString().toIntOrNull()    ?: settings.nStable
                settings.kEmpty         = kEt.text.toString().toIntOrNull()    ?: settings.kEmpty
                settings.mDriftPx       = mEt.text.toString().toIntOrNull()    ?: settings.mDriftPx
                applyRecognitionMode(if (faceModeCb.isChecked) {
                    RecognitionMode.FACE
                } else {
                    RecognitionMode.OBJECT
                })
                settings.darkObjectMode = darkCb.isChecked
                settings.binThreshold   = thrEt.text.toString().toIntOrNull()  ?: settings.binThreshold
                settings.roiX           = roiXEt.text.toString().toIntOrNull() ?: settings.roiX
                settings.roiY           = roiYEt.text.toString().toIntOrNull() ?: settings.roiY
                settings.roiW           = roiWEt.text.toString().toIntOrNull() ?: settings.roiW
                settings.roiH           = roiHEt.text.toString().toIntOrNull() ?: settings.roiH
                settings.trackerEnabled = trackerEnabledCb.isChecked
                settings.trackerMinHits = trackerMinHitsEt.text.toString().toIntOrNull()
                    ?: settings.trackerMinHits
                settings.trackerMaxAge = trackerMaxAgeEt.text.toString().toIntOrNull()
                    ?: settings.trackerMaxAge
                settings.trackerIouThresholdPct = trackerIouEt.text.toString().toIntOrNull()
                    ?: settings.trackerIouThresholdPct
                settings.trackerCenterDistancePx = trackerDistanceEt.text.toString().toIntOrNull()
                    ?: settings.trackerCenterDistancePx
                settings.faceMinDetectionConfidencePct = faceConfidenceEt.text.toString().toIntOrNull()
                    ?: settings.faceMinDetectionConfidencePct
                settings.faceMinSuppressionThresholdPct = faceSuppressionEt.text.toString().toIntOrNull()
                    ?: settings.faceMinSuppressionThresholdPct
                toast("Settings saved")
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** Run-level settings snapshot — captures everything the C-side and recorder
     *  must agree on for the duration of a single run. */
    private data class SettingsSnapshot(
        val n: Int, val k: Int, val m: Int,
        val binThreshold: Int,
        val darkObjectMode: Boolean,
        val roiX: Int, val roiY: Int, val roiW: Int, val roiH: Int,
        val cameraLens: BenchCameraLens,
        val trackerEnabled: Boolean,
        val trackerMinHits: Int,
        val trackerMaxAge: Int,
        val trackerIouThresholdPct: Int,
        val trackerCenterDistancePx: Int,
        val recognitionMode: RecognitionMode,
        val faceEnabled: Boolean,
        val faceMinDetectionConfidencePct: Int,
        val faceMinSuppressionThresholdPct: Int,
    )

    private fun buildVisionPipeline(s: SettingsSnapshot): VisionPipeline {
        val roi = YuvAdapter.CropRect(s.roiX, s.roiY, s.roiW, s.roiH)
        val engines = mutableListOf<VisionEngine>()
        if (s.recognitionMode == RecognitionMode.OBJECT) {
            engines += TpvBlobEngine(
                TpvBlobConfig(
                    binThreshold = s.binThreshold,
                    darkObjectMode = s.darkObjectMode,
                    roi = roi,
                    modelDataSha256 = modelSha256,
                    trackerEnabled = s.trackerEnabled,
                    trackerMinHits = s.trackerMinHits,
                    trackerMaxAge = s.trackerMaxAge,
                    trackerIouThreshold = s.trackerIouThresholdPct / 100f,
                    trackerCenterDistancePx = s.trackerCenterDistancePx.toFloat(),
                )
            )
        } else {
            engines += FaceEngine(
                applicationContext,
                FaceEngineConfig(
                    enabled = true,
                    modelSha256 = faceModelSha256,
                    minDetectionConfidence = s.faceMinDetectionConfidencePct / 100f,
                    minSuppressionThreshold = s.faceMinSuppressionThresholdPct / 100f,
                ),
            )
        }
        val tracker = if (s.trackerEnabled) {
            MultiObjectTracker(buildTrackerConfig(s))
        } else {
            NoopObjectTracker()
        }
        return VisionPipeline(
            engines = engines,
            tracker = tracker,
            policyConfig = EventPolicyConfig(
                primaryEventEngine = primaryEngineId(s),
                enabledCommitEngines = if (s.recognitionMode == RecognitionMode.FACE) {
                    emptySet()
                } else {
                    setOf(primaryEngineId(s))
                },
                mode = if (s.recognitionMode == RecognitionMode.FACE) {
                    CommitMode.LIVE_ONLY
                } else {
                    CommitMode.PRIMARY_ONLY
                },
            ),
        )
    }

    private fun primaryEngineId(s: SettingsSnapshot): String =
        if (s.recognitionMode == RecognitionMode.FACE) FACE_ENGINE_ID else TPV_BLOB_ENGINE_ID

    private fun buildTrackerConfig(s: SettingsSnapshot): TrackerConfig =
        TrackerConfig(
            minHits = s.trackerMinHits,
            maxAge = s.trackerMaxAge,
            iouThreshold = s.trackerIouThresholdPct / 100f,
            centerDistancePx = s.trackerCenterDistancePx.toFloat(),
        )

    private fun buildVisionRunConfig(s: SettingsSnapshot): VisionRunConfig =
        VisionRunConfig(
            schemaVersion = 1,
            engines = listOfNotNull(
                VisionEngineRunConfig(
                    id = TPV_BLOB_ENGINE_ID,
                    type = "native_c",
                    version = "v3",
                    modelSha256 = modelSha256,
                    providerVersion = null,
                    requiredInputs = listOf("Y640"),
                    params = TpvBlobRunParams(
                        binThreshold = s.binThreshold,
                        darkObjectMode = s.darkObjectMode,
                        roiX = s.roiX,
                        roiY = s.roiY,
                        roiW = s.roiW,
                        roiH = s.roiH,
                    ),
                    enabled = s.recognitionMode == RecognitionMode.OBJECT,
                ),
                VisionEngineRunConfig(
                    id = FACE_ENGINE_ID,
                    type = "mediapipe_tasks",
                    version = "face_detector",
                    modelSha256 = faceModelSha256,
                    providerVersion = "mediapipe-tasks-$MEDIAPIPE_TASKS_VISION_VERSION",
                    requiredInputs = listOf("ARGB8888"),
                    params = FaceRunParams(
                        modelAssetPath = FACE_ENGINE_MODEL_ASSET,
                        minDetectionConfidence = s.faceMinDetectionConfidencePct / 100f,
                        minSuppressionThreshold = s.faceMinSuppressionThresholdPct / 100f,
                    ),
                    enabled = s.recognitionMode == RecognitionMode.FACE,
                )
            ),
            eventPolicy = VisionEventPolicyRunConfig(
                mode = if (s.recognitionMode == RecognitionMode.FACE) "LIVE_ONLY" else "PRIMARY_ONLY",
                primaryEventEngine = primaryEngineId(s),
                enabledCommitEngines = if (s.recognitionMode == RecognitionMode.FACE) {
                    emptyList()
                } else {
                    listOf(primaryEngineId(s))
                },
            ),
            tracker = VisionTrackerRunConfig(
                type = if (s.trackerEnabled) "sort_like" else "noop",
                enabled = s.trackerEnabled,
                minHits = s.trackerMinHits,
                maxAge = s.trackerMaxAge,
                iouThreshold = s.trackerIouThresholdPct / 100f,
                centerDistancePx = s.trackerCenterDistancePx.toFloat(),
            ),
        )

    private fun buildMeta(
        runId: String, s: SettingsSnapshot,
        nativeW: Int, nativeH: Int, crop: YuvAdapter.CropRect,
    ): MetaInfo {
        return MetaInfo(
            runId = runId,
            deviceModel = Build.MODEL, androidLevel = Build.VERSION.SDK_INT,
            abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown",
            cpuMaxFreqKhz = readMaxCpuFreqKhz(),
            soSha256 = soSha256, modelDataSha256 = modelSha256,  // pre-computed
            nClasses = TpvNative.nClasses(),
            binThreshold = s.binThreshold,
            darkObjectMode = s.darkObjectMode,
            roiX = s.roiX, roiY = s.roiY, roiW = s.roiW, roiH = s.roiH,
            nStable = s.n, kEmpty = s.k, mDriftPx = s.m,
            requestedW = 640, requestedH = 480,
            nativeW = nativeW, nativeH = nativeH,
            cameraLensFacing = s.cameraLens.jsonName,
            cropX = crop.x, cropY = crop.y, cropW = crop.w, cropH = crop.h,
            downsampleRatioX = crop.w / 640.0,
            downsampleRatioY = crop.h / 480.0,
            vision = buildVisionRunConfig(s),
        )
    }

    private fun sha256Of(bytes: ByteArray): String {
        val md = MessageDigest.getInstance("SHA-256").apply { update(bytes) }
        val d = md.digest()
        return buildString(64) { for (b in d) append("%02x".format(b.toInt() and 0xFF)) }
    }

    private fun readMaxCpuFreqKhz(): Long = try {
        File("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq")
            .readText().trim().toLong()
    } catch (e: Exception) { 0L }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()

    companion object {
        private const val TAG = "TpvBench"
        // Skipped-frame detection: a gap > SKIP_GAP_RATIO × median triggers
        // a "missed frames" estimate. 1.5× is a bench-test heuristic; tune if
        // skipped counts look off in field data.
        private const val SKIP_GAP_RATIO_NUM = 3
        private const val SKIP_GAP_RATIO_DEN = 2
    }
}
