# C-First Vision SDK Architecture

Date: 2026-04-27
Status: Draft for review
Owner: Tiny Pick Vision

## 1. Decision

Tiny Pick Vision is a **C language SDK first** project.

The Android bench app is a demo, validation, and recording tool. It may use
Android-only code for camera, UI, overlay, and diagnostics, but product vision
capabilities that will be offered to third parties must be implemented behind a
stable C ABI exported by `libtpv.so`.

The current Android `MediaPipe FaceEngine` is therefore a **spike / reference
prototype only**. It proves face-detection UX and metadata wiring, but it is not
the SDK implementation path.

## 2. Consequences

- Third parties consume `include/tpv.h` / future `include/tpv_vision.h` and
  `libtpv.so`, not Kotlin `VisionEngine` classes.
- TPV blob, face detection, object detection, and tracking must produce results
  through C structs and C functions.
- Android JNI is an adapter layer. It must not contain product-only detection
  logic beyond marshaling buffers and C results.
- Model runtime dependencies must have C/C++ integration paths and redistributable
  binary packaging stories.
- App-layer MediaPipe can remain for comparison until C face parity exists, but
  it must be build-time or settings-gated and documented as non-SDK.

## 3. Current Boundary

Existing stable C API:

| File | Contract |
|---|---|
| `include/tpv.h` | Public C entry: `tpv_process_frame`, `tpv_serialize_payload`, status/class constants |
| `include/tpv_internal.h` | Internal/debug TPV structs and debug v1/v2 functions |
| `src/pipeline.c` | TPV threshold → CCL → features → classify → pose pipeline |
| `android/app/src/main/cpp/tpv_jni.c` | JNI marshaling from Android `ByteArray` to C debug APIs |
| `android/app/src/main/java/com/tpv/bench/TpvNative.kt` | Kotlin native declarations |

Android-only spike boundary:

| File | Role |
|---|---|
| `android/app/src/main/java/com/tpv/bench/vision/FaceEngine.kt` | MediaPipe Tasks face-detection prototype |
| `android/app/src/main/java/com/tpv/bench/vision/VisionPipeline.kt` | Android-side multi-engine orchestration prototype |
| `android/app/src/main/java/com/tpv/bench/vision/MultiObjectTracker.kt` | Kotlin tracker prototype |

The SDK architecture must move the second table's product responsibilities into
C before third-party delivery.

## 4. Target Layers

```text
Third-party app / Android demo
  └─ JNI / FFI / direct C call adapter
      └─ libtpv.so public C ABI
          ├─ tpv_blob engine
          ├─ face engine
          ├─ object engine
          ├─ multi-object tracker
          └─ event policy / trigger integration
              └─ optional model runtime backend
```

Only the top adapter layer may be platform-specific. Detection/tracking outputs
must be C-owned and portable.

## 5. Public C ABI Shape

Add a new public header instead of expanding `tpv_process_frame` beyond its
original compact contract:

```text
include/tpv_vision.h
```

Initial API shape:

```c
typedef struct tpv_vision_context tpv_vision_context;

typedef enum {
    TPV_ENGINE_TPV_BLOB = 1u << 0,
    TPV_ENGINE_FACE     = 1u << 1,
    TPV_ENGINE_OBJECT   = 1u << 2,
} tpv_engine_flags;

typedef enum {
    TPV_PIXEL_Y8_640x480 = 1,
    TPV_PIXEL_NV21       = 2,
    TPV_PIXEL_RGBA8888   = 3,
} tpv_pixel_format;

typedef struct {
    uint32_t abi_version;
    uint32_t enabled_engines;
    uint32_t primary_event_engine;
    uint8_t  bin_threshold;
    uint8_t  dark_object_mode;
    uint8_t  reserved0[2];
    int16_t  roi_x, roi_y, roi_w, roi_h;
    int32_t  tracker_min_hits;
    int32_t  tracker_max_age;
    float    tracker_iou_threshold;
    float    tracker_center_distance_px;
    float    face_min_score;
    float    object_min_score;
} tpv_vision_config;

typedef struct {
    const uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
    int32_t rotation_degrees;
    int64_t timestamp_ns;
} tpv_vision_frame;

typedef struct {
    uint32_t engine_id;
    uint32_t class_id;
    float score;
    int16_t x, y, w, h;       /* 640×480 canonical coords */
    uint32_t track_id;        /* 0 when untracked */
    uint32_t flags;
} tpv_vision_detection;

typedef struct {
    int32_t status;
    int32_t detection_count;
    tpv_vision_detection *detections;
} tpv_vision_result;
```

