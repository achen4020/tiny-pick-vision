# Face/Object Tracking Expansion — M1/M2 Implementation Plan

> **SDK delivery note:** This plan is useful for Android bench-app scaffolding,
> but it is not the final third-party SDK architecture. Product face/object/
> tracking functionality must follow
> `docs/plans/2026-04-27-c-first-vision-sdk-implementation-plan.md` and be
> exposed through `libtpv.so` C ABI. Kotlin `VisionEngine` code is prototype/demo
> infrastructure until C parity exists.

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the Android bench app from a TPV-only inline pipeline into a
multi-engine vision pipeline with a unified detection contract and a tracker
core, without introducing face/object ML dependencies yet. M1 preserves current
TPV behavior behind the new abstractions. M2 adds a SORT-like tracker core,
synthetic tracker tests, TPV single-winner track IDs, and track metadata.

**Scope:** M1/M2 only. Face detection, face recognition, and generic object
detector runtime integration are explicitly deferred to later plans. This keeps
the first implementation dependency-free and validates the architecture before
adding MediaPipe/LiteRT/ONNX Runtime.

**Source of truth:** `docs/specs/2026-04-27-face-object-tracking-expansion-design.md`.

**Tech Stack:** Existing Android stack only — Kotlin 1.9.22, AGP 8.5,
CameraX 1.3.4, JUnit4, current NDK/native TPV setup. No new Gradle dependency
in M1/M2.

---

## 0. Review Gates

Do not violate existing Android bench app gates:

- `TriggerMachine` must still commit on the first PRESENT frame when
  `N_stable == 1`.
- Live overlay rendering must still skip invalid TPV detections for
  `TPV_EMPTY`, `TPV_SCENE_ERROR`, and `TPV_BAD_INPUT`.
- `CameraAdapter` async binding cancellation safety must remain unchanged.
- App remains landscape-only; overlay must preserve `PreviewView fillCenter`
  mapping.

Additional M1/M2 gates:

- No eager per-frame NV21/RGB allocation.
- TPV-only mode remains behaviorally equivalent to current v2.
- `tpv.*` metadata remains a compatibility mirror; `vision.engines[]` is the
  v3+ source of truth when present.
- `PRIMARY_ONLY` event policy is the only implemented commit mode.
- M2 tracker tests use synthetic detection streams; TPV remains a single-winner
  smoke path until Face/Object engines exist.

---

## 1. File Structure

### New vision package

```text
android/app/src/main/java/com/tpv/bench/vision/
  Geometry.kt                    NEW — RectI, PointI, geometry helpers
  VisionDetection.kt             NEW — unified detection + tracked detection types
  VisionFrame.kt                 NEW — frame contract + FrameBufferProvider
  VisionEngine.kt                NEW — engine interface + metadata
  VisionTiming.kt                NEW — timing sink interfaces/data
  EventPolicy.kt                 NEW — PRIMARY_ONLY event source selection
  VisionPipeline.kt              NEW — engines → tracker → event policy bundle
  TpvBlobEngine.kt               NEW — wraps TpvNative.processFrameDebugV2
  MultiObjectTracker.kt          NEW — M1 noop placeholder, M2 real tracker
```

### Existing Android files

```text
android/app/src/main/java/com/tpv/bench/
  MainActivity.kt                MODIFY — delegate frame processing to VisionPipeline
  RunRecorder.kt                 MODIFY — add vision meta and optional tracks[] json
  OverlayView.kt                 MODIFY — accept/display track IDs without regressing TPV mask
  OverlayPainter.kt              MODIFY — pure geometry helpers if needed
  SettingsState.kt               MODIFY — primaryEventEngine + tracker params, run-locked
  TriggerMachine.kt              MODIFY? — avoid behavior change; adapters may wrap it
  Yuv420ToNv21.kt                UNCHANGED or reused lazily by FrameBufferProvider
```

### Android tests

