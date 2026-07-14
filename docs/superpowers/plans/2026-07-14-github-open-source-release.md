# Tiny Pick Vision GitHub Open-Source Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare the existing `achen4020/tiny-pick-vision` repository for public release, rewrite all Git author/committer emails to `der20044@msn.com`, publish under Apache-2.0, and verify anonymous access.

**Architecture:** Complete and commit local source fixes first, then add a bilingual public-facing documentation layer and legal notices. Keep the GitHub repository private while auditing and rewriting history; push rewritten refs with explicit leases, then make the repository public as the final external-state change.

**Tech Stack:** Git, GitHub CLI, C11/Make, Android Gradle/Kotlin/JNI, Apache License 2.0, shell-based release checks.

## Global Constraints

- Existing repository: `https://github.com/achen4020/tiny-pick-vision.git`.
- Preserve commit content, messages, author/committer names, dates, branches, and tags; rewrite only Author/Committer email fields.
- Rewrite every reachable commit email to `der20044@msn.com`.
- License the project under Apache License 2.0 with `Copyright 2026 Alvin Chen`.
- README must be bilingual: Chinese first, English second.
- Keep `android/app/src/main/assets/blaze_face_short_range.tflite` in Git.
- The bundled model must be attributed to the official MediaPipe model URL and SHA-256 `b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f`.
- Keep the repository private until all local checks, history checks, and pushes succeed.
- Use explicit `--force-with-lease=<ref>:<old-object-id>` when pushing rewritten refs.
- Preserve all Android review gates in `AGENTS.md`.
- Do not stage or discard unrelated user changes.

---

## File Responsibility Map

- `README.md`: bilingual project landing page, build entry points, limitations, privacy, roadmap, and links.
- `LICENSE`: unmodified Apache License 2.0 text.
- `NOTICE`: project copyright notice.
- `THIRD_PARTY_NOTICES.md`: MediaPipe runtime/model and Gradle Wrapper provenance and licensing.
- `docs/DEVELOPER.md`: detailed operational build/test/calibration guide linked from README; only correct stale statements discovered during README verification.
- `.gitignore`: excludes generated native libraries, APK/CMake/Gradle output, calibration output, IDE files, and local configuration.
- `docs/superpowers/specs/2026-07-14-github-open-source-release-design.md`: approved release design.
- `docs/superpowers/plans/2026-07-14-github-open-source-release.md`: execution checklist.

---

### Task 1: Commit the Completed Review Fixes

**Files:**
- Modify: `android/app/src/main/cpp/tpv_jni.c`
- Modify: `android/app/src/main/java/com/tpv/bench/CameraAdapter.kt`
- Modify: `android/app/src/main/java/com/tpv/bench/MainActivity.kt`
- Modify: `android/app/src/main/java/com/tpv/bench/TpvNative.kt`
- Modify: `android/app/src/main/java/com/tpv/bench/vision/EventPolicy.kt`
- Modify: `android/app/src/main/java/com/tpv/bench/vision/FaceEngine.kt`
- Modify: `android/app/src/main/java/com/tpv/bench/vision/TpvBlobEngine.kt`
- Modify: `android/app/src/test/java/com/tpv/bench/vision/EventPolicyTest.kt`
- Modify: `android/app/src/test/java/com/tpv/bench/vision/TpvBlobEngineTest.kt`
- Create: `android/app/src/test/java/com/tpv/bench/CameraStartGenerationTest.kt`
- Create: `android/app/src/test/java/com/tpv/bench/vision/DetectionCadenceTest.kt`
- Modify: `include/tpv_internal.h`
- Modify: `src/vision_api.c`
- Modify: `tests/test_debug_api_v2.c`
- Modify: `docs/DEVELOPER.md`

**Interfaces:**
- Consumes: Existing v3 C vision API and Android `VisionPipeline`.
- Produces: Generation-safe camera binding, `LIVE_ONLY` face policy, one TPV inference per frame, context-owned debug access, and 12 FPS face inference behavior.

- [ ] **Step 1: Confirm the worktree contains only the reviewed implementation set plus release documents**

