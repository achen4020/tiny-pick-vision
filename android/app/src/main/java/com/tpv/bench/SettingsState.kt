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

    // ---- v2 additions (T-v2.6) — same run-locked semantics ----

    /** Binarisation threshold (Y-channel cutoff, 0..255). Default 128. */
    var binThreshold: Int
        get() = prefs.getInt("bin_threshold", 128).coerceIn(0, 255)
        set(v) = prefs.edit { putInt("bin_threshold", v.coerceIn(0, 255)) }

    /**
     * Polarity of the binariser. true → Y < threshold counted as foreground
     * (white-bg, dark object). Default true per spec §5.5 (current
     * acceptance scene).
     */
    var darkObjectMode: Boolean
        get() = prefs.getBoolean("dark_object_mode", true)
        set(v) = prefs.edit { putBoolean("dark_object_mode", v) }

    /** ROI in 640×480 coords. Defaults to full frame (0,0,640,480) — i.e. ROI disabled. */
    var roiX: Int
        get() = prefs.getInt("roi_x", 0).coerceIn(0, 639)
        set(v) = prefs.edit { putInt("roi_x", v.coerceIn(0, 639)) }

    var roiY: Int
        get() = prefs.getInt("roi_y", 0).coerceIn(0, 479)
        set(v) = prefs.edit { putInt("roi_y", v.coerceIn(0, 479)) }

    var roiW: Int
        get() = prefs.getInt("roi_w", 640).coerceIn(1, 640)
        set(v) = prefs.edit { putInt("roi_w", v.coerceIn(1, 640)) }

    var roiH: Int
        get() = prefs.getInt("roi_h", 480).coerceIn(1, 480)
        set(v) = prefs.edit { putInt("roi_h", v.coerceIn(1, 480)) }
}
