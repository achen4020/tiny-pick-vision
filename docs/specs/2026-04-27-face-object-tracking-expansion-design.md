# Face Recognition and Object Tracking Expansion Design

Date: 2026-04-27
Status: Accepted
Scope: Extend the Android bench app and vision pipeline from TPV blob detection
to face detection/recognition and generic object tracking.

---

## 0. Review Decisions

These decisions close the P0 feasibility questions raised during review and
are binding for the first implementation plan.

| Topic | Decision | Rationale |
|---|---|---|
| Face runtime | **MediaPipe Tasks Face Detector first** | The target Lenovo TB322FC device class may not have GMS / Google Play Services. MediaPipe Tasks ships in-app and does not depend on Play Services model download. |
| ML Kit usage | ML Kit unbundled is **not allowed** unless a specific deployment device is verified to have GMS. ML Kit bundled may be revisited as an alternative, with APK-size review. | ML Kit unbundled downloads/manages models via Google Play Services. Bundled ML Kit avoids that dependency but still increases APK size and duplicates the MediaPipe option. |
| Frame buffers | `VisionFrame` must not eagerly allocate NV21/RGB every frame. Engines declare required input formats; the pipeline materializes them lazily from reusable buffers. | Prevent per-frame ~hundreds-of-KB allocations and GC pressure. |
| Event trigger | Default v3 event source is a single run-locked `primary_event_engine`. Additional engines provide overlay/tracks but do not commit events unless explicitly configured. | Avoid ambiguous OR/AND semantics across TPV/face/object detections. |
| Tracker milestone | Tracker core can be built before face/object engines, but real multi-detection acceptance starts with Face/Object milestones. | Current TPV v2 only emits one winner, so it cannot validate multi-object tracking by itself. |
| Orientation | First expansion remains **landscape-only**. Portrait support is a separate design because it changes camera/overlay coordinate contracts. | Current overlay correctness relies on the landscape lock and `fillCenter` mapping. |

---

## 1. Executive Summary

The current project is a strong base for **camera ingest, Android JNI
integration, live overlay, event recording, replay, and device acceptance**.
It is not, by itself, a face-recognition or generic object-detection system:
the native TPV pipeline is a 640×480 Y-channel threshold/CCL/shape-feature
classifier optimized for high-contrast pick targets.

The recommended direction is therefore:

1. Keep TPV as one detector backend (`TpvBlobEngine`).
2. Introduce a common `VisionEngine` interface and unified detection model.
3. Add a tracker layer after detection, shared by TPV, face, and object engines.
4. Add MediaPipe Tasks face detection first, then face recognition as a
   separate identity layer.
5. Add generic object detection through an Android ML runtime such as LiteRT
   / TensorFlow Lite or ONNX Runtime Mobile.

This preserves the current app's verified behavior while creating a clean
path for ML-based capabilities.

---

## 2. Goals

- Support multiple vision engines in one camera pipeline:
  - `tpv_blob`: existing TPV threshold/CCL/mask detector.
  - `face`: face bbox + landmarks + optional tracking ID.
  - `object`: generic object bbox + class + confidence.
- Add cross-frame tracking with stable `track_id` values.
- Extend overlay, diagnostics, run recording, and replay to handle multiple
  detections per frame/event.
- Keep TPV production path behavior unchanged.
- Keep per-frame latency measurable and bounded.
- Treat face recognition data as sensitive biometric data.

---

## 3. Non-Goals for the First Expansion

- Do not rewrite the TPV native C pipeline into a neural-network runtime.
- Do not attempt identity recognition using only face detection.
- Do not persist raw face embeddings by default.
- Do not solve liveness / anti-spoofing in the first recognition milestone.
- Do not require cloud inference.
- Do not introduce portrait orientation in this expansion.
- Do not use Google Play Services-dependent model delivery in the first face
  milestone.

---

## 4. Current System Baseline

### 4.1 Camera and Frame Pipeline

Current hot path:

```text
CameraX ImageProxy
  → YuvAdapter extracts 640×480 Y
  → TpvNative.processFrameDebugV2(...)
  → TriggerMachine
  → RunRecorder
  → OverlayView / DiagnosticsView
```

