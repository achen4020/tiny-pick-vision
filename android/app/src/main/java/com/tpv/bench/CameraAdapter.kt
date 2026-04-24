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

/**
 * Binds a back-facing camera via CameraX and hands every YUV_420_888 frame
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

    fun start(
        lifecycleOwner: LifecycleOwner,
        previewView: PreviewView,
        onFrame: (proxy: ImageProxy) -> Unit,
    ) {
        val fut = ProcessCameraProvider.getInstance(ctx)
        fut.addListener({
            val p = fut.get()
            provider = p
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
                try { onFrame(proxy) } finally { proxy.close() }
            }
            p.unbindAll()
            p.bindToLifecycle(
                lifecycleOwner,
                CameraSelector.DEFAULT_BACK_CAMERA,
                preview, analysis
            )
        }, ContextCompat.getMainExecutor(ctx))
    }

    fun stop() {
        provider?.unbindAll()
        provider = null
    }
}
