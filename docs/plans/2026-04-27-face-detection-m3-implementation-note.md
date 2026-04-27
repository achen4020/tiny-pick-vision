# Face Detection M3 Implementation Note

Date: 2026-04-27
Status: Implemented as Android demo spike; non-SDK

This note extends `docs/plans/2026-04-27-face-object-tracking-expansion-plan.md`,
which intentionally stopped at M1/M2. The implementation adds the first M3
slice: live MediaPipe face detection plus tracker/overlay integration.

SDK delivery note: this MediaPipe implementation is not the final product path.
The final face detector must be implemented behind the C ABI described in
`docs/specs/2026-04-27-c-first-vision-sdk-architecture.md`.

## Scope

Included:

- MediaPipe Tasks Face Detector runtime.
- Bundled BlazeFace short-range `.tflite` asset.
- Run-locked Settings toggle and confidence thresholds.
- Lazy NV21 → ARGB conversion only when the face engine is enabled.
- Face detections converted into 640×480 `VisionDetection` records.
- `MultiObjectTracker` integration for stable face track IDs.
- Live cyan face bbox/label/landmark overlay.
- `meta.json.vision.engines[id=face]` metadata with privacy export flags.

Not included:

- Face identity recognition.
- Embeddings or encrypted face gallery.
- Face crops/landmarks/identity export.
- Face-triggered event commits.
- Portrait/rotation remapping beyond the existing landscape lock.

## Runtime Contract

- `primary_event_engine` remains `tpv_blob`.
- Face detections never create `TriggerMachine` commits in the default mode.
- Committed `log.jsonl.tracks[]` remains primary-event tracks only.
- Face landmarks are live overlay data only and are not serialized by default.
- Disabled face detection has no per-frame ARGB/NV21 allocation beyond the
  existing TPV path.

## Validation

Local gates:

```bash
cd android
./gradlew :app:testDebugUnitTest

cd ..
make build/test_debug_api_v2 build/replay
./build/test_debug_api_v2
make android-apk
make android-verify-sha
```

Device smoke:

- Install and launch on `HA25YBM0`.
- Enable Settings → Face Detection.
- Start with the front camera and verify a cyan face box follows a face.
- Switch back to TPV target testing and verify green mask alignment is unchanged.
- Stop and inspect the latest zip: `meta.json.ui_version == "v3"` only when
  face was enabled; no face crops, landmarks, identities, or embeddings exist.
