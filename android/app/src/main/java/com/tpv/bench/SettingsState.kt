package com.tpv.bench

import android.content.Context
import androidx.core.content.edit

enum class RecognitionMode {
    OBJECT,
    FACE;

    val displayName: String
        get() = when (this) {
            OBJECT -> "Object"
            FACE -> "Face"
        }

    companion object {
        fun fromStored(value: String?): RecognitionMode =
            values().firstOrNull { it.name == value } ?: OBJECT
    }
}

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

    var trackerEnabled: Boolean
        get() = prefs.getBoolean("tracker_enabled", true)
        set(v) = prefs.edit { putBoolean("tracker_enabled", v) }

    var trackerMinHits: Int
        get() = prefs.getInt("tracker_min_hits", 2).coerceIn(1, 30)
        set(v) = prefs.edit { putInt("tracker_min_hits", v.coerceIn(1, 30)) }

    var trackerMaxAge: Int
        get() = prefs.getInt("tracker_max_age", 10).coerceIn(0, 120)
        set(v) = prefs.edit { putInt("tracker_max_age", v.coerceIn(0, 120)) }

    var trackerIouThresholdPct: Int
        get() = prefs.getInt("tracker_iou_threshold_pct", 25).coerceIn(0, 100)
        set(v) = prefs.edit { putInt("tracker_iou_threshold_pct", v.coerceIn(0, 100)) }

    var trackerCenterDistancePx: Int
        get() = prefs.getInt("tracker_center_distance_px", 80).coerceIn(0, 640)
        set(v) = prefs.edit { putInt("tracker_center_distance_px", v.coerceIn(0, 640)) }

    var recognitionMode: RecognitionMode
        get() {
            val stored = prefs.getString("recognition_mode", null)
            if (stored != null) return RecognitionMode.fromStored(stored)
            return if (prefs.getBoolean("face_enabled", false)) {
                RecognitionMode.FACE
            } else {
                RecognitionMode.OBJECT
            }
        }
        set(v) = prefs.edit {
            putString("recognition_mode", v.name)
            putBoolean("face_enabled", v == RecognitionMode.FACE)
        }

    var faceEnabled: Boolean
        get() = recognitionMode == RecognitionMode.FACE
        set(v) {
            recognitionMode = if (v) RecognitionMode.FACE else RecognitionMode.OBJECT
        }

    var faceMinDetectionConfidencePct: Int
        get() = prefs.getInt("face_min_detection_confidence_pct", 50).coerceIn(1, 100)
        set(v) = prefs.edit { putInt("face_min_detection_confidence_pct", v.coerceIn(1, 100)) }

    var faceMinSuppressionThresholdPct: Int
        get() = prefs.getInt("face_min_suppression_threshold_pct", 30).coerceIn(0, 100)
        set(v) = prefs.edit { putInt("face_min_suppression_threshold_pct", v.coerceIn(0, 100)) }
}
