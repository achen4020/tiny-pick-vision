package com.tpv.bench

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CameraStartGenerationTest {
    @Test
    fun `stopped request stays cancelled after a later start`() {
        val generations = CameraStartGeneration()
        val first = generations.begin()

        generations.cancel()
        val second = generations.begin()

        assertFalse(generations.isCurrent(first))
        assertTrue(generations.isCurrent(second))
    }
}
