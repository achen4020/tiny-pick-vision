# Repository Agent Instructions

These instructions apply to the entire repository.

## Android Bench App Review Gates

When modifying the Android bench-test app or its implementation plan, preserve these correctness requirements:

- `TriggerMachine` must commit on the first PRESENT frame when `N_stable == 1`; do not require a second PRESENT frame.
- Live overlay rendering must skip detection markers/text for `TPV_EMPTY`, `TPV_SCENE_ERROR`, and `TPV_BAD_INPUT`; zero-filled detections are not class-0 objects.
- `CameraAdapter` asynchronous binding must be cancellation-safe: if Stop happens before `ProcessCameraProvider.getInstance()` resolves, the later listener must not bind camera/analyzer.
- Live overlay coordinate mapping must match the PreviewView display orientation. Currently achieved by locking `MainActivity` to `screenOrientation="landscape"` in `AndroidManifest.xml` so the 640×480 camera buffer and the view share orientation and no rotation transform is needed. If the activity is ever unlocked to portrait, `ImageProxy.imageInfo.rotationDegrees` must be threaded into `OverlayView` and applied before rendering; update spec §9 and DEVELOPER.md §11 in lockstep with that change.

