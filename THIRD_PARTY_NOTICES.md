# Third-Party Notices

Tiny Pick Vision includes or depends on the following third-party components.

## MediaPipe Tasks Vision

- Component: `com.google.mediapipe:tasks-vision:0.10.33`
- Use: Android demo face detection runtime
- Upstream: https://github.com/google-ai-edge/mediapipe
- License: Apache License 2.0
- License text: https://github.com/google-ai-edge/mediapipe/blob/master/LICENSE

MediaPipe is used only by the Android demo/spike. It is not the product C SDK
implementation of face detection.

## BlazeFace Short-Range Model

- File: `android/app/src/main/assets/blaze_face_short_range.tflite`
- Use: MediaPipe Tasks Face Detector model asset
- Official source: https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/latest/blaze_face_short_range.tflite
- SHA-256: `b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f`
- Upstream project: https://github.com/google-ai-edge/mediapipe
- License: Apache License 2.0

The checked-in file is byte-identical to the official model at the URL above.

## Gradle Wrapper

- Files: `android/gradlew`, `android/gradlew.bat`, `android/gradle/wrapper/gradle-wrapper.jar`
- Upstream: https://github.com/gradle/gradle
- License: Apache License 2.0
- License text: https://github.com/gradle/gradle/blob/master/LICENSE