Run:

```bash
git status --short
git diff --name-only
```

Expected: the files listed above are modified/untracked; no credentials, generated APKs, `src/model_data.c`, `.idea`, or `jniLibs` files appear.

- [ ] **Step 2: Run the implementation verification gates**

Run sequentially:

```bash
make test
make -C tools/calibrate test
make check-layout
cd android && ANDROID_HOME="$HOME/android/sdk" ./gradlew :app:testDebugUnitTest --rerun-tasks && cd ..
PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH" make android-so
cd android && ANDROID_HOME="$HOME/android/sdk" ./gradlew :app:assembleDebug && cd ..
PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH" make size
make android-verify-sha
git diff --check
```

Expected: all tests/builds exit 0; ARM stripped size is at most 20480 bytes; APK model SHA matches `src/model_data.c`.

Android SO/APK/size/SHA validation requires a locally calibrated, ignored
`src/model_data.c`. Before running those four gates, require both:

```bash
test -f src/model_data.c
git check-ignore -q src/model_data.c
```

Never stage or publish this generated file.

- [ ] **Step 3: Stage only the reviewed implementation set**

Run:

```bash
git add android/app/src/main/cpp/tpv_jni.c
git add android/app/src/main/java/com/tpv/bench/CameraAdapter.kt
git add android/app/src/main/java/com/tpv/bench/MainActivity.kt
git add android/app/src/main/java/com/tpv/bench/TpvNative.kt
git add android/app/src/main/java/com/tpv/bench/vision/EventPolicy.kt
git add android/app/src/main/java/com/tpv/bench/vision/FaceEngine.kt
git add android/app/src/main/java/com/tpv/bench/vision/TpvBlobEngine.kt
git add android/app/src/test/java/com/tpv/bench/CameraStartGenerationTest.kt
git add android/app/src/test/java/com/tpv/bench/vision/DetectionCadenceTest.kt
git add android/app/src/test/java/com/tpv/bench/vision/EventPolicyTest.kt
git add android/app/src/test/java/com/tpv/bench/vision/TpvBlobEngineTest.kt
git add include/tpv_internal.h src/vision_api.c tests/test_debug_api_v2.c docs/DEVELOPER.md
git diff --cached --check
git diff --cached --name-status
```

Expected: only the reviewed implementation files are staged; the open-source plan file remains unstaged until Task 4.

- [ ] **Step 4: Commit the reviewed fixes**

Run:

```bash
git commit -m "fix: harden Android vision pipeline"
```

Expected: one commit containing the reviewed fixes and regression tests.

---

### Task 2: Add Apache-2.0 and Third-Party Notices

**Files:**
- Create: `LICENSE`
- Create: `NOTICE`
- Create: `THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Consumes: MediaPipe dependency `com.google.mediapipe:tasks-vision:0.10.33`, the bundled model, and Gradle Wrapper files.
- Produces: GitHub-detectable project license and auditable third-party provenance.

- [ ] **Step 1: Add the canonical Apache License 2.0 text**

Create `LICENSE` from the canonical text at:

```text
https://www.apache.org/licenses/LICENSE-2.0.txt
```

The file must begin with:

```text
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/
```

and end with the standard Apache appendix sentence:

```text
   limitations under the License.
```

- [ ] **Step 2: Add the project NOTICE**

Create `NOTICE` with exactly:

```text
Tiny Pick Vision
Copyright 2026 Alvin Chen

