# C-First Vision SDK Implementation Plan

Date: 2026-04-27
Status: Draft for review
Spec: `docs/specs/2026-04-27-c-first-vision-sdk-architecture.md`

Current implementation status:

- C0 public ABI skeleton is implemented:
  - `include/tpv_vision.h`
  - `src/vision_api.c`
  - `tests/test_vision_api.c`
- C0 currently supports TPV-only `TPV_PIXEL_Y8_640X480` frames. Face/object
  engine flags are intentionally rejected until later phases implement them in C.

## 1. Objective

Re-center face/object/tracking work on a third-party consumable C SDK.

The Android MediaPipe face detector remains a spike until the C SDK face engine
reaches parity. New product behavior must be implemented behind C ABI first,
then exposed to Android through JNI.

## 2. Non-Goals

- Do not remove the existing `tpv_process_frame` API.
- Do not block the Android demo from using current TPV v2 debug APIs.
- Do not implement identity recognition in this plan.
- Do not choose cloud inference.
- Do not move Android camera/overlay/recorder UI into C.

## 3. Deliverable Map

| Phase | Goal | Primary output |
|---|---|---|
| C0 | Public ABI skeleton | `include/tpv_vision.h`, host ABI tests |
| C1 | C tracker/event policy | `src/vision_tracker.c`, `src/vision_policy.c` |
| C2 | TPV blob as C vision engine | `tpv_vision_process()` emits TPV detections |
| C3 | Android JNI v3 bridge | `TpvNative.processVisionFrameV3()` calls C SDK |
| C4 | Runtime decision spike | LiteRT/TFLite C API vs ONNX Runtime Mobile decision |
| C5 | C face detector | C-side preprocess/inference/decode/NMS |
| C6 | Android app cutover | Disable MediaPipe spike by default, use C face |
| C7 | Third-party sample | Minimal C/Android consumer sample and packaging docs |

## 4. Phase C0 — Public ABI Skeleton

Goal: introduce additive C API without changing current TPV v1/v2 behavior.

Files:

- `include/tpv_vision.h` new
- `src/vision_api.c` new
- `tests/test_vision_api.c` new
- `Makefile` update
- `android/app/src/main/cpp/CMakeLists.txt` no-op for C0 because Android imports
  the `libtpv.so` built by `make android-so`; update it only when JNI v3 starts
  including `tpv_vision.h` directly.

Steps:

- Define `TPV_VISION_ABI_VERSION`.
- Define public opaque `tpv_vision_context`.
- Define config, frame, detection, result, engine metadata structs.
- Use caller-owned output arrays:
  - `tpv_vision_detection *detections`
  - `int32_t detection_capacity`
  - `int32_t detection_count`
- Add:
  - `tpv_vision_default_config(tpv_vision_config *out)`
  - `tpv_vision_context_size(const tpv_vision_config *cfg, size_t *bytes_out)`
  - `tpv_vision_init(void *mem, size_t bytes, const tpv_vision_config *cfg, tpv_vision_context **ctx_out)`
  - `tpv_vision_reset(tpv_vision_context *ctx)`
  - `tpv_vision_process(tpv_vision_context *ctx, const tpv_vision_frame *frame, tpv_vision_result *out)`
- Add host tests for:
  - null argument returns `TPV_BAD_INPUT`
  - insufficient output capacity returns deterministic truncation or capacity error
  - struct sizes are stable
  - default config matches TPV-only mode

Validation:

```bash
make build/test_vision_api
./build/test_vision_api
make build/test_debug_api_v2
./build/test_debug_api_v2
```

## 5. Phase C1 — C Tracker and Event Policy

Goal: move stable ID and primary-event semantics out of Kotlin.

Files:

- `include/tpv_vision.h` update
- `src/vision_tracker.c` new
- `src/vision_policy.c` new
- `tests/test_vision_tracker.c` new
- `tests/test_vision_policy.c` new

Steps:

- Port Kotlin `MultiObjectTracker` behavior to C:
  - match by `(engine_id, class_id/class_name equivalent)`
  - greedy IoU + center-distance matching
  - `min_hits`, `max_age`, `CONFIRMED/TENTATIVE/LOST`
- Keep bounded arrays; no unbounded heap allocations in hot path.
- Implement `primary_event_engine` filtering in C.
- Define stable numeric engine IDs:
  - `TPV_ENGINE_ID_TPV_BLOB`
  - `TPV_ENGINE_ID_FACE`
  - `TPV_ENGINE_ID_OBJECT`
- Host tests mirror existing JVM tracker/policy tests.

Validation:

```bash
make build/test_vision_tracker build/test_vision_policy
./build/test_vision_tracker
./build/test_vision_policy
```

## 6. Phase C2 — TPV Blob Engine Behind Vision API

Goal: make current TPV blob detector the first C vision engine.

Files:

- `src/vision_api.c` update
- `src/pipeline.c` reuse
- `tests/test_vision_tpv_blob.c` new