Primary code entry points:

| Area | Current file | Notes |
|---|---|---|
| Camera binding | `android/app/src/main/java/com/tpv/bench/CameraAdapter.kt` | CameraX back camera, `STRATEGY_KEEP_ONLY_LATEST`, target 640×480 |
| Frame loop | `android/app/src/main/java/com/tpv/bench/MainActivity.kt` | `onFrame()` owns Y extraction, JNI call, trigger, recorder, overlay |
| Native bridge | `android/app/src/main/java/com/tpv/bench/TpvNative.kt` | `processFrameDebugV2()` returns TPV debug result and masks |
| Trigger | `android/app/src/main/java/com/tpv/bench/TriggerMachine.kt` | PRESENT/EMPTY/DROP state machine with stable-window commit |
| Recording | `android/app/src/main/java/com/tpv/bench/RunRecorder.kt` | `meta.json`, `log.jsonl`, `.y`, `.jpg`, `.mask`, `timing.bin` |
| Overlay | `android/app/src/main/java/com/tpv/bench/OverlayView.kt` | TPV mask + ROI + center/axis, `fillCenter` mapping |
| Native TPV API | `include/tpv.h` / `include/tpv_internal.h` | `tpv_process_frame`, debug v1/v2 APIs |

### 4.2 Important Existing Review Gates

These must remain true while expanding:

- `TriggerMachine` must commit on the first PRESENT frame when `N_stable == 1`.
- Overlay must not render zero-filled detections for `TPV_EMPTY`,
  `TPV_SCENE_ERROR`, or `TPV_BAD_INPUT`.
- `CameraAdapter` async binding must remain cancellation-safe.
- Activity is landscape-locked; if portrait is enabled later, rotation degrees
  must be threaded into overlay mapping and documentation.

---

## 5. Target Architecture

### 5.1 High-Level Pipeline

```text
CameraX ImageProxy
  ├─ Y plane  → TpvBlobEngine
  ├─ ImageProxy / lazy RGB → FaceEngine
  ├─ ImageProxy / lazy RGB → ObjectEngine
  └─ Timing capture

List<VisionDetection>
  → MultiObjectTracker
  → Primary-engine EventPolicy
  → OverlayView
  → RunRecorder
  → Replay / analysis
```

### 5.2 Engine Boundary

Introduce a common Android-side interface:

```kotlin
interface VisionEngine {
    val id: String
    val metadata: VisionEngineMetadata

    fun process(frame: VisionFrame, timing: VisionTimingSink): List<VisionDetection>

    fun close()
}
```

`VisionFrame` exposes only cheap/common fields eagerly. Expensive derived
formats such as NV21/RGB must be materialized lazily by `FrameBufferProvider`
using reusable buffers.

```kotlin
data class VisionFrame(
    val frameIdxInRun: Long,
    val tCameraArriveNs: Long,
    val nativeW: Int,
    val nativeH: Int,
    val crop: YuvAdapter.CropRect,
    val y640: ByteArray,
    val rotationDegrees: Int,
    val buffers: FrameBufferProvider,
)
```

```kotlin
interface FrameBufferProvider {
    fun imageProxy(): ImageProxyView
    fun nv21(): ByteArray
    fun rgbBitmap(): Bitmap
    fun modelInput(engineId: String, width: Int, height: Int): ByteBuffer
}
```

Rules:

- `y640` is always available because TPV needs it and current code already
  keeps a reusable Y scratch buffer.
- NV21/RGB/model input buffers are created only if an enabled engine requests
  them.
- `FrameBufferProvider` owns buffer reuse; engines must not allocate a fresh
  640×480×format array every frame.
- Overlay JPEG conversion should be demand-driven on commit, still before the
  `ImageProxy` is closed, instead of an unconditional per-frame conversion.
- Engine implementations must declare their required input format in metadata
  so performance impact is visible at review time.

The initial implementation should keep `rotationDegrees = 0` under the current
landscape lock. If the lock is removed, all engines and overlay mapping must
consume the real `ImageProxy.imageInfo.rotationDegrees`.