This product includes software and model assets developed by third parties.
See THIRD_PARTY_NOTICES.md for details.
```

- [ ] **Step 3: Add third-party notices**

Create `THIRD_PARTY_NOTICES.md` with these concrete entries:

```markdown
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
```

- [ ] **Step 4: Verify the legal files and model identity**

Run:

```bash
test -s LICENSE
test -s NOTICE
test -s THIRD_PARTY_NOTICES.md
rg -n "Apache License|Alvin Chen|MediaPipe|b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f" LICENSE NOTICE THIRD_PARTY_NOTICES.md
shasum -a 256 android/app/src/main/assets/blaze_face_short_range.tflite
git diff --check -- LICENSE NOTICE THIRD_PARTY_NOTICES.md
```

Expected: all files are non-empty; model hash exactly matches the declared SHA-256; diff check exits 0.

- [ ] **Step 5: Commit the legal files**

Run:

```bash
git add LICENSE NOTICE THIRD_PARTY_NOTICES.md
git commit -m "docs: add Apache-2.0 licensing"
```

---

### Task 3: Create the Bilingual README

**Files:**
- Create: `README.md`
- Reference: `docs/DEVELOPER.md`
- Reference: `docs/specs/2026-04-27-c-first-vision-sdk-architecture.md`

**Interfaces:**
- Consumes: verified commands and architecture from existing developer/spec documents.
- Produces: the public GitHub landing page.

- [ ] **Step 1: Create the Chinese README half**

Use this exact section order and meaning:

```markdown
# Tiny Pick Vision

面向嵌入式拾取与实机验证场景的轻量视觉 SDK。核心算法使用 C11 实现，
Android Bench App 用于 CameraX 实时预览、诊断、事件录制和设备验收。

