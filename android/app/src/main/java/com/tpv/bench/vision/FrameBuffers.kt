package com.tpv.bench.vision

import java.nio.ByteBuffer
import java.nio.ByteOrder

class FrameScopedBufferProvider(
    private val nv21Supplier: () -> ByteArray,
    private val argbSupplier: (() -> IntArray)? = null,
) : FrameBufferProvider {
    private var nv21Cache: ByteArray? = null
    private var argbCache: IntArray? = null
    private val modelInputs = HashMap<ModelInputKey, ByteBuffer>()

    override fun nv21(): ByteArray {
        val cached = nv21Cache
        if (cached != null) return cached
        return nv21Supplier().also { nv21Cache = it }
    }

    override fun argb8888(): IntArray {
        val cached = argbCache
        if (cached != null) return cached
        val supplier = argbSupplier ?: throw UnsupportedOperationException("ARGB8888 buffer is not available")
        return supplier().also { argbCache = it }
    }

    override fun modelInput(engineId: String, width: Int, height: Int, dtype: ModelDType): ByteBuffer {
        require(engineId.isNotBlank()) { "engineId must not be blank" }
        require(width > 0 && height > 0) { "width and height must be positive" }
        val key = ModelInputKey(engineId, width, height, dtype)
        return modelInputs.getOrPut(key) {
            ByteBuffer.allocateDirect(width * height * dtype.bytes).order(ByteOrder.nativeOrder())
        }.also { it.clear() }
    }

    private data class ModelInputKey(
        val engineId: String,
        val width: Int,
        val height: Int,
        val dtype: ModelDType,
    )
}