### 5.3 Event Policy

Multi-engine detection does not imply multi-engine event commits. The default
policy is:

```kotlin
data class EventPolicyConfig(
    val primaryEventEngine: String,     // default: "tpv_blob"
    val enabledCommitEngines: Set<String>, // PRIMARY_ONLY: must equal setOf(primaryEventEngine)
    val mode: CommitMode,               // PRIMARY_ONLY in MVP
)
```

MVP behavior:

- `primaryEventEngine = "tpv_blob"` by default, preserving current TPV event
  semantics.
- In `PRIMARY_ONLY` mode, `enabledCommitEngines` must be exactly
  `{primaryEventEngine}`. Empty sets, unknown engine IDs, or extra engine IDs
  are configuration errors.
- Face/object detections are rendered and tracked but do not create commits
  unless selected as the primary event engine for the run.
- Independent per-engine commit can be added later by running one
  `TriggerMachine` per `(engineId, className)` and writing engine-qualified
  event IDs.
- OR/AND fusion across engines is explicitly out of scope for the first
  implementation because it makes event semantics ambiguous.

### 5.4 Unified Detection Contract

```kotlin
data class VisionDetection(
    val engineId: String,              // "tpv_blob", "face", "object"
    val detectionId: Long,
    val frameIdxInRun: Long,
    val classId: Int,
    val className: String,
    val score: Float,
    val bbox640: RectI,                // canonical 640×480 coordinate system
    val mask: ByteArray? = null,       // TPV mask or segmentation mask, 38400 B if 640×480 packed
    val landmarks640: List<PointI> = emptyList(),
    val pose: VisionPose? = null,
    val rawStatus: Int = 0,
    val attributes: Map<String, String> = emptyMap(),
)
```

Canonical coordinate rule:

- Internally store `bbox640` / landmarks in the existing 640×480 TPV coordinate
  space.
- Convert ML model output from native/RGB/model input coordinates to 640×480 at
  engine boundary.
- Overlay converts 640×480 → native crop → `PreviewView fillCenter` view space,
  reusing the corrected mapping logic.

### 5.5 Input Allocation Policy

The pipeline must treat derived image buffers as pooled resources:

| Buffer | Size at 640×480 | Allocation policy |
|---|---:|---|
| Y 640×480 | 307,200 B | Existing reusable scratch / run buffer |
| NV21 640×480 | ~460,800 B | Lazy + pooled; never allocate per frame unconditionally |
| ARGB 640×480 | ~1,228,800 B | Lazy + pooled; avoid unless engine/runtime requires it |
| Model input | model-dependent | One reusable direct buffer per enabled engine/input size |

Every engine must declare `requiredInputs` in `VisionEngineMetadata` so enabling
it has an explicit memory and latency cost.

Pool ownership rule:

- Use one shared reusable buffer per raw canonical format (`NV21_640x480`,
  `ARGB_640x480`) per frame pipeline instance.
- Use one reusable direct model-input buffer per `(engineId, width, height,
  dtype)` because models may require different layout/normalization.
- If two engines require the same raw ARGB/NV21 view, they share the raw buffer;
  each still owns its model-input transform buffer.

---

## 6. Tracking Layer

### 6.1 Recommended MVP: Detection-by-Tracking

Use a SORT-like tracker first:

1. Predict active tracks with constant-velocity state.
2. Match current detections to tracks using IoU and center distance.
3. Confirm a track after `min_hits` consecutive matches.
4. Keep unmatched tracks for `max_age` frames.
5. Emit stable `track_id` per active track.

This is simple, explainable, testable, and engine-agnostic.

### 6.2 Tracker Input

```kotlin
data class TrackableDetection(
    val detection: VisionDetection,
    val matchClass: String,            // engine + className bucket
    val bbox640: RectI,
    val score: Float,
)
```

### 6.3 Tracker Output

```kotlin
data class TrackedDetection(
    val detection: VisionDetection,
    val trackId: Long,
    val trackState: TrackState,        // Tentative / Confirmed / Lost
    val ageFrames: Int,
    val hits: Int,
    val misses: Int,
)
```