[English](#english) · [开发指南](docs/DEVELOPER.md) · [许可证](LICENSE)

## 项目状态

> 当前是工程验证版本，不是开箱即用的通用目标识别产品。

- Object 模式：阈值分割、连通域、形状特征、Mahalanobis 分类与姿态估计；不是通用神经网络目标检测。
- Face 模式：Android MediaPipe 人脸检测与跟踪；不包含身份识别、注册、Embedding 或活体检测。
- 第三方 SDK 边界：`include/tpv.h` 和 `include/tpv_vision.h`；Android MediaPipe 实现仅用于 Demo/Spike。

## 架构

Camera/Frame → C Vision Pipeline → Tracker/Event Policy → JNI/FFI → Android Demo or third-party consumer

## 主要能力

- 固定 640×480 Y8 输入的轻量 C 视觉管线
- 无运行时堆分配的调用方结果缓冲区 ABI
- 连通域、形状分类、中心点与主轴姿态
- C 与 Kotlin 多目标跟踪/事件策略验证
- CameraX 实时 Overlay、诊断面板、运行录制和回放
- PC 端标定、mask 可视化和时延分析工具

## 仓库结构

| 路径 | 说明 |
|---|---|
| `include/` | 公共 C API 与配置 |
| `src/` | C 视觉管线、跟踪和策略 |
| `tests/` | 主机端 C 测试与 ABI 检查 |
| `android/` | Android Bench App 和 JNI |
| `tools/` | 标定、回放、mask 与时延工具 |
| `docs/` | 设计、计划、验收和开发文档 |

## 快速开始

```bash
make check-layout
make test
make -C tools/calibrate all test
```

`src/model_data.c` 由标定工具生成并被 `.gitignore` 排除。需要构建真实模型的
`target`、`replay` 或 Android APK 时，请先按开发指南完成标定。

Android 要求 JDK 17、Android SDK 34 和 NDK r26+：

```bash
PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH" make android-apk
make android-verify-sha
```

完整构建、标定、设备安装和回放流程见 [docs/DEVELOPER.md](docs/DEVELOPER.md)。

## 正确性约束

- `N_stable == 1` 时首个 PRESENT 帧必须提交。
- EMPTY、SCENE_ERROR、BAD_INPUT 不绘制检测标记。
- CameraX 异步绑定必须支持 Stop 取消。
- Android Activity 当前固定横屏；解除横屏前必须实现 rotation-aware Overlay 映射。

## 隐私

Face 模式默认只做设备端实时检测。项目不默认导出人脸裁剪、关键点、身份、
Embedding 或人脸库。集成者仍需根据适用法律取得用户授权并处理 MediaPipe 运行时遥测告知。

## 路线图

- 选择 C/C++ 端模型运行时
- 通过 C ABI 提供人脸和通用目标检测
- 提供独立第三方 C/Android Consumer 示例
- 完善真实设备性能与数据集验收

## 贡献与许可证

Issue 和 Pull Request 均欢迎。提交前请运行主机端和 Android 测试。

项目使用 [Apache License 2.0](LICENSE)。第三方组件和模型见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
```

- [ ] **Step 2: Add the English README half**

Append this English section so that both language versions state the same capabilities and limitations:

```markdown
## English

Tiny Pick Vision is a lightweight vision SDK for embedded pick-and-place and
on-device validation. The core pipeline is written in C11; the Android Bench
App provides CameraX preview, diagnostics, event recording, and device acceptance.

### Project Status

> This is an engineering-validation release, not a general-purpose object
> recognition product that works out of the box.

- Object mode uses thresholding, connected components, shape features,
  Mahalanobis classification, and pose estimation; it is not a general neural
  network object detector.
- Face mode uses Android MediaPipe face detection and tracking; it does not
  provide identity recognition, enrollment, embeddings, or liveness detection.
- The third-party SDK boundary is `include/tpv.h` and `include/tpv_vision.h`;
  the Android MediaPipe implementation is only a demo/spike.

### Architecture

Camera/Frame → C Vision Pipeline → Tracker/Event Policy → JNI/FFI → Android Demo or third-party consumer

### Features

- Lightweight C pipeline for fixed 640×480 Y8 input
- Caller-owned result-buffer ABI with no runtime heap allocation
- Connected components, shape classification, center point, and principal-axis pose
- C and Kotlin validation of multi-object tracking and event policy
- CameraX live overlay, diagnostics, run recording, and replay
- Desktop calibration, mask visualization, and latency-analysis tools

### Repository Layout

| Path | Purpose |
|---|---|
| `include/` | Public C APIs and configuration |
| `src/` | C vision pipeline, tracking, and policy |
| `tests/` | Host-side C tests and ABI checks |
| `android/` | Android Bench App and JNI |
| `tools/` | Calibration, replay, mask, and latency tools |
| `docs/` | Design, planning, acceptance, and development documentation |

### Quick Start

```bash
make check-layout
make test
make -C tools/calibrate all test
```

`src/model_data.c` is generated by the calibration tool and excluded by
`.gitignore`. Complete the calibration workflow in the developer guide before
building the real-model `target`, `replay`, or Android APK targets.

Android requires JDK 17, Android SDK 34, and NDK r26+:

```bash
PATH="$HOME/android/sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64/bin:$PATH" make android-apk
make android-verify-sha
```

See [docs/DEVELOPER.md](docs/DEVELOPER.md) for complete build, calibration,
device-installation, and replay instructions.

### Correctness Constraints

- When `N_stable == 1`, commit on the first PRESENT frame.
- Do not render detection markers for EMPTY, SCENE_ERROR, or BAD_INPUT.
- CameraX asynchronous binding must be cancellable by Stop.
- The Android Activity is currently locked to landscape; rotation-aware Overlay
  mapping must be implemented before removing that lock.

### Privacy

Face mode performs on-device live detection by default. The project does not
export face crops, landmarks, identities, embeddings, or face databases by
default. Integrators remain responsible for user consent under applicable laws
and for disclosing any MediaPipe runtime telemetry.

### Roadmap

- Select a C/C++ model runtime
- Expose face and general object detection through the C ABI
- Add standalone third-party C and Android consumer examples
- Expand real-device performance and dataset acceptance coverage

### Contributing and License

Issues and pull requests are welcome. Run the host and Android test suites
before submitting changes.

This project is licensed under the [Apache License 2.0](LICENSE). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party components and
model assets.
```

- [ ] **Step 3: Verify every README command against repository targets**

Run:

```bash
rg -n "^(check-layout|test|android-apk|android-verify-sha|size):" Makefile
rg -n "^all:|^test:" tools/calibrate/Makefile
rg -n "Object 模式|Face 模式|Object mode|Face mode|Apache License|THIRD_PARTY" README.md
git diff --check -- README.md
```

Expected: every command maps to an existing Make target; both languages contain the required limitations and license links.

- [ ] **Step 4: Commit the README**

Run:

```bash
git add README.md
git commit -m "docs: add bilingual project README"
```

---

### Task 4: Complete the Private Pre-Publication Audit

**Files:**
- Modify if necessary: `.gitignore`
- Add: `docs/superpowers/plans/2026-07-14-github-open-source-release.md`
- Inspect: all tracked files and all reachable commits.

**Interfaces:**
- Consumes: committed source fixes and open-source documentation.
- Produces: a clean, reproducible release candidate before history mutation.

- [ ] **Step 1: Check tracked and ignored local-only artifacts**

Run:

```bash
git ls-files | rg '(^|/)(\.env|local\.properties|id_rsa|.*\.(pem|key|p12|jks|keystore)|.*\.zip|.*\.apk)$' || true
git status --ignored --short
git ls-files -z | xargs -0 stat -f '%z %N' | sort -nr | head -30
```

Expected: no secrets, signing keys, APKs, run archives, `local.properties`, `src/model_data.c`, generated `.so`, or IDE directories are tracked. The 229746-byte model and Gradle Wrapper JAR are the only intentional binary assets of note.

- [ ] **Step 2: Scan current tracked content for credential patterns**

Run:

```bash
git grep -n -I -E '(AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|gh[pousr]_[A-Za-z0-9_]{30,}|-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----|password[[:space:]]*[:=]|api[_-]?key[[:space:]]*[:=]|secret[[:space:]]*[:=])' -- . ':(exclude)android/gradlew' || true
```

Expected: no matches requiring remediation.

- [ ] **Step 3: Scan every reachable commit for credential patterns and risky paths**

Run:

```bash
found=0
for commit in $(git rev-list --all); do
  if git grep -I -l -E '(AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{35}|gh[pousr]_[A-Za-z0-9_]{30,}|-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----)' "$commit" -- . >/dev/null; then
    printf 'credential pattern match in %s\n' "$commit"
    found=1
  else
    grep_status=$?
    test "$grep_status" -eq 1 || exit "$grep_status"
  fi
done
test "$found" -eq 0
git log --all --name-only --format= | rg '(^|/)(\.env|local\.properties|id_rsa|.*\.(pem|key|p12|jks|keystore)|.*\.zip|.*\.apk)$' || true
```

Expected: no matches. Any match blocks publication and requires an explicit remediation task.

- [ ] **Step 4: Run the full release candidate verification**

In an isolated worktree, `src/model_data.c` is absent by design. Read the
trusted local calibration output at
`/Users/chen/work/tiny-pick-vision/src/model_data.c`, record its SHA-256, and
use `apply_patch` to create the exact same ignored file temporarily in the
isolated worktree. Confirm `git check-ignore -q src/model_data.c` before the
Android gates. After verification, delete the temporary file with
`apply_patch` and confirm it was never staged or committed.

Run the complete command set from Task 1 Step 2 again. Expected: all commands exit 0.

- [ ] **Step 5: Commit the implementation plan and any justified ignore correction**

If `.gitignore` required a justified correction, run:

```bash
git add docs/superpowers/plans/2026-07-14-github-open-source-release.md
git add .gitignore
git diff --cached --check
git commit -m "docs: add open-source release plan"
```

If `.gitignore` did not require changes, run instead:

```bash
git add docs/superpowers/plans/2026-07-14-github-open-source-release.md
git diff --cached --check
git commit -m "docs: add open-source release plan"
```

---

### Task 5: Restore GitHub Authentication and Confirm the Repository Is Private

**Files:** None.

**Interfaces:**
- Consumes: GitHub account `achen4020` and existing `origin` remote.
- Produces: authenticated CLI access and a confirmed private remote before destructive operations.

- [ ] **Step 1: Verify the current authentication failure**

Run:

```bash
gh auth status -h github.com
```

Expected before reauthentication: the existing token is invalid.

- [ ] **Step 2: Reauthenticate interactively**

Run:

```bash
gh auth login -h github.com -p https -w
```

Expected: browser/device authorization completes for account `achen4020`. This step requires user interaction and must not be bypassed with a pasted token in chat or a tracked file.

- [ ] **Step 3: Confirm identity, remote, visibility, and default branch**

Run:

```bash
gh api user --jq .login
gh repo view achen4020/tiny-pick-vision --json nameWithOwner,visibility,defaultBranchRef,url
git remote get-url origin
```

Expected:

```text
achen4020
nameWithOwner = achen4020/tiny-pick-vision
visibility = PRIVATE
default branch = main
origin = https://github.com/achen4020/tiny-pick-vision.git
```

Stop if any value differs.

- [ ] **Step 4: Fetch and confirm no unexpected remote divergence**

Run:

```bash
git fetch origin
git log --oneline --left-right --graph origin/main...main
git log --oneline --left-right --graph origin/impl/v1...impl/v1
```

Expected: local branches contain only the newly prepared commits; remote branches have no unexpected commits absent locally.

---

### Task 6: Rewrite Every Commit Email with Recoverable Backups

**Files:** Git object database and refs only; no working-tree content changes.

**Interfaces:**
- Consumes: clean committed branches `main` and `impl/v1`.
- Produces: rewritten local branches/tags with uniform Author/Committer email.

- [ ] **Step 1: Require a clean worktree and capture exact remote leases**

Run:

```bash
test -z "$(git status --porcelain)"
git fetch origin
git rev-parse origin/main > /tmp/tpv-origin-main-before.txt
git rev-parse origin/impl/v1 > /tmp/tpv-origin-impl-v1-before.txt
```

Expected: clean worktree; both files contain 40-character object IDs.

- [ ] **Step 2: Create backup refs outside refs/heads**

Run:

```bash
git update-ref refs/backup/pre-open-source/main "$(git rev-parse main)"
git update-ref refs/backup/pre-open-source/impl-v1 "$(git rev-parse impl/v1)"
git for-each-ref --format='%(refname) %(objectname)' refs/backup/pre-open-source
```

Expected: two immutable recovery refs point to the pre-rewrite branch tips.

- [ ] **Step 3: Rewrite Author and Committer emails on all local branches and tags**

Run:

```bash
FILTER_BRANCH_SQUELCH_WARNING=1 git filter-branch -f \
  --env-filter '
    export GIT_AUTHOR_EMAIL="der20044@msn.com"
    export GIT_COMMITTER_EMAIL="der20044@msn.com"
  ' \
  --tag-name-filter cat -- --branches --tags
```

Expected: `main` and `impl/v1` move to rewritten histories; backup refs remain on original object IDs.

- [ ] **Step 4: Verify history contents and metadata**

Run:

```bash
git log main impl/v1 --tags --format='%ae%n%ce' | sort -u
git log main --format='%H %an <%ae> %cn <%ce>' | head -10
git diff --exit-code refs/backup/pre-open-source/main^{tree} main^{tree}
git diff --exit-code refs/backup/pre-open-source/impl-v1^{tree} impl/v1^{tree}
```

Expected: the unique email output is exactly `der20044@msn.com`; both tree comparisons exit 0, proving file contents are unchanged by the rewrite.

- [ ] **Step 5: Re-run tests on rewritten `main`**

Run the complete Task 1 Step 2 verification suite again. Expected: all commands exit 0.

---

### Task 7: Push the Rewritten Private History

**Files:** Remote Git refs only.

**Interfaces:**
- Consumes: verified rewritten local branches and exact pre-rewrite remote object IDs.
- Produces: updated private remote branches/tags.

- [ ] **Step 1: Preview the push**

Run:

```bash
OLD_MAIN=$(cat /tmp/tpv-origin-main-before.txt)
OLD_IMPL=$(cat /tmp/tpv-origin-impl-v1-before.txt)
git push --dry-run origin \
  --force-with-lease=refs/heads/main:$OLD_MAIN \
  --force-with-lease=refs/heads/impl/v1:$OLD_IMPL \
  main:main impl/v1:impl/v1 --tags
```

Expected: dry run proposes forced updates only for `main` and `impl/v1`, plus intended tags. No deletion is proposed.

- [ ] **Step 2: Push with explicit leases**

Run the same command without `--dry-run`:

```bash
OLD_MAIN=$(cat /tmp/tpv-origin-main-before.txt)
OLD_IMPL=$(cat /tmp/tpv-origin-impl-v1-before.txt)
git push origin \
  --force-with-lease=refs/heads/main:$OLD_MAIN \
  --force-with-lease=refs/heads/impl/v1:$OLD_IMPL \
  main:main impl/v1:impl/v1 --tags
```

Expected: both branches update successfully; a lease rejection stops execution and must not be replaced with plain `--force`.

- [ ] **Step 3: Verify remote rewritten metadata while still private**

Run:

```bash
git fetch origin
test "$(git rev-parse main)" = "$(git rev-parse origin/main)"
test "$(git rev-parse impl/v1)" = "$(git rev-parse origin/impl/v1)"
gh api 'repos/achen4020/tiny-pick-vision/commits?sha=main&per_page=100' --paginate --jq '.[].commit.author.email,.[].commit.committer.email' | sort -u
gh api 'repos/achen4020/tiny-pick-vision/commits?sha=impl/v1&per_page=100' --paginate --jq '.[].commit.author.email,.[].commit.committer.email' | sort -u
gh repo view achen4020/tiny-pick-vision --json visibility --jq .visibility
```

Expected: branch tips match; remote commit email output is `der20044@msn.com`; visibility remains `PRIVATE`.

---

### Task 8: Make the Repository Public and Verify Anonymous Access

**Files:** GitHub repository settings only.

**Interfaces:**
- Consumes: verified private remote release candidate.
- Produces: public GitHub repository with description, topics, licensing, and anonymous clone access.

- [ ] **Step 1: Set repository description and topics while private**

Run:

```bash
gh repo edit achen4020/tiny-pick-vision \
  --description "Lightweight C vision SDK and Android bench app for embedded pick-and-place validation" \
  --add-topic computer-vision \
  --add-topic embedded-vision \
  --add-topic android \
  --add-topic camerax \
  --add-topic c-sdk \
  --add-topic object-detection \
  --add-topic face-detection \
  --add-topic tracking \
  --add-topic robotics \
  --add-topic tflite
```

Expected: command exits 0.

- [ ] **Step 2: Perform the final private gate**

Run:

```bash
gh repo view achen4020/tiny-pick-vision --json visibility,description,repositoryTopics,licenseInfo
git status --short
```

Expected: visibility is `PRIVATE`, description/topics are present, Apache-2.0 is detected or pending indexing, and the worktree is clean.

- [ ] **Step 3: Change visibility to public**

Run:

```bash
gh repo edit achen4020/tiny-pick-vision \
  --visibility public \
  --accept-visibility-change-consequences
```

Expected: command exits 0. This is the final irreversible external publication step authorized by the user.

- [ ] **Step 4: Verify GitHub and anonymous network access**

Run:

```bash
gh repo view achen4020/tiny-pick-vision --json visibility,url,description,repositoryTopics,licenseInfo
git ls-remote https://github.com/achen4020/tiny-pick-vision.git HEAD refs/heads/main refs/heads/impl/v1
curl -s -o /dev/null -w '%{http_code}\n' https://github.com/achen4020/tiny-pick-vision
curl -s -o /dev/null -w '%{http_code}\n' https://raw.githubusercontent.com/achen4020/tiny-pick-vision/main/README.md
```

Expected: visibility is `PUBLIC`; anonymous `git ls-remote` returns refs; both HTTP checks return `200`; GitHub reports Apache-2.0 after indexing.

- [ ] **Step 5: Report publication details**

Report:

```text
Repository: https://github.com/achen4020/tiny-pick-vision
Visibility: Public
License: Apache-2.0
History email: der20044@msn.com
Published branches: main, impl/v1
Model SHA-256: b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f
```

Keep `refs/backup/pre-open-source/*` locally until the user confirms the public repository is correct. Do not push backup refs.