```text
android/app/src/test/java/com/tpv/bench/vision/
  GeometryTest.kt                NEW
  EventPolicyTest.kt             NEW
  TpvBlobEngineTest.kt           NEW or JVM adapter test with fake native result
  FrameBufferProviderTest.kt     NEW — verifies lazy allocation/cache semantics
  MultiObjectTrackerTest.kt      NEW — M2 synthetic sequences

android/app/src/test/java/com/tpv/bench/
  TriggerMachineTest.kt          EXISTING — must remain green
  RunRecorderTest.kt             MODIFY — vision meta / tracks[] assertions
  OverlayPainterTest.kt          MODIFY if geometry helpers move
```

### Docs

```text
docs/DEVELOPER.md                MODIFY — add v3 M1/M2 architecture note
```

No native C file changes are required for M1/M2.

---

## 2. Shared Type Contract

### 2.1 Geometry

```kotlin
data class RectI(val x: Int, val y: Int, val w: Int, val h: Int) {
    val x1Inclusive: Int get() = x + w - 1
    val y1Inclusive: Int get() = y + h - 1
    val area: Int get() = if (w <= 0 || h <= 0) 0 else w * h
}

data class PointI(val x: Int, val y: Int)
```

Rules:

- `RectI` uses `{x, y, w, h}` to match `TpvBbox` and jsonl.
- Coordinates are canonical 640×480 unless a type name explicitly says
  native/model/view.
- Reject or clamp invalid rectangles at engine boundaries, not in tracker.

### 2.2 Vision Detection

```kotlin
data class VisionDetection(
    val engineId: String,
    val detectionId: Long,
    val frameIdxInRun: Long,
    val classId: Int,
    val className: String,
    val score: Float,
    val bbox640: RectI,
    val mask: ByteArray? = null,
    val landmarks640: List<PointI> = emptyList(),
    val rawStatus: Int = 0,
    val attributes: Map<String, String> = emptyMap(),
)
```

M1 TPV mapping:

| TPV field | `VisionDetection` field |
|---|---|
| engine | `engineId = "tpv_blob"` |
| `det.classId` | `classId` |
| class name | `"tpv_$classId"` for 0..4, `"tpv_rejected"` for 255, `"tpv_ambiguous"` for 254 |
| confidence | `score = confidence_q8 / 255f` for accepted; `0f` for rejected/ambiguous |
| `bbox` | `bbox640` |
| `mask` | `mask` |
| status | `rawStatus` |

### 2.3 Tracked Detection

```kotlin
enum class TrackState { TENTATIVE, CONFIRMED, LOST }

data class TrackedDetection(
    val detection: VisionDetection,
    val trackId: Long,
    val state: TrackState,
    val ageFrames: Int,
    val hits: Int,
    val misses: Int,
)
```

M1 noop tracker:

- Emits one `TrackedDetection` per input detection.
- Uses `trackId = detection.detectionId`.
- Uses `state = CONFIRMED`.
- Does not persist track state across frames.

M2 real tracker replaces this implementation behind the same public API.

### 2.4 Vision Frame and Lazy Buffers

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

interface FrameBufferProvider {
    fun nv21(): ByteArray
    fun argb8888(): IntArray
    fun modelInput(engineId: String, width: Int, height: Int, dtype: ModelDType): ByteBuffer
}
```

M1 requirements:

- `MainActivity.onFrame()` must stop converting NV21 unconditionally.
- `nv21()` is called only when a commit needs overlay JPEG rendering.
- `argb8888()` and `modelInput()` may throw `UnsupportedOperationException`
  until Face/Object engines are implemented, but their allocation policy must
  be unit-tested with fakes.

### 2.5 Engine Metadata

```kotlin
data class VisionEngineMetadata(
    val id: String,
    val type: String,
    val version: String,
    val modelSha256: String?,
    val providerVersion: String?,
    val requiredInputs: Set<VisionInputFormat>,
    val enabled: Boolean,
)
```

M1 TPV metadata:

- `id = "tpv_blob"`
- `type = "native_c"`
- `version = "v2"`
- `modelSha256 = modelDataSha256`
- `providerVersion = null`
- `requiredInputs = setOf(Y640)`

### 2.6 Event Policy

```kotlin
enum class CommitMode { PRIMARY_ONLY }