### 6.4 Matching Policy

Default matching thresholds:

| Detection type | IoU threshold | Center distance threshold | Notes |
|---|---:|---:|---|
| TPV blob | 0.20 | 60 px | Masks can vary; center is stable |
| Face | 0.30 | 50 px | Face boxes are usually stable |
| Object | 0.25 | 80 px | Generic objects vary by model |

The thresholds should be settings-backed but run-locked, just like TPV
threshold/ROI.

### 6.5 Why Not Optical-Flow Tracking First?

Optical-flow/KCF/CSRT-style tracking can be useful, but it increases
implementation and debugging cost. Detection-by-tracking gives stable IDs
using the detectors we already need. Optical flow can be added later to bridge
detector throttling gaps if necessary.

---

## 7. Face Capability

### 7.1 Face Detection

Recommended MVP:

- Android-side `MediaPipeFaceEngine` based on MediaPipe Tasks Face Detector.
- Input: `ImageProxy` or lazily materialized model input, depending on the
  final MediaPipe Tasks API shape.
- Output: bbox + landmarks + confidence + optional face tracking ID.
- Overlay: rectangle, landmarks, `track_id`.

ML Kit Face Detection detects faces and can provide tracking IDs, but it does
**not** identify who the person is. ML Kit unbundled is not suitable for the
first target device class because it depends on Google Play Services model
delivery. ML Kit bundled may be considered only after an APK-size review.
MediaPipe Tasks is the default first implementation because it ships with the
app and avoids the GMS dependency.

### 7.2 Face Recognition

Face recognition must be a separate layer:

```text
Face bbox
  → crop
  → landmark alignment
  → embedding model
  → normalized embedding
  → gallery cosine similarity
  → identity / unknown
```

Required components:

- Enrollment flow.
- Local gallery store.
- Embedding threshold calibration.
- Identity confidence output.
- Secure storage and deletion.
- Explicit user consent.

### 7.3 Biometric Data Rules

Default behavior:

- Do not write embeddings to `run.zip`.
- Do not write recognized identity to artifacts unless explicitly enabled.
- Do not write face landmarks to artifacts by default. Landmarks are treated as
  biometric/PII because eye/nose/mouth geometry can contribute to
  re-identification.
- Do not write face crops by default.
- Store gallery embeddings encrypted at rest.
- Provide deletion/reset of face gallery.
- Mark face-recognition runs in `meta.json`.

Encryption requirement:

- Use Android Keystore-backed AES-GCM for local gallery encryption.
- Preferred implementation path is Jetpack Security / Tink with a
  Keystore-backed master key. A direct Android Keystore AES-GCM implementation
  is acceptable only if IV generation, auth tag verification, and key rotation
  are covered by tests.

### 7.4 Face Recognition Acceptance

Face recognition cannot pass based on a visual smoke test. It needs:

- Same-person true accept rate at selected threshold.
- Different-person false accept rate.
- Unknown-person rejection behavior.
- Lighting and angle test set.
- Enrollment/update/delete flow tests.

---

## 8. Generic Object Detection

### 8.1 Recommended Runtime Options

| Runtime | Strength | Tradeoff |
|---|---|---|
| LiteRT / TensorFlow Lite | Android-native path, delegates, broad examples | Model conversion and delegate compatibility must be managed |
| ONNX Runtime Mobile | Good ONNX model compatibility | APK size and performance tuning need review |
| MediaPipe Tasks | Higher-level task APIs | Less flexible for custom post-processing |

Recommendation: start with LiteRT/TFLite for Android MVP unless there is a
specific ONNX model requirement.

### 8.2 Object Engine Contract

`ObjectEngine` should output:

- `classId`
- `className`
- `score`
- `bbox640`
- optional coarse mask if using an instance-segmentation model later

First milestone should be bbox-only. Instance segmentation can come later.

### 8.3 Model Selection Criteria

Review candidates by:

- Target classes.
- Input size.
- CPU p50/p95 latency.
- GPU/NNAPI delegate compatibility.
- APK size impact.
- License.
- Quantized vs floating-point accuracy.
- Ease of replay reproducibility.

