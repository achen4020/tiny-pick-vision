package com.tpv.bench.vision

import com.tpv.bench.YuvAdapter
import java.nio.ByteBuffer

enum class VisionInputFormat { Y640, NV21, ARGB8888, MODEL_INPUT }

enum class ModelDType(val bytes: Int) {
    UINT8(1),
    FLOAT32(4),
    INT8(1),
}

data class VisionFrame(
    val frameIdxInRun: Long,
    val tCameraArriveNs: Long,
    val nativeW: Int,
    val nativeH: Int,
    val crop: YuvAdapter.CropRect,
    val y640: ByteArray,
    val rotationDegrees: Int,
    val buffers: FrameBufferProvider,
    val tpvTimingNs: LongArray = LongArray(3),
)

interface FrameBufferProvider {
    fun nv21(): ByteArray
    fun argb8888(): IntArray
    fun modelInput(engineId: String, width: Int, height: Int, dtype: ModelDType): ByteBuffer
}