data class EventPolicyConfig(
    val primaryEventEngine: String,
    val enabledCommitEngines: Set<String>,
    val mode: CommitMode,
)
```

Validation:

- M1/M2 only support `PRIMARY_ONLY`.
- `enabledCommitEngines` must equal `setOf(primaryEventEngine)`.
- Unknown engine IDs are configuration errors.
- Default `primaryEventEngine = "tpv_blob"` preserves current behavior.

---

## 3. Task M1.1 — Add Vision Core Types

**Goal:** Add pure Kotlin type contracts without changing runtime behavior.

Files:

- Create `android/app/src/main/java/com/tpv/bench/vision/Geometry.kt`
- Create `android/app/src/main/java/com/tpv/bench/vision/VisionDetection.kt`
- Create `android/app/src/main/java/com/tpv/bench/vision/VisionFrame.kt`
- Create `android/app/src/main/java/com/tpv/bench/vision/VisionEngine.kt`
- Create `android/app/src/main/java/com/tpv/bench/vision/VisionTiming.kt`

Steps:

- [ ] Define `RectI`, `PointI`, IoU, center distance, and clamp helpers.
- [ ] Define `VisionDetection`, `TrackedDetection`, `TrackState`.
- [ ] Define `VisionFrame`, `FrameBufferProvider`, `VisionInputFormat`,
      `ModelDType`.
- [ ] Define `VisionEngine` and `VisionEngineMetadata`.
- [ ] Keep all new files Android-light where possible so JVM tests can cover
      them.

Tests:

- [ ] Add `GeometryTest` for area, invalid rect, IoU, center distance.
- [ ] Add equality/hash tests only where arrays are involved; avoid brittle
      identity checks.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.vision.GeometryTest"
```

---

## 4. Task M1.2 — Add EventPolicy PRIMARY_ONLY

**Goal:** Make commit source explicit before adding extra engines.

Files:

- Create `android/app/src/main/java/com/tpv/bench/vision/EventPolicy.kt`
- Create `android/app/src/test/java/com/tpv/bench/vision/EventPolicyTest.kt`

Steps:

- [ ] Implement `CommitMode.PRIMARY_ONLY`.
- [ ] Validate `enabledCommitEngines == setOf(primaryEventEngine)`.
- [ ] Validate primary engine exists in `VisionPipeline` engine metadata.
- [ ] Add helper to filter detections/tracked detections to the primary engine.

Tests:

- [ ] Accept default `tpv_blob` config.
- [ ] Reject empty commit set.
- [ ] Reject extra engine IDs in `PRIMARY_ONLY`.
- [ ] Reject unknown primary engine ID.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.vision.EventPolicyTest"
```

---

## 5. Task M1.3 — Add FrameBufferProvider With Lazy NV21

**Goal:** Remove unconditional per-frame NV21 conversion while preserving commit
JPEG generation.

Files:

- Create `android/app/src/main/java/com/tpv/bench/vision/FrameBuffers.kt`
- Create `android/app/src/test/java/com/tpv/bench/vision/FrameBufferProviderTest.kt`
- Modify `android/app/src/main/java/com/tpv/bench/MainActivity.kt`

Steps:

- [ ] Implement a frame-scoped provider that can lazily call
      `Yuv420ToNv21.convert(proxy)`.
- [ ] Cache the result inside the provider so two consumers do not convert twice.
- [ ] Keep provider usage within `onFrame()` before `proxy.close()`.
- [ ] Move current unconditional `val nv21 = Yuv420ToNv21.convert(proxy)` into
      the commit branch by calling `frame.buffers.nv21()`.
- [ ] Make unsupported ARGB/model input methods explicit stubs for M1.

Tests:

- [ ] Fake provider converts NV21 zero times when no commit occurs.
- [ ] Fake provider converts NV21 once when two consumers request it.
- [ ] Model-input pool key isolation: two fake engines requesting same
      `(width, height)` but different `dtype` receive different buffers.
- [ ] Commit branch still receives NV21 bytes for `renderOverlayJpeg`.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.vision.FrameBufferProviderTest"
./gradlew :app:testDebugUnitTest
```