### 8.4 Object Detection Acceptance

- Stable bbox over 30 seconds on static object.
- Track ID remains stable under small movement.
- No TPV overlay/regression.
- Per-engine timing logged.
- Replay can parse recorded detections.

---

## 9. Android App Changes

### 9.1 MainActivity Refactor

Current `MainActivity.onFrame()` does all pipeline work inline. Split it:

```text
onFrame()
  → build VisionFrame
  → VisionPipeline.process(frame)
  → TriggerPolicy.update(...)
  → Recorder.record(...)
  → Overlay.update(...)
```

Suggested new files:

```text
android/app/src/main/java/com/tpv/bench/vision/
  VisionFrame.kt
  VisionDetection.kt
  VisionEngine.kt
  VisionPipeline.kt
  VisionTiming.kt
  TpvBlobEngine.kt
  FaceEngine.kt
  ObjectEngine.kt
  MultiObjectTracker.kt
  Track.kt
```

### 9.2 Settings

Add run-locked settings:

The current settings dialog should not absorb all new fields in one long
single-column form. Use tabs or separate pages:

```text
Settings
  ├─ Pipeline
  │   ├─ Enabled engines: TPV / Face / Object
  │   ├─ Primary event engine: TPV / Face / Object
  │   └─ Per-engine throttle FPS
  ├─ Tracker
  │   ├─ min_hits
  │   ├─ max_age
  │   ├─ IoU threshold
  │   └─ center-distance threshold
  ├─ ML Runtime
  │   ├─ Face runtime: MediaPipe Tasks
  │   ├─ Object runtime: LiteRT / ONNX Runtime
  │   └─ Delegate: CPU / GPU / NNAPI where applicable
  └─ Privacy
      ├─ record face landmarks: off by default
      ├─ record face crops: off by default
      ├─ record face identity: off by default
      └─ reset face gallery
```

All settings remain run-locked: Stop → edit → Start.

### 9.3 Overlay

Overlay should support multiple layers:

| Layer | Default |
|---|---|
| TPV mask | on |
| TPV center/axis | on |
| Face bbox/landmarks | on when face engine enabled |
| Object bbox/class/score | on when object engine enabled |
| Track ID | on |
| Debug labels | toggle |

The overlay must continue to skip invalid detections and must preserve
`PreviewView fillCenter` alignment.

Face landmarks may be rendered live, but persisted overlays/artifacts must
respect the Privacy tab settings.

---

## 10. Data Format Changes

### 10.1 `meta.json`

Add:

```json
{
  "vision": {
    "schema_version": 1,
    "engines": [
      {
        "id": "tpv_blob",
        "type": "native_c",
        "version": "v2",
        "model_sha256": "...",
        "provider_version": null,
        "params": {
          "bin_threshold": 120,
          "dark_object_mode": true,
          "roi": {"x": 0, "y": 0, "w": 640, "h": 480}
        },
        "enabled": true
      },
      {
        "id": "face",
        "type": "mediapipe_tasks",
        "model_sha256": null,
        "provider_version": "mediapipe-tasks-<version>",
        "model": "face_detector",
        "enabled": false
      },
      {
        "id": "object",
        "type": "litert",
        "model_sha256": "...",
        "provider_version": "litert-<version>",
        "input_size": [320, 320],
        "enabled": false
      }
    ],
    "tracker": {
      "type": "sort_like",
      "min_hits": 3,
      "max_age": 10
    }
  }
}
```

Source-of-truth rule:

- For v3+ runs, `vision.engines[]` is the source of truth.
- Existing top-level `tpv.*` metadata remains a deprecated compatibility mirror
  for v2 replay and tooling.
- Writers must derive the compatibility `tpv.*` fields from
  `vision.engines[id="tpv_blob"].params`.
- Readers should prefer `vision.engines[]` when present and fall back to
  `tpv.*` for v2/legacy runs.
- Tests must assert the mirror fields do not drift.

