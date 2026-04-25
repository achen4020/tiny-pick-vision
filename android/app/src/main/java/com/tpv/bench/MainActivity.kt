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
import android.util.Log
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.text.SimpleDateFormat
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
    private lateinit var btnStart: Button
    private lateinit var btnStop: Button
    private lateinit var btnExport: Button
    private lateinit var btnSettings: Button

    private var recorder: RunRecorder? = null
    private var trigger: TriggerMachine? = null
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

    @Volatile private var lastZip: File? = null

    // Pre-computed in onStartClicked (UI thread) so the first onFrame()
    // callback isn't taxed with ~10ms of file+hash work — that pollutes
    // A2 timing and can trip the skipped-gap estimator on the first frame.
    @Volatile private var soSha256: String = ""
    @Volatile private var modelSha256: String = ""

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
        btnStart = findViewById(R.id.btn_start)
        btnStop = findViewById(R.id.btn_stop)
        btnExport = findViewById(R.id.btn_export)
        btnSettings = findViewById(R.id.btn_settings)

        btnStart.setOnClickListener { requestCameraAndStart() }
        btnStop.setOnClickListener { onStopClicked() }
        btnExport.setOnClickListener { onExportClicked() }
        btnSettings.setOnClickListener { showSettingsDialog() }
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
        val snapshotSettings = SettingsSnapshot(
            settings.nStable, settings.kEmpty, settings.mDriftPx
        )

        // Pre-compute once on UI thread so the first frame callback is fast.
        soSha256 = sha256Of(File(applicationInfo.nativeLibraryDir, "libtpv.so").readBytes())
        modelSha256 = try {
            assets.open("tpv_model_sha.txt").bufferedReader().readText().trim()
        } catch (e: Exception) { "unknown" }

        pendingInit.set { nativeW, nativeH, crop ->
            val meta = buildMeta(runId, snapshotSettings, nativeW, nativeH, crop)
            recorder = RunRecorder(runDir, meta).also { it.start() }
        }
        trigger = TriggerMachine(snapshotSettings.n, snapshotSettings.k, snapshotSettings.m)
        frameCounter.set(0) ; eventCounter.set(0) ; fpsWin.clear()
        skippedCount.set(0) ; lastArrivalNs = 0L ; recentGaps.clear()
        lastCommittedEvent.set(null) ; overlay.reset()

        running.set(true)
        lockUi(true)
        camera.start(this, preview) { proxy -> onFrame(proxy) }
        Log.i(TAG, "Started run $runId (recorder deferred until first frame)")
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
            recorder = null ; trigger = null
            pendingInit.set(null)
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
        btnStop.isEnabled = runActive
        btnSettings.isEnabled = !runActive
    }

    /** Pipeline: camera frame → YuvAdapter (Y) + Yuv420ToNv21 (full NV21)
     *              → JNI (timing + detection) → TriggerMachine
     *              → RunRecorder (timing always, event on commit)
     *              → OverlayView (live + persistent commit state) */
    private fun onFrame(proxy: androidx.camera.core.ImageProxy) {
        val tCamArrive = System.nanoTime()
        // Update skipped-frame estimate from arrival gap vs. sliding median.
        if (lastArrivalNs != 0L) {
            val gap = tCamArrive - lastArrivalNs
            recentGaps.addLast(gap)
            while (recentGaps.size > 30) recentGaps.removeFirst()
            if (recentGaps.size >= 5) {
                val sorted = recentGaps.toLongArray().sortedArray()
                val median = sorted[sorted.size / 2]
                if (median > 0 && gap > median * SKIP_GAP_RATIO_NUM / SKIP_GAP_RATIO_DEN) {
                    val missed = (gap + median / 2) / median - 1
                    if (missed > 0) skippedCount.addAndGet(missed)
                }
            }
        }
        lastArrivalNs = tCamArrive

        val nativeW = proxy.width ; val nativeH = proxy.height

        // Extract the Y plane for tpv, AND collect the full NV21 for the
        // eventual .jpg. Both happen before we close the proxy.
        val yPlane = proxy.planes[0]
        val yBuf = yPlane.buffer
        val yArr = ByteArray(yBuf.remaining()).also { yBuf.get(it) }
        val adapted = yuv.extract(yArr, yPlane.rowStride, nativeW, nativeH)
        val nv21 = Yuv420ToNv21.convert(proxy)

        // First-frame lazy init: now that we know nativeW/H + crop, write meta.json.
        pendingInit.getAndSet(null)?.invoke(nativeW, nativeH, adapted.crop)

        val frameIdx = frameCounter.incrementAndGet().toLong()

        val timingBuf = LongArray(3)
        // TODO(T-v2.6): plumb darkObjectMode + binThreshold + roi from SettingsState snapshot
        val result = TpvNative.processFrameDebugV2(
            adapted.y, 640, 480,
            TpvNative.binThreshold(), /*darkObjectMode=*/false,
            /*roiX=*/0, /*roiY=*/0, /*roiW=*/640, /*roiH=*/480,
            timingBuf,
        )
        val tJniReturn = System.nanoTime()

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
            tTpvEnterNs  = timingBuf[1],    // immediately before tpv_process_frame_debug
            tTpvExitNs   = timingBuf[2],    // immediately after  tpv_process_frame_debug
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
                nv21 = nv21,
            )
            recorder?.recordEvent(
                out.event, triggerTsMs = triggerTsMs,
                rawY = adapted.y, overlayJpeg = jpg,
                mask = out.event.triggerFrameDebug.mask,
            )
            eventCounter.incrementAndGet()
            lastCommittedEvent.set(out.event)
            lastTriggerTsMs = triggerTsMs
            overlay.onCommit(out.event.eventClassId, out.event.flicker)
        }

        // Only paint a live marker on PRESENT frames. On TPV_EMPTY /
        // SCENE_ERROR / BAD_INPUT the C side zero-fills det, so rendering
        // would otherwise draw a bogus (0,0) class-0 dot. clearLive()
        // drops only the live layer — commit state + 300 ms flash survive.
        if (presence == FramePresence.PRESENT) {
            // TODO(T-v2.6): wire ROI from SettingsState (currently hard-coded full-frame)
            val overlayRoi = YuvAdapter.CropRect(0, 0, 640, 480)
            overlay.updateLive(result, overlayRoi, adapted.crop, nativeW, nativeH)
        } else {
            overlay.clearLive()
        }
        updateHud(result, presence)
    }

    /**
     * Real RGB overlay JPEG, built from the trigger frame's NV21. Path:
     * NV21 → YuvImage.compressToJpeg() → Bitmap (ARGB) → Canvas draw
     * overlay → Bitmap.compress(JPEG, 85). Fully honours spec §5.5's
     * "original camera RGB + overlay".
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

        // Step 3: overlay annotations — same rules as OverlayView (§5.5)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE ; strokeWidth = 4f
            color = OverlayPainter.colorFor(d.det.classId)
        }
        val (nx, ny) = OverlayPainter.mapCoord(d.det.x, d.det.y, crop)
        canvas.drawCircle(
            nx.toFloat(), ny.toFloat(),
            OverlayPainter.circleRadius(crop).toFloat(), paint
        )
        val axisLen = OverlayPainter.axisLength(crop).toFloat()
        val th = Math.toRadians(d.det.thetaX10 / 10.0)
        canvas.drawLine(
            nx.toFloat(), ny.toFloat(),
            (nx + axisLen * cos(th)).toFloat(),
            (ny + axisLen * sin(th)).toFloat(),
            paint
        )

        val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = 48f ; color = paint.color
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
     * HUD rendering. The "Last" lines read from `lastCommittedEvent` and
     * persist across frames that aren't themselves commits — per spec §9
     * "最近一次 COMMITTED 事件的摘要".
     */
    private fun updateHud(live: TpvDetectionDebugV2, presence: FramePresence) {
        val now = System.nanoTime()
        fpsWin.addLast(now)
        while (fpsWin.size > fpsWindowSize) fpsWin.removeFirst()
        val fps = if (fpsWin.size >= 2) {
            (fpsWin.size - 1).toDouble() * 1_000_000_000.0 / (fpsWin.last() - fpsWin.first())
        } else 0.0

        val st = trigger?.state ?: MachineState.IDLE
        val ev = lastCommittedEvent.get()
        val msg = buildString {
            append("State: $st   FPS: %.1f   skipped: %d\n".format(
                fps, skippedCount.get()))
            append("Events: ${eventCounter.get()}\n")
            if (ev != null) {
                val d = ev.triggerFrameDebug
                append("Last ${OverlayPainter.textLine1(d)}\n")
                append("Last ${OverlayPainter.textLine2(ev.eventClassId, ev.flicker)}" +
                       "  x=${d.det.x} y=${d.det.y} θ=${d.det.thetaX10/10.0}")
            } else {
                append("Last (no committed event yet)\n")
                // Fallback line: on EMPTY/DROP the `live` struct is zero-filled,
                // so textLine1(live) would print misleading det_cls=0 conf=0.
                if (presence == FramePresence.PRESENT) {
                    append("Live ${OverlayPainter.textLine1(live)}")
                } else {
                    append("Live (nothing in view)")
                }
            }
        }
        runOnUiThread { hud.text = msg }
    }

    private fun showSettingsDialog() {
        if (running.get()) { toast("Stop the run before changing settings") ; return }
        val ll = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL ; setPadding(32, 16, 32, 16)
        }
        val nEt = EditText(this).apply { setText(settings.nStable.toString()) ; hint = "N_stable" }
        val kEt = EditText(this).apply { setText(settings.kEmpty.toString()) ; hint = "K_empty" }
        val mEt = EditText(this).apply { setText(settings.mDriftPx.toString()) ; hint = "M_drift_px" }
        ll.addView(nEt) ; ll.addView(kEt) ; ll.addView(mEt)
        AlertDialog.Builder(this)
            .setTitle("Trigger Settings")
            .setView(ll)
            .setPositiveButton("OK") { _, _ ->
                settings.nStable = nEt.text.toString().toIntOrNull() ?: 3
                settings.kEmpty = kEt.text.toString().toIntOrNull() ?: 5
                settings.mDriftPx = mEt.text.toString().toIntOrNull() ?: 30
                toast("Settings saved")
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private data class SettingsSnapshot(val n: Int, val k: Int, val m: Int)

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
            nClasses = TpvNative.nClasses(), binThreshold = TpvNative.binThreshold(),
            // TODO(T-v2.6): plumb darkObjectMode + binThreshold + roi from SettingsState snapshot
            darkObjectMode = false,
            roiX = 0, roiY = 0, roiW = 640, roiH = 480,
            nStable = s.n, kEmpty = s.k, mDriftPx = s.m,
            requestedW = 640, requestedH = 480,
            nativeW = nativeW, nativeH = nativeH,
            cropX = crop.x, cropY = crop.y, cropW = crop.w, cropH = crop.h,
            downsampleRatioX = crop.w / 640.0,
            downsampleRatioY = crop.h / 480.0
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
