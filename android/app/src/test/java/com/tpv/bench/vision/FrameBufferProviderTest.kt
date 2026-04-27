package com.tpv.bench.vision

import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertEquals
import org.junit.Test

class FrameBufferProviderTest {
    @Test
    fun `nv21 is not converted when unused`() {
        var calls = 0
        FrameScopedBufferProvider(nv21Supplier = { calls += 1; ByteArray(4) })
        assertEquals(0, calls)
    }

    @Test
    fun `nv21 conversion is cached within frame`() {
        var calls = 0
        val provider = FrameScopedBufferProvider(nv21Supplier = { calls += 1; byteArrayOf(1, 2, 3) })
        assertSame(provider.nv21(), provider.nv21())
        assertEquals(1, calls)
    }

    @Test
    fun `model input buffers are isolated by dtype`() {
        val provider = FrameScopedBufferProvider(nv21Supplier = { ByteArray(4) })
        val uint8 = provider.modelInput("engine", 8, 8, ModelDType.UINT8)
        val float32 = provider.modelInput("engine", 8, 8, ModelDType.FLOAT32)
        assertNotSame(uint8, float32)
        assertEquals(64, uint8.capacity())
        assertEquals(256, float32.capacity())
    }

    @Test
    fun `model input buffer reuses exact same key`() {
        val provider = FrameScopedBufferProvider(nv21Supplier = { ByteArray(4) })
        val first = provider.modelInput("engine", 8, 8, ModelDType.UINT8)
        val second = provider.modelInput("engine", 8, 8, ModelDType.UINT8)
        assertSame(first, second)
    }

    @Test
    fun `argb conversion is lazy and cached within frame`() {
        var calls = 0
        val provider = FrameScopedBufferProvider(
            nv21Supplier = { ByteArray(4) },
            argbSupplier = { calls += 1; intArrayOf(0xFF000000.toInt()) },
        )

        assertEquals(0, calls)
        assertSame(provider.argb8888(), provider.argb8888())
        assertEquals(1, calls)
    }

    @Test(expected = UnsupportedOperationException::class)
    fun `argb is explicit unsupported without supplier`() {
        FrameScopedBufferProvider(nv21Supplier = { ByteArray(4) }).argb8888()
    }
}