`model_sha256` is nullable because some managed-provider runtimes do not expose
a stable model file hash to the app. In those cases, write `provider_version`
and runtime-specific model identifier instead.

### 10.2 `log.jsonl`

Add per-frame/event detections:

```json
{
  "tracks": [
    {
      "track_id": 7,
      "engine": "object",
      "class_name": "mouse",
      "score": 0.91,
      "bbox": {"x": 240, "y": 140, "w": 120, "h": 80},
      "state": "confirmed"
    }
  ],
  "detections": [
    {
      "engine": "face",
      "class_name": "face",
      "score": 0.98,
      "bbox": {"x": 120, "y": 80, "w": 90, "h": 110}
    }
  ]
}
```

Do not write face embeddings, real identity, face crops, or landmarks by
default. If a privacy setting enables landmarks, the field may be added:

```json
"landmarks": [{"name": "left_eye", "x": 145, "y": 120}]
```

### 10.3 Artifacts

Retain:

- `NNNNNN.y`
- `NNNNNN.jpg`
- `NNNNNN.mask`

Optional future additions:

- `NNNNNN.detections.json`
- `NNNNNN.tracks.json`
- `NNNNNN.face_crop_<id>.jpg` only when explicitly enabled

---

## 11. Performance Plan

### 11.1 Timing Records

Current timing records measure camera/JNI/TPV. Extend timing with per-engine
records:

```text
frame_idx
engine_id
t_enter_ns
t_exit_ns
status
num_detections
```

Keep `timing.bin` backward-compatible by either:

1. adding a `timing_v2.bin`, or
2. versioning the record header and retaining v1 parser support.

Recommendation: add `timing_v2.bin` for the expansion; leave existing
`timing.bin` untouched.

### 11.2 Runtime Budget

Initial targets on Lenovo TB322FC-class device:

| Component | Target |
|---|---:|
| TPV engine | current p95 class, no regression |
| Tracker | p95 ≤ 1 ms |
| Face detection | p95 ≤ 50 ms when enabled |
| Object detection | p95 ≤ 80 ms when enabled |
| UI overlay | no visible jank |

Run all detectors with throttling:

- TPV: every frame.
- Face detection: 10–15 FPS.
- Object detection: 5–10 FPS.
- Tracker: every frame using latest detector results.

---

## 12. Security and Privacy

Face recognition changes the risk class of the app.

Required review items:

- Explicit user consent before enrollment.
- Local-only processing by default.
- Encrypt gallery embeddings with Android Keystore-backed AES-GCM.
- Provide delete/reset gallery action.
- Do not export embeddings in run zip by default.
- Do not export recognized identities by default.
- Do not export face landmarks or face crops by default.
- Mark face-recognition enabled runs in metadata.
- Document model provenance and license.

If cloud inference is introduced later, it requires a separate privacy,
security, and network design review.

---

## 13. Milestones

### M1 — Vision Abstraction Without New ML

Deliverables:

- `VisionEngine`, `VisionFrame`, `VisionDetection`.
- `TpvBlobEngine` wrapping current `TpvNative.processFrameDebugV2`.
- `MultiObjectTracker` noop placeholder that passes detections through without
  assigning stable IDs, keeping the M1 pipeline shape compatible with M2.
- `VisionPipeline` called by `MainActivity.onFrame()`.
- No behavioral regression in TPV overlay, trigger, recorder.

Validation:

- Existing Android unit tests pass.
- Existing C tests pass.
- v2 replay parity remains valid.
- Live overlay remains aligned.

### M2 — Tracker Core and Synthetic Harness

Deliverables:

- `MultiObjectTracker` core.
- Synthetic sequence test harness with generated `VisionDetection` streams.
- Track ID overlay for current TPV single-winner detections.
- Track metadata in `log.jsonl` for the primary event engine.
- JVM tests for matching, lost tracks, re-identification window.

Validation:

- Synthetic two-object crossing sequence has bounded ID switches.
- Static TPV winner keeps one stable ID for 30 seconds.
- Slow TPV winner motion does not cause repeated ID switches.
- Track TTL expires after configured missing frames.