---

## 6. Task M1.4 — Wrap TPV as TpvBlobEngine

**Goal:** Move the TPV JNI call behind the new `VisionEngine` contract.

Files:

- Create `android/app/src/main/java/com/tpv/bench/vision/TpvBlobEngine.kt`
- Create `android/app/src/test/java/com/tpv/bench/vision/TpvBlobEngineTest.kt`

Implementation notes:

- Wrap `TpvNative.processFrameDebugV2(...)`.
- Return an empty detection list for non-OK status.
- Return exactly one `VisionDetection` for `TPV_OK`, because current TPV v2 is
  single-winner.
- Preserve the original `TpvDetectionDebugV2` result for trigger/recorder via
  an adapter attribute or a side-channel result object.

Suggested wrapper output:

```kotlin
data class EngineFrameResult(
    val engineId: String,
    val detections: List<VisionDetection>,
    val raw: Any?,
)
```

Steps:

- [ ] Define an injectable `TpvNativeAdapter` interface so unit tests do not
      call JNI.
- [ ] Implement production adapter using `TpvNative`.
- [ ] Implement `TpvBlobEngine.process(...)`.
- [ ] Map `TpvBbox` to `RectI`.
- [ ] Attach `TpvDetectionDebugV2` raw result for current trigger path.

Tests:

- [ ] `TPV_EMPTY` returns no detections.
- [ ] `TPV_OK` maps bbox/mask/class/status correctly.
- [ ] rejected class `255` maps to stable class name and score `0f`.
- [ ] `VisionEngineMetadata` declares `Y640` only.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.vision.TpvBlobEngineTest"
```

---

## 7. Task M1.5 — Add VisionPipeline With Noop Tracker

**Goal:** Preserve current pipeline shape while preparing for M2 tracker.

Files:

- Create `android/app/src/main/java/com/tpv/bench/vision/VisionPipeline.kt`
- Create `android/app/src/main/java/com/tpv/bench/vision/MultiObjectTracker.kt`
- Create `android/app/src/test/java/com/tpv/bench/vision/VisionPipelineTest.kt`

Steps:

- [ ] Implement `VisionPipeline.process(frame)`:
      engines → detections → noop tracker → event policy view.
- [ ] Implement M1 `MultiObjectTracker` noop placeholder.
- [ ] Return both all tracked detections and primary-engine detections.
- [ ] Keep engine ordering deterministic.
- [ ] Add per-engine timing hooks, but do not change binary timing format yet.

Tests:

- [ ] Pipeline invokes only enabled engines.
- [ ] Pipeline passes TPV detection through noop tracker.
- [ ] Event policy returns only primary engine detections.
- [ ] Engine exceptions are surfaced as DROP-equivalent pipeline errors, not
      silent stale results.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.vision.VisionPipelineTest"
```

---

## 8. Task M1.6 — Integrate MainActivity Without Behavior Drift

**Goal:** Route `MainActivity.onFrame()` through `VisionPipeline` while keeping
current TPV trigger/overlay/record behavior equivalent.

Files:

- Modify `android/app/src/main/java/com/tpv/bench/MainActivity.kt`

Steps:

- [ ] Build `VisionFrame` after Y extraction and frame index allocation.
- [ ] Rebuild `VisionPipeline` once per Start from the run-locked
      `SettingsSnapshot`; `TpvBlobEngine` receives immutable
      `TpvBlobConfig(binThreshold, darkObjectMode, roi)` at construction time,
      not per frame.
