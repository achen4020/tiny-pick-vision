package com.tpv.bench

import android.content.Context
import androidx.core.content.edit

/**
 * Persistent Settings for the trigger machine. Locked while a run is active
 * (MainActivity enforces this by disabling the button).
 */
class SettingsState(context: Context) {
    private val prefs = context.getSharedPreferences("tpv_bench", Context.MODE_PRIVATE)

    var nStable: Int
        get() = prefs.getInt("n_stable", 3).coerceIn(1, 30)
        set(v) = prefs.edit { putInt("n_stable", v.coerceIn(1, 30)) }

    var kEmpty: Int
        get() = prefs.getInt("k_empty", 5).coerceIn(1, 30)
        set(v) = prefs.edit { putInt("k_empty", v.coerceIn(1, 30)) }

    var mDriftPx: Int
        get() = prefs.getInt("m_drift_px", 30).coerceIn(0, 320)
        set(v) = prefs.edit { putInt("m_drift_px", v.coerceIn(0, 320)) }
}