The exact names can change during implementation, but the ABI principles are
fixed:

- Use fixed-width integer types.
- Keep coordinates in the existing canonical 640×480 space.
- Never expose Kotlin/Java classes as contract types.
- Avoid returning heap allocations owned by an unknown runtime. Caller-provided
  output buffers are preferred.
- Include `abi_version` and reserved fields from the first version.
- Public structs require size/layout tests on host and Android.

## 6. Engine Ownership

| Engine | Final owner | Notes |
|---|---|---|
| TPV blob | C | Already implemented; wrap into new multi-engine result model |
| Tracker | C | Kotlin tracker is prototype only |
| Event policy | C | Required so `primary_event_engine` semantics are SDK-consistent |
| Face detection | C/C++ behind C ABI | May use LiteRT/TFLite C API, ONNX Runtime C API, or custom inference backend |
| Object detection | C/C++ behind C ABI | Same runtime decision as face where practical |
| Face recognition | C/C++ behind C ABI | Separate from detection; includes embeddings/gallery/privacy controls |

## 7. Model Runtime Requirements

The chosen runtime must satisfy:

- Works without Google Play Services.
- Has C or C++ APIs callable from `libtpv.so`.
- Can be packaged with Android third-party apps without hidden service downloads.
- Has a clear license and redistribution story.
- Supports deterministic model asset loading from app files/assets.
- Allows C-side preprocessing and postprocessing ownership.

Candidate runtimes:

| Runtime | Fit | Risk |
|---|---|---|
| LiteRT / TensorFlow Lite C API | Strong Android/mobile fit, common `.tflite` model path | Need package-size and delegate policy |
| ONNX Runtime Mobile C API | Broad model format and C API | Larger binary and operator-config work |
| Custom tiny inference | Maximum control | High engineering cost; only viable for narrow models |
| MediaPipe Tasks Android API | Good app demo | Not SDK path because it is Android/Java task API first |

## 8. Android App Role

The app should eventually call:

```text
CameraX YUV frame
  → minimal adapter/copy
  → TpvNative.processVisionFrameV3(...)
  → JNI
  → tpv_vision_process(...)
  → Kotlin DTOs for overlay/recording
```

Allowed Android responsibilities:

- Camera binding and front/back switching.
- UI settings and run-locked snapshots.
- Overlay rendering.
- Recorder artifacts and device acceptance.
- Optional spike engines hidden behind debug settings.

Disallowed Android responsibilities for SDK product path:

- Owning face/object model inference as Kotlin/Java logic.
- Owning final tracker/event-policy semantics.
- Defining final result schema independently from C structs.

## 9. Privacy Boundary

Face recognition remains a separate risk class.

For the C SDK:

- Face detection may output bbox/score and optional landmarks.
- Face landmarks are biometric/PII when persisted.
- Embeddings, face crops, gallery records, and identity labels must be opt-in.
- If recognition is added, encrypted gallery storage is a platform adapter
  concern, but embedding generation and matching must be behind C ABI.

For the Android demo:

- Existing MediaPipe landmarks stay live-only.
- Recorder must not export face crops, landmarks, identity, or embeddings by
  default.

## 10. Compatibility

- Existing `tpv_process_frame` remains unchanged for v1/v2 users.
- Existing debug v2 APIs remain available for bench app diagnostics.
- New multi-engine APIs are additive.
- Replay can read old v1/v2 runs and new v3 runs.
- `meta.json.tpv` remains a compatibility mirror; `meta.json.vision` describes
  the multi-engine source of truth.

## 11. Acceptance Gates

A capability is SDK-ready only when:

- Public C header exists with documented structs/functions.
- Host tests cover bad input, capacity limits, ABI struct sizes, and result
  serialization.
- Android JNI calls the C API, not a Kotlin-only detector.
- Device smoke validates the Android demo path.
- Third-party integration sample can call the same C ABI without app-private
  classes.
- Model assets and runtime binaries have documented packaging and license
  requirements.