- [ ] Replace direct `TpvNative.processFrameDebugV2(...)` with
      `visionPipeline.process(frame)`.
- [ ] Extract the primary TPV raw result via a typed helper such as
      `EngineFrameResult.requireRaw<TpvDetectionDebugV2>()`; use that same
      value for `DiagnosticsView`, `FramePresence`, recorder compatibility, and
      legacy overlay state.
- [ ] Keep current `FramePresence` derivation based on TPV status.
- [ ] Keep `TriggerMachine` input behavior unchanged.
- [ ] Convert NV21 lazily only inside the commit branch.
- [ ] Keep live overlay clear/render semantics unchanged.
- [ ] Keep DiagnosticsView behavior using TPV result.
- [ ] Keep `lastEventMask`, `lastRoi`, and other UI/cache state in
      `MainActivity`; `VisionPipeline` must remain stateless with respect to UI
      presentation and recorder artifact caches.

Review checklist:

- `proxy.close()` still happens exactly once in `finally`.
- Stop/run snapshot guards remain in place.
- No work moves onto the UI thread.
- `TimingScratch` semantics stay compatible.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest
```

---

## 9. Task M1.7 — Add Vision Metadata Mirror

**Goal:** Add `vision.engines[]` metadata while preserving existing `tpv.*`
compatibility.

Files:

- Modify `android/app/src/main/java/com/tpv/bench/RunRecorder.kt`
- Modify `android/app/src/test/java/com/tpv/bench/RunRecorderTest.kt`

Meta contract:

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
        "required_inputs": ["Y640"],
        "params": {
          "bin_threshold": 120,
          "dark_object_mode": true,
          "roi": {"x": 0, "y": 0, "w": 640, "h": 480}
        },
        "enabled": true
      }
    ],
    "event_policy": {
      "mode": "PRIMARY_ONLY",
      "primary_event_engine": "tpv_blob",
      "enabled_commit_engines": ["tpv_blob"]
    },
    "tracker": {
      "type": "noop",
      "enabled": false
    }
  }
}
```

Steps:

- [ ] Keep `MetaInfo` as the recorder entry point and add
      `MetaInfo.vision: VisionRunConfig`.
- [ ] Write `vision.engines[]`.
- [ ] Keep current top-level `tpv` object.
- [ ] Update `metaToJson()` so legacy `tpv.*` mirror fields are derived from
      `vision.engines[id=tpv_blob].params`, not separate state.
- [ ] Add test that fails if mirror fields drift.

Tests:

- [ ] Existing v2 meta fields still exist.
- [ ] New `vision.schema_version == 1`.
- [ ] `vision.engines[0].id == "tpv_blob"`.
- [ ] `tpv.bin_threshold == vision.engines[id=tpv_blob].params.bin_threshold`.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.RunRecorderTest"
```

---

## 10. Task M1.8 — M1 Full Validation

Run:

```bash
cd android
./gradlew :app:testDebugUnitTest

