package com.tpv.bench

import android.content.Context
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleOwner
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

enum class BenchCameraLens(
    val lensFacing: Int,
    val jsonName: String,
    val buttonLabel: String,
    val mirrorPreviewX: Boolean,
) {
    BACK(CameraSelector.LENS_FACING_BACK, "back", "Cam: Back", false),
    FRONT(CameraSelector.LENS_FACING_FRONT, "front", "Cam: Front", true);

    fun toggled(): BenchCameraLens = if (this == BACK) FRONT else BACK

    companion object {
        fun fromLensFacing(lensFacing: Int): BenchCameraLens =
            values().firstOrNull { it.lensFacing == lensFacing } ?: BACK
    }
}

internal class CameraStartGeneration {
    private val generation = AtomicLong(0)

    fun begin(): Long = generation.incrementAndGet()

    fun cancel() {
        generation.incrementAndGet()
    }

    fun isCurrent(candidate: Long): Boolean = generation.get() == candidate
}

/**
 * Binds a selected camera via CameraX and hands every YUV_420_888 frame
 * to `onFrame`. Uses STRATEGY_KEEP_ONLY_LATEST so the pipeline never queues.
 * CameraX does not notify when a frame is dropped; MainActivity estimates
 * skipped-frame counts from arrival-gap statistics.
 */
class CameraAdapter(private val ctx: Context) {

    val executor: ExecutorService = Executors.newSingleThreadExecutor()
    // Skipped-frame counting lives in MainActivity, not here — CameraX's
    // KEEP_ONLY_LATEST never tells us when a frame was dropped, so we
    // estimate from frame-arrival gaps on the consumer side. (See
    // MainActivity.onFrame.)
    var nativeW = 0 ; private set
    var nativeH = 0 ; private set

    private var provider: ProcessCameraProvider? = null
    /** Each start owns a unique generation. A later stop/start can never make
     * an older asynchronous provider listener current again. */
    private val startGeneration = CameraStartGeneration()

    fun start(
        lifecycleOwner: LifecycleOwner,
        previewView: PreviewView,
        lens: BenchCameraLens,
        onError: (Throwable) -> Unit = {},
        onFrame: (proxy: ImageProxy) -> Unit,
    ) {
        val generation = startGeneration.begin()
        nativeW = 0
        nativeH = 0
        val fut = ProcessCameraProvider.getInstance(ctx)
        fut.addListener({
            if (!startGeneration.isCurrent(generation)) return@addListener
            try {
                val p = fut.get()
                if (!startGeneration.isCurrent(generation)) return@addListener
                provider = p
                val selector = CameraSelector.Builder()
                    .requireLensFacing(lens.lensFacing)
                    .build()
                val preview = Preview.Builder().build().also {
                    it.setSurfaceProvider(previewView.surfaceProvider)
                }
                val analysis = ImageAnalysis.Builder()
                    .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                    .setOutputImageFormat(ImageAnalysis.OUTPUT_IMAGE_FORMAT_YUV_420_888)
                    .setTargetResolution(android.util.Size(640, 480))
                    .build()
                analysis.setAnalyzer(executor) { proxy ->
                    if (nativeW == 0) { nativeW = proxy.width ; nativeH = proxy.height }
                    onFrame(proxy)
                }
                p.unbindAll()
                if (!startGeneration.isCurrent(generation)) return@addListener
                p.bindToLifecycle(
                    lifecycleOwner,
                    selector,
                    preview, analysis
                )
            } catch (t: Throwable) {
                if (!startGeneration.isCurrent(generation)) return@addListener
                provider?.unbindAll()
                provider = null
                onError(t)
            }
        }, ContextCompat.getMainExecutor(ctx))
    }

    fun stop() {
        startGeneration.cancel()
        provider?.unbindAll()
        provider = null
    }
}