Note: Current TPV v2 emits one winner, not top-N blobs. M2 validates tracker
state mechanics, but real multi-detection acceptance starts in M3/M4.

### M3 — Face Detection

Deliverables:

- `FaceEngine`.
- Integration with `MultiObjectTracker` from M2 for face tracks.
- Face bbox/landmarks overlay.
- Per-engine timing.
- Settings toggle.

Validation:

- Real-time face bbox appears and follows face movement.
- Multiple faces receive independent stable track IDs.
- Disabled face engine has zero runtime overhead except settings checks.
- No face data is exported unless enabled.

### M4 — Generic Object Detection

Deliverables:

- `ObjectEngine`.
- One selected object detector model.
- Object class/score overlay.
- Tracker integration.

Validation:

- Static object bbox stable.
- Track ID stable under small movement.
- Timing within configured budget.

### M5 — Face Recognition

Deliverables:

- Enrollment flow.
- Local gallery.
- Embedding inference.
- Identity/unknown decision.
- Privacy controls.

Validation:

- Same-person / different-person evaluation set.
- False accept / false reject report.
- Gallery delete/reset verified.

---

## 14. Acceptance Criteria

| Area | Criterion |
|---|---|
| Backward compatibility | TPV-only mode behaves like current v2 |
| Overlay | All boxes/masks align under landscape `fillCenter` |
| Tracker | Confirmed track IDs stable under slow motion |
| Face detection | Face bbox appears with engine enabled and disappears when disabled |
| Object detection | Object bbox/class/score appears with engine enabled |
| Recording | `meta.json` declares active engines, model hashes when available, provider versions otherwise |
| Replay | Replay can parse old TPV runs and new multi-engine runs |
| Performance | Per-engine timing logged; UI remains responsive |
| Privacy | Face embeddings, identity, landmarks, and crops are not exported by default |

---

## 15. Risks and Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| ML runtime latency | UI jank, frame drops | Engine throttling, KEEP_ONLY_LATEST, per-engine timing |
| Coordinate mismatch | Wrong overlay | Canonical 640×480 coordinate contract and fillCenter tests |
| Face detection confused with recognition | Product misunderstanding | Separate milestones and data contracts |
| Embedding leakage | Privacy/security issue | Do not export by default; encrypted gallery |
| APK size growth | Install/runtime overhead | Review model size and runtime dependency size before M4 |
| Model distribution | Large models can block APK/channel distribution, especially on non-Play-Store devices | Track model size, runtime AAR size, ABI splits, and optional asset delivery strategy before selecting models |
| Tracker ID switches | Poor UX | Dedicated tracker tests and acceptance runs |
| Model license mismatch | Release blocker | Model provenance checklist |
| GMS dependency | Face engine may fail on GMS-less Lenovo/China devices | Use MediaPipe Tasks first; ML Kit unbundled requires explicit device verification |
| Landscape-only UX | Face use cases often expect portrait | Keep v3 landscape-only; portrait requires separate rotation/overlay spec |
| Face landmarks leakage | Landmarks can be biometric/PII | Live-only by default; persist only behind privacy setting |

---

## 16. Open Questions

1. Which object classes are required first?
2. Is object detection expected to run always, or only during a configured mode?
3. Do we need identity recognition on-device only, or is cloud ever allowed?
4. What false-accept rate is acceptable for face recognition?
5. Should replay reproduce ML detections bit-identically, or only parse recorded
   model outputs?
6. Do we need multi-camera support later?
7. Should TPV eventually expose top-N blobs for multi-object tracker testing,
   or remain a single-winner engine?

---

## 17. Reference Links

- ML Kit Face Detection: <https://developers.google.com/ml-kit/vision/face-detection>
- ML Kit Face Detection Android guide: <https://developers.google.com/ml-kit/vision/face-detection/android>
- MediaPipe Face Detector Android: <https://ai.google.dev/edge/mediapipe/solutions/vision/face_detector/android>
- LiteRT Android quickstart: <https://ai.google.dev/edge/litert/android/quickstart>
- ONNX Runtime Mobile tutorials: <https://onnxruntime.ai/docs/tutorials/mobile/>