cd ..
rm -f build/test_debug_api_v2.d build/replay.d
make build/test_debug_api_v2 build/replay
./build/test_debug_api_v2
```

Device smoke:

```bash
PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$HOME/android/sdk/platform-tools:$PATH" make android-apk
make android-verify-sha
adb -s HA25YBM0 install -r android/app/build/outputs/apk/debug/app-debug.apk
adb -s HA25YBM0 shell am force-stop com.tpv.bench
adb -s HA25YBM0 shell pm grant com.tpv.bench android.permission.CAMERA
adb -s HA25YBM0 shell am start -n com.tpv.bench/.MainActivity
```

Acceptance:

- [ ] TPV live overlay still appears.
- [ ] Commit still fires for object placement.
- [ ] `.mask`, `.jpg`, `.y`, `log.jsonl`, `timing.bin` still written.
- [ ] `meta.json` contains both legacy `tpv` and new `vision` blocks.
- [ ] No unconditional NV21 conversion remains in non-commit frames.

---

## 11. Task M2.1 — Implement Tracker Core

**Goal:** Replace noop tracker with deterministic SORT-like tracker.

Files:

- Modify `android/app/src/main/java/com/tpv/bench/vision/MultiObjectTracker.kt`
- Create/modify `android/app/src/test/java/com/tpv/bench/vision/MultiObjectTrackerTest.kt`

Algorithm:

1. Tracks are grouped by `engineId + className`.
2. For each group, match detections to active tracks.
3. Matching score uses IoU first, center distance as fallback.
4. Greedy matching is acceptable for M2 because expected detection counts are
   small. Hungarian assignment can be added later if needed.
5. Unmatched tracks increment `misses`.
6. Unmatched detections create tentative tracks.
7. Track becomes `CONFIRMED` after `minHits`.
8. Track is removed after `maxAge` misses.

M2 state:

```kotlin
data class TrackerConfig(
    val minHits: Int = 2,
    val maxAge: Int = 10,
    val iouThreshold: Float = 0.25f,
    val centerDistancePx: Float = 80f,
)
```

Rationale: M2 uses `minHits = 2` instead of the generic SORT-style default `3`
because the TPV path is still single-winner and device smoke should confirm a
track quickly. Face/Object engines may raise their own default to `3` once real
multi-detection streams land in later milestones.

Steps:

- [ ] Implement track state with stable monotonic `trackId`.
- [ ] Implement IoU and center-distance matching.
- [ ] Implement deterministic greedy assignment.
- [ ] Keep lost tracks out of overlay by default, but expose them in tests.
- [ ] Ensure tracker can reset on Start.

Tests:

- [ ] Single static detection keeps same ID.
- [ ] Slow moving detection keeps same ID.
- [ ] Missing detections for fewer than `maxAge` frames keep track alive.
- [ ] Missing detections past `maxAge` removes track.
- [ ] Two synthetic objects crossing has bounded ID switches.
- [ ] Detections from different engine/class groups do not match each other.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.vision.MultiObjectTrackerTest"
```

---

## 12. Task M2.2 — Add Tracker Settings

**Goal:** Add run-locked tracker config without overloading the current dialog.

Files:

- Modify `android/app/src/main/java/com/tpv/bench/SettingsState.kt`
- Modify MainActivity settings dialog code
- Add/modify settings tests if present

M2 settings:

- `trackerEnabled`: default `true`
- `trackerMinHits`: default `2`
- `trackerMaxAge`: default `10`
- `trackerIouThresholdPct`: default `25`
- `trackerCenterDistancePx`: default `80`

UI:

- If tabbed settings already exist, add `Tracker` tab.
- If current dialog is still single-page, add a compact `Tracker` section but
  keep the spec note that v3+ should move to tabs before M3.

Steps:

- [ ] Add persisted settings with bounds.
- [ ] Snapshot settings at Start.
- [ ] Thread `TrackerConfig` into `VisionPipeline`.
- [ ] Reset tracker state at Start.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest
```

---

## 13. Task M2.3 — Overlay Track IDs

**Goal:** Render track IDs for confirmed TPV tracks without degrading mask
overlay.

Files:

- Modify `android/app/src/main/java/com/tpv/bench/OverlayView.kt`
- Modify `android/app/src/main/java/com/tpv/bench/OverlayPainter.kt`
- Modify `android/app/src/test/java/com/tpv/bench/OverlayPainterTest.kt`

Rules:

- TPV mask rendering remains unchanged.
- Track ID text is drawn only for valid `TPV_OK` detections.
- Text position uses `bbox640` mapped through existing fillCenter transform.
- Track ID drawing must not run for `TPV_EMPTY`, `TPV_SCENE_ERROR`, or
  `TPV_BAD_INPUT`.

Suggested label:

```text
#<track_id> <class_name>
```

Example under the M1/M2 TPV stub class mapping: `#3 tpv_rejected`.