Steps:

- Accept `TPV_PIXEL_Y8_640x480` frames first.
- Call existing `tpv_process_frame_debug_v2` or an internal non-debug adapter.
- Convert TPV status/classes into `tpv_vision_detection`.
- Preserve mask/debug data only in debug extension structs, not mandatory base
  `tpv_vision_detection`.
- Ensure `N_stable == 1` first-present commit behavior is not regressed if event
  policy/trigger moves into C later.

Validation:

```bash
make build/test_vision_tpv_blob build/test_debug_api_v2
./build/test_vision_tpv_blob
./build/test_debug_api_v2
```

## 7. Phase C3 — Android JNI v3 Bridge

Goal: Android app can use the same C SDK API third parties will use.

Files:

- `android/app/src/main/cpp/tpv_jni.c` update
- `android/app/src/main/java/com/tpv/bench/TpvNative.kt` update
- `android/app/src/main/java/com/tpv/bench/vision/TpvBlobEngine.kt` update or replace
- JVM tests for DTO mapping

Steps:

- Add native method `processVisionFrameV3(...)`.
- Allocate/reuse C context at run boundary from the run-locked settings snapshot.
- JNI marshals Y/NV21/RGBA frame data into `tpv_vision_frame`.
- JNI marshals C detections/tracks into Kotlin DTOs for overlay/recorder only.
- Android `VisionPipeline` becomes a demo adapter, not source-of-truth product
  logic.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest
cd ..
make android-apk
make android-verify-sha
```

## 8. Phase C4 — Runtime Decision Spike

Goal: choose a C/C++ model runtime before implementing C face/object.

Decision inputs:

- APK/AAR size impact.
- Supported Android ABI matrix.
- License and redistribution.
- Asset loading path.
- NNAPI/GPU delegate policy.
- C/C++ API maturity.
- Ability to run face and object models with shared runtime.

Candidates:

- LiteRT / TensorFlow Lite C API.
- ONNX Runtime Mobile C API.
- Custom tiny inference only if chosen model is small enough.

Deliverables:

- `docs/decisions/2026-04-27-model-runtime-selection.md`
- Minimal host/Android runtime smoke.
- Binary size report.

Validation:

```bash
make runtime-smoke
make android-apk
ls -lh android/app/build/outputs/apk/debug/app-debug.apk
```

## 9. Phase C5 — C Face Detector

Goal: replace Android MediaPipe product path with C-side face detection.

Files:

- `src/face_detector.c` new
- `src/face_preprocess.c` new
- `src/face_postprocess.c` new
- `tests/test_face_postprocess.c` new
- model asset packaging docs

Steps:

- Define model input format and preprocessing in C.
- Run selected runtime from C/C++.
- Decode face boxes/scores/landmarks in C.
- Apply NMS in C.
- Emit `TPV_ENGINE_ID_FACE` detections through `tpv_vision_process`.
- Keep landmarks optional/live-only unless caller explicitly requests export.
- Match Android MediaPipe spike behavior on representative frames before
  removing the spike from default builds.

Validation:

```bash
make build/test_face_postprocess build/test_vision_api
./build/test_face_postprocess
./build/test_vision_api
cd android && ./gradlew :app:testDebugUnitTest
```

Device acceptance:

- Front camera face bbox follows a face.
- Face enabled run writes `meta.json.vision.engines[id=face].type == "c_sdk"`.
- No MediaPipe Android dependency is required for the C face path.

## 10. Phase C6 — Android App Cutover

Goal: make C face engine the default demo path.

Steps:

- Replace `FaceEngine.kt` runtime with JNI-backed C face engine.
- Keep MediaPipe spike only behind debug build flag or remove it.
- Ensure `RunRecorder` metadata marks provider/runtime from C SDK.
- Verify TPV-only runs still write `ui_version=v2`; face/object runs write `v3`.

Validation:

```bash
cd android
./gradlew :app:testDebugUnitTest
cd ..
make android-apk
make android-verify-sha
```

## 11. Phase C7 — Third-Party SDK Sample

Goal: prove the API can be consumed outside the bench app.

Deliverables:

- `examples/c_vision_smoke/` host sample.
- `examples/android_sdk_consumer/` minimal Android sample or documented JNI
  integration snippet.
- Packaging document:
  - headers
  - `libtpv.so`
  - model assets
  - runtime shared libs if any
  - ProGuard/R8 notes if Android wrapper is provided

Acceptance:

- Sample compiles without importing `com.tpv.bench.*`.
- Sample calls `tpv_vision_process`.
- Sample prints detections/tracks from a test frame.

## 12. Review Gates

Before any face/object feature is called SDK-ready:

- No Kotlin/Java detector is required for the capability.
- Public C header documents ownership, capacity, and status codes.
- Host tests run without Android.
- Android tests prove JNI mapping only.
- Device acceptance proves demo integration.
- License/redistribution notes exist for every model/runtime binary.