Steps:

- [ ] Extend live overlay state to carry `List<TrackedDetection>`.
- [ ] Keep existing TPV result path for mask/center/axis.
- [ ] Draw track ID near bbox top-left.
- [ ] Add pure geometry tests for label anchor mapping.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.OverlayPainterTest"
```

---

## 14. Task M2.4 — Record Tracks in JSONL

**Goal:** Add `tracks[]` to event records, preserving existing fields.

Files:

- Modify `android/app/src/main/java/com/tpv/bench/RunRecorder.kt`
- Modify `android/app/src/test/java/com/tpv/bench/RunRecorderTest.kt`

JSON addition:

```json
{
  "tracks": [
    {
      "track_id": 1,
      "engine": "tpv_blob",
      "class_name": "tpv_rejected",
      "score": 0.0,
      "bbox": {"x": 254, "y": 157, "w": 268, "h": 153},
      "state": "confirmed"
    }
  ]
}
```

Steps:

- [ ] Extend `CommittedEvent` or recorder call with current tracked detections.
- [ ] Write `tracks[]` for committed frame.
- [ ] Leave existing `detection` object unchanged for v2 compatibility.
- [ ] Do not change `.mask` semantics.

Tests:

- [ ] Existing recordEvent test still finds legacy fields.
- [ ] New test asserts one TPV track entry.
- [ ] Zero tracks always writes `tracks: []` for replay-tool stability.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest --tests "com.tpv.bench.RunRecorderTest"
```

---

## 15. Task M2.5 — M2 Full Validation

Run all local tests:

```bash
cd android
./gradlew :app:testDebugUnitTest

cd ..
rm -f build/test_debug_api_v2.d build/replay.d
make build/test_debug_api_v2 build/replay
./build/test_debug_api_v2
```

Device smoke:

- [ ] Install APK on `HA25YBM0`.
- [ ] Start run.
- [ ] Place a phone/mouse target.
- [ ] Confirm TPV mask still aligns.
- [ ] Confirm visible track ID remains stable while object moves slowly.
- [ ] Stop and export run.
- [ ] Pull latest zip.
- [ ] Confirm `log.jsonl` has `tracks[]`.
- [ ] Confirm old replay still parses the run for TPV core fields.

Suggested commands:

```bash
PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$HOME/android/sdk/platform-tools:$PATH" make android-apk
adb -s HA25YBM0 install -r android/app/build/outputs/apk/debug/app-debug.apk
adb -s HA25YBM0 shell am force-stop com.tpv.bench
adb -s HA25YBM0 shell pm grant com.tpv.bench android.permission.CAMERA
adb -s HA25YBM0 shell am start -n com.tpv.bench/.MainActivity
```

---

## 16. Out of Scope for This Plan

Note: this section describes the original M1/M2 plan boundary. The later M3
face-detection slice is tracked in
`docs/plans/2026-04-27-face-detection-m3-implementation-note.md`.

- MediaPipe Tasks dependency and `FaceEngine`.
- LiteRT/TFLite/ONNX Runtime dependency and `ObjectEngine`.
- Face recognition enrollment/gallery/embedding.
- Portrait orientation.
- Top-N TPV C API.
- OR/AND multi-engine event fusion.
- `timing_v2.bin` binary format change.

These are intentionally deferred to keep M1/M2 reviewable and low risk.

---

## 17. Plan Completion Criteria

M1/M2 is complete when:

- [ ] All Android JVM tests pass.
- [ ] Existing C debug v2 tests pass.
- [ ] TPV-only live behavior is unchanged except optional track ID label.
- [ ] `meta.json` includes `vision` source-of-truth metadata and legacy `tpv`
      mirror.
- [ ] `log.jsonl` includes `tracks[]` for committed events.
- [ ] No unconditional per-frame NV21/RGB conversion remains.
- [ ] Device smoke on `HA25YBM0` confirms stable TPV track ID.
