# Android 上机测试 APP — 设计文档

**日期**：2026-04-23
**父工程**：`tiny-pick-vision`
**状态**：待审

---

## 1. 背景与目标

主工程 `tiny-pick-vision` 已完成 C 库与 5 道硬闸，但从未在真实 Android 设备上运行过。工厂部署前必须先回答三个问题：

1. **能不能跑？** `libtpv.so` 在 arm64 真机上能否加载、调用 `tpv_process_frame` 返回合理结果。
2. **跑多快？** 单帧推理时延在典型 Android 设备上是否满足 spec §A2 的 ≤ 30 ms 预算。
3. **跑得对不对？** 对同一批真实物品，摄像头输入到算法输出的端到端决策是否稳定、失效模式是否可接受。

本文档定义一个 Android 宿主 APP，**专门服务于回答以上三问**，不是交付给产线的最终形态。APP 的运行形态是"开发者把机器架在桌面、手动往摄像头下面放物品、APP 自动记录每次识别事件"。

---

## 2. 范围

### 2.1 In scope

- Android 宿主工程（Gradle + Kotlin + CameraX + JNI）
- arm64-v8a 交叉编译的 `libtpv.so`
- 带 `-DTPV_DEBUG_FEATURES` 的 debug 出口 `tpv_process_frame_debug`，多暴露 10 维特征向量与各模板的 Mahalanobis 距离平方
- 事件驱动的触发状态机（不是每帧都记录）
- 每次事件的完整落盘：原始 Y 帧、带叠加的 RGB 预览、结构化 JSON 日志
- run 级 zip 打包，方便 `adb pull`
- 实时预览 + 识别叠加 + HUD

### 2.2 Out of scope

- 产线形态的 UI/UX（操作员向、多语言、工单集成）
- 云端上报、OTA、远程调参
- 在 APP 内跑标定工具——`src/model_data.c` 仍由 PC 端 `tools/calibrate` 产出
- 录制"训练集"用途——本 APP 是**评估**工具，不是**采集**工具（虽然落盘的原始 Y 帧可以二次利用）
- 多相机、外置 USB 相机
- 非 arm64 架构（32 位 armv7、x86_64 模拟器都不在目标内）

---

## 3. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                      Android APP (Kotlin)                    │
│                                                              │
│  ┌──────────────┐    ┌──────────────────┐   ┌────────────┐  │
│  │ CameraX      │───▶│ TriggerMachine   │──▶│ RunRecorder│  │
│  │ ImageAnalysis│    │ (IDLE/CAND/COMM) │   │ (jsonl+y+jpg)│
│  │  YUV_420_888 │    └──────────────────┘   └────────────┘  │
│  └──────┬───────┘            ▲                    │         │
│         │ Y plane, 640×480   │                    ▼         │
│         ▼                    │              zip → 私有目录  │
│  ┌──────────────┐    ┌──────┴───────┐                       │
│  │ YuvAdapter   │───▶│ TpvNative    │                       │
│  │  (crop+scale)│    │  (JNI)       │                       │
│  └──────────────┘    └──────┬───────┘                       │
│                              │                              │
│  ┌──────────────┐            │                              │
│  │ OverlayView  │◀───────────┘ (AtomicReference<Det>)       │
│  │  (canvas)    │                                           │
│  └──────────────┘                                           │
└──────────────────────────────┬──────────────────────────────┘
                                │ JNI
                                ▼
                ┌───────────────────────────┐
                │ libtpv.so (arm64-v8a)     │
                │ -DTPV_DEBUG_FEATURES      │
                │ tpv_process_frame_debug() │
                └───────────────────────────┘
```

数据流：**摄像头帧 → YuvAdapter 提取并下采样 Y 通道 → JNI → tpv 推理 → 回到 Kotlin → TriggerMachine 判事件 → 命中时 RunRecorder 落盘 → OverlayView 重绘**。

线程模型：CameraX 给的 `ImageAnalysis` 回调跑在一个后台 Executor（单线程、背压丢帧策略 STRATEGY_KEEP_ONLY_LATEST）。整条"camera → JNI → 状态机 → recorder"链路全部在这个单线程里，**不跨线程**；UI 线程只从 `AtomicReference<Detection>` 读一份快照绘制 HUD 和叠加。

---

## 4. 触发状态机

核心问题：**从"每帧一个 `tpv_Detection`"到"每放一个物品产生恰好一条事件"，中间需要一个去抖 + 新旧区分的状态机。**

### 4.1 状态

- `IDLE` — 视场空，等新物品出现
- `CANDIDATE` — 看到了非 EMPTY 帧，正在累积"稳定"证据
- `COMMITTED` — 证据充分，已触发一次记录事件；锁定，必须先回到"无物品"才肯再记

### 4.2 状态转换

**晋升判据（关键）**：只看"**物品在不在**"和"**位置稳不稳**"——**不**要求 class_id 在 N 帧里一致。这样 REJECTED ↔ AMBIGUOUS ↔ 某一类之间抖动的坏物体也能进 COMMITTED，不会被漏记。

**帧到状态机输入的映射**（单一来源规则，不再分散在正文里）：

| tpv 返回 | 状态机视角 | 备注 |
|---|---|---|
| `status == TPV_OK`（任何 `class_id`，含 `0..4`、`0xFE` AMBIGUOUS、`0xFF` REJECTED）| **"存在"** | REJECTED 也算存在——算法判不出也是"看到了一个物体" |
| `status == TPV_EMPTY` | **"空"** | 视场里什么都没有 |
| `status == TPV_SCENE_ERROR` 或 `TPV_BAD_INPUT` | **帧丢弃** | 不推进状态机、不计入窗口、不写 `timing.bin` 之外的任何日志 |

**位置稳** = 连续 N 帧里，每一帧的中心 (x,y) 与 **窗口内第一帧** 的中心满足 `|Δx| ≤ M ∧ |Δy| ≤ M`。
**窗口** = CANDIDATE 累积的最近 N 帧（环形缓冲）。

| 起点 | 条件 | 终点 | 动作 |
|---|---|---|---|
| `IDLE` | 当前帧是"存在" | `CANDIDATE` | 开窗口，压入该帧；`stable_count = 1` |
| `CANDIDATE` | 当前帧是"存在"且与窗口首帧位置差 ≤ M | `CANDIDATE` | 压入窗口；`stable_count += 1`；若 `stable_count ≥ N` → 下一行 |
| `CANDIDATE`(`stable_count ≥ N`) | (隐式)触发 | `COMMITTED` | **用窗口第 N 帧的数据**落盘 `.y` / `.jpg` / `detection` / `features` / `distances_sq`；同时从窗口整体算出 `event_class_id`（多数票）、`class_id_histogram`、`flicker`；`empty_count = 0` |
| `CANDIDATE` | 当前帧是"空"或位置差 > M | `IDLE` | 清窗口 |
| `COMMITTED` | 当前帧是"空" | `COMMITTED` | `empty_count += 1`；若 `empty_count ≥ K` → `IDLE` |
| `COMMITTED` | 当前帧是"存在" | `COMMITTED` | `empty_count = 0`（间歇 EMPTY 被清零，避免长期压着物不放时 K 次空帧意外跳回 IDLE） |

**`event_class_id` 投票规则**（APP 侧合成，非 tpv 输出）：
- `class_id_histogram = {0:n0, 1:n1, ..., 4:n4, 254:nAmb, 255:nRej}`（N 帧里每类出现次数；键为十进制字符串）
- `event_class_id` = 直方图中**票数最多**的类；平票时优先级 `0..4 > 0xFE > 0xFF`（真类 > AMBIGUOUS > REJECTED）
- `flicker = true` 当且仅当窗口内 class_id 的集合 size ≥ 2。让离线分析一眼看出不稳定物体

**`detection.class_id` 与 `event_class_id` 的关系**（重要、决定 replay 一致性）：
- `detection.class_id` = **窗口第 N 帧**的 tpv 原始输出。与 `.y` / `features` / `distances_sq` 在时间上严格对齐；单帧 replay 可复现
- `event_class_id` = 窗口多数票。事件级判决，无法由单帧 replay 复现——它就不是单帧量
- 两者在 `flicker == false` 时必然相等；在 `flicker == true` 时可能不等，这是正常的

> **为什么不同时要求 "存在" 超过半数**：N 默认 3，要求 "存在" 连续 3 帧已经足够过滤瞬态噪声；更严的半数门槛会让真抖动物体再次漏记。

### 4.3 默认参数（Settings 页面可调）

| 参数 | 默认 | 含义 |
|---|---|---|
| `N_stable` | **3** | 候选晋升所需连续"存在+位置稳"帧数。24fps 下约 125 ms |
| `K_empty` | **5** | COMMITTED 返回 IDLE 所需连续 EMPTY 帧数。24fps 下约 208 ms |
| `M_drift_px` | **30** | 稳定判定的最大中心漂移（L∞ 范数，单位：Y 通道像素，640×480 坐标系） |

### 4.4 不区分 ACCEPTED / AMBIGUOUS / REJECTED

状态机对三种 class_id 一视同仁——它们都是"检测到了一个物品"，都会触发事件。关键细节在 §4.2 晋升判据里已经体现：class_id 本身不进入晋升条件，抖动的物体**也会被 COMMITTED**，并在事件里通过 `class_id_histogram + flicker` 把抖动暴露出来。

---

## 5. 事件记录契约

### 5.1 目录与文件

```
${context.filesDir}/runs/run_<startTsIso8601>/
    000001.y            307200 字节 raw Y（640×480），每次 COMMITTED 写一个
    000001.jpg          JPEG，预览分辨率 + 识别叠加，每次 COMMITTED 写一个
    000002.y
    000002.jpg
    ...
    log.jsonl           每行一个 COMMITTED 事件（稀疏）
    timing.bin          帧级时延日志（每帧一条记录，密集）
    meta.json           run 级元数据
```

事件（`.y`/`.jpg`/`log.jsonl`）和时延（`timing.bin`）是**两条正交的记录流**：前者回答"识别到什么"，后者回答"算法多快"。

Stop 时：整个 run 目录原地打包成 `run_<startTsIso8601>.zip`，放同目录下；原始目录保留（不自动删，避免打包失败丢数据）。

### 5.2 `meta.json`（run 开始时写）

```json
{
  "run_id": "run_2026-04-23T16:12:08Z",
  "device": {
    "model": "Pixel 6",
    "android": 34,
    "abi": "arm64-v8a",
    "cpu_max_freq_khz": 2802000
  },
  "tpv": {
    "so_sha256": "<64 hex>",
    "model_data_sha256": "<64 hex>",
    "n_classes": 5,
    "bin_threshold": 137
  },
  "trigger": {"n_stable": 3, "k_empty": 5, "m_drift_px": 30},
  "camera": {
    "requested_w": 640, "requested_h": 480,
    "native_w": 1280, "native_h": 720,
    "crop": {"x": 160, "y": 0, "w": 960, "h": 720},
    "downsample_ratio_x": 1.5, "downsample_ratio_y": 1.5
  }
}
```

- `so_sha256`：由 APP 启动时对 `/data/app/.../lib/arm64/libtpv.so` 做 SHA-256 算出来
- `model_data_sha256`：从 APK 的 `assets/tpv_model_sha.txt` 读（由 `make android-so` 写入，见 §8）。**不经过 JNI**——`tpv_jni.c` 与 `libtpv.so` 分别编译，无法共享宏定义
- `bin_threshold` / `n_classes`：run 开始时通过 JNI 从 `libtpv.so` 的 `tpv_bin_threshold` / `TPV_N_CLASSES` 读出，不在 APP 侧硬编码
- `camera.crop`：当 native 非 4:3 时用到。若 native 本身就是 4:3（例如 1280×960），crop = `{x:0, y:0, w:1280, h:960}`，downsample_ratio = `native / 640`；若 native 是 16:9 或其它比例，按 §7.2 的中心裁剪算 crop rect。叠加坐标映射由这个 rect 精确定义（见 §5.5）

### 5.3 `log.jsonl` 一行（示例）

```json
{
  "event_idx": 42,
  "trigger_ts_ms": 1745394128012,
  "frame_idx_in_run": 1234,
  "detection": {
    "status": 0,
    "class_id": 254,
    "x": 320, "y": 240,
    "theta_x10": -450,
    "confidence_q8": 0
  },
  "event_class_id": 2,
  "class_id_histogram": {"2": 2, "254": 1},
  "flicker": true,
  "features": {
    "hu": ["0x00012345", "0xff...", "..7 entries.."],
    "perim_ratio":  "0x0001a2b4",
    "eccentricity": "0x0000dd74",
    "m3_axis_sign": 0
  },
  "distances_sq": [12345678, 987654, 456789, 111111, 999999],
  "artifacts": {"raw_y": "000042.y", "overlay": "000042.jpg"}
}
```

（示例中：窗口 3 帧里 class_id 是 `[2, 2, 254]`。**第 N 帧（第 3 帧）** 是 AMBIGUOUS(254)，所以 `detection.class_id = 254`。**多数票**是 2，所以 `event_class_id = 2`。`flicker = true` 因为直方图键数 ≥ 2。）

**字段来源严格对应**：

| 字段 | 来源 | replay 能否复现 |
|---|---|---|
| `detection.*` （所有子字段，含 `class_id`） | 窗口**第 N 帧**的 tpv 原始输出 | ✅ 可。把 `artifacts.raw_y` 喂 `build/replay` 得到的 tpv_Detection 必须字节相同 |
| `features` / `distances_sq` | 同上，第 N 帧 | ✅ 可 |
| `event_class_id` | APP 侧窗口投票 | ❌ 不可（它就不是单帧量），不纳入回放一致性判据 |
| `class_id_histogram` / `flicker` | APP 侧窗口统计 | ❌ 不可（同上） |

说明：
- **`detection` 的语义锁死为 "tpv 对 raw_y 的直接输出"**。这是为了让 `.y` 与 `detection`/`features`/`distances_sq` 三者构成一个可 bit-identical 重放的 triple
- **`event_class_id` 是 APP 侧的事件判决**，只在离线分析"这个物体被判成啥"时看
- **不记 `trigger_params`**——因为 §9 规定 Settings 只能在 run 未启动时改，整个 run 的 N/K/M 不变，`meta.json.trigger` 已经足够还原；事件行不需要重复
- `class_id_histogram` 的键是 class_id 的十进制字符串；取值是该 class_id 在窗口 N 帧里的出现次数，总和 = N
- `flicker`：窗口内 `class_id` 出现过 ≥ 2 种不同值时为 true
- **Q16.16 值以 `"0x..."` 字符串编码**，避免 JSON 浮点/整数歧义
- `distances_sq` 数组长度固定为 `TPV_N_CLASSES`（当前 5）；每项是 Q16.16 下的 d²
- **事件 index 从 1 起递增**，与 `000042.y` 文件名的前 6 位零填充数字对应

### 5.4 原始 Y 帧（`NNNNNN.y`）

- **固定 640×480 无头裸字节**，逐行 stride=640，大小恰为 307200 字节
- 就是送给 `tpv_process_frame_debug` 的**那一份**，字节级相同
- 方便 PC 端 `tools/replay` 直接 `fread`

### 5.5 叠加预览（`NNNNNN.jpg`）

- **画布**：相机原生帧（`native_w × native_h`），不是 PreviewView 的屏幕尺寸
- **内容**：原始相机 RGB + 在上面绘制：
  - 识别框：以物品中心为圆心、半径 = `crop.w × 0.05` 的圆
  - 主轴：从中心沿 θ 方向，长度 = `crop.w × 0.08`
  - 文本两行：
    - 行 1（帧级）——**分三档，按 `det_cls` 取值**：
      - `det_cls ∈ {0..4}`（ACCEPTED）：`det_cls=<det_cls> conf=<confidence_q8> d²=<distances_sq[det_cls]>`。这里 `distances_sq[det_cls]` 是合法下标（argmax-over-ACCEPTED 的赢家 d²）
      - `det_cls == 0xFE`（AMBIGUOUS）：`det_cls=254 conf=0 d²min=<min(distances_sq)>`。用 `d²min` 标签而不是 `d²`，显式与下标语义区分；数值是"最接近哪个 template"的诊断量
      - `det_cls == 0xFF`（REJECTED）：`det_cls=255 conf=0 d²min=<min(distances_sq)>`。同上
    - 行 2（事件级）：`event_cls=<event_class_id> flicker=<true|false>`
- **颜色选择**（识别框、主轴、行 1 文字颜色）——三档硬编码，覆盖 `det_cls` 全部合法取值，无 fallback：
  - `det_cls ∈ {0..4}`：**类别色** `palette[det_cls]`（需事先定义一张 5 色调色板，见 §12 开放项）
  - `det_cls == 0xFE` (AMBIGUOUS)：**黄色** (`#F5A623`)，warning 语义
  - `det_cls == 0xFF` (REJECTED)：**红色** (`#D0021B`)，error 语义

  三档正好对应"算法判出了某类 / 算法拿不定 / 算法拒绝"，颜色差让人眼一扫就能看出失效模式。行 2 文字**固定中性灰** (`#9B9B9B`)，不随任何字段变色，视觉上保持帧级 vs 事件级分层。

  **为什么颜色跟 `det_cls` 而不是 `event_cls`**：圆心位置、θ、d²/d²min 全部来自窗口第 N 帧，视觉元素必须跟**同一帧**的 tpv 判定对齐；抖动事件若按多数票上色，会让圆圈的颜色和 d² 配不上，误导分析者以为"这帧 tpv 判出了多数票类"。
- **一致性与 §9 HUD**：HUD 的 "Last" 行用**完全相同的两行布局**（同样 `det_cls` 在前、`event_cls` 在后），保证人看 APP 屏幕和看落盘 JPG 时的语义相同
- **JPEG 质量 85**（文件大小/清晰度折中，事后分析足够）
- **坐标映射（关键）**。tpv 输出的 `(x_640, y_640, theta)` 在 **640×480 坐标系**，需要映射到原生帧坐标系：

```
  x_native = meta.camera.crop.x + x_640 * (meta.camera.crop.w / 640.0)
  y_native = meta.camera.crop.y + y_640 * (meta.camera.crop.h / 480.0)
  theta_radians = theta_x10 / 10.0 * π / 180.0     # θ 不随裁剪变
```

- 当 native 本身就是 4:3 且尺寸 = 640×480 时，crop = `{0,0,640,480}`、缩放比 = 1，映射退化为恒等
- **不能简单"按比例放大"**——如果 native 是 16:9（例如 1280×720），中心裁剪会引入非零的 `crop.x` 偏移；漏掉它会让圆心画偏半个裁剪宽度

### 5.6 帧级时延日志（`timing.bin`）

**动机**：§1 提出要验证"单帧推理 ≤ 30 ms"（spec §A2）。这个指标只能由**每一帧**的 tpv 耗时来回答，而不是事件级的 `trigger_ts_ms`。事件触发是稀疏的，零事件 run 里事件级时间戳根本不存在；即便有事件，它们之间的差值也包含了 IDLE/COMMITTED 等待期，不能反映算法本身的耗时。所以**独立开一条帧级日志流**。

**格式**：二进制追加、定长记录，省空间省解析。

```
Header (32 字节，仅在 Start 时写一次):
  0..3     magic = "TTML"            (4 字节)
  4..5     version = 1               (uint16 little-endian)
  6..7     record_size = 48          (uint16 LE, 下方每条记录的字节数)
  8..15    run_start_ns              (uint64 LE, Start 时 System.nanoTime())
 16..31    reserved (zeros)

Record (48 字节，每帧一条):
  0..3     frame_idx_in_run          (uint32 LE, 从 1 起)
  4..7     tpv_status                (int32  LE, tpv_process_frame_debug 返回值)
  8..11    tpv_class_id              (int32  LE, -1 表示该帧未进入 tpv，仅相机/drop)
 12..19    t_camera_arrive_ns        (uint64 LE, ImageAnalysis 回调入口)
 20..27    t_jni_enter_ns            (uint64 LE, JNI Java→C 边界)
 28..35    t_tpv_enter_ns            (uint64 LE, C 侧 tpv_process_frame_debug 之前)
 36..43    t_tpv_exit_ns             (uint64 LE, tpv_process_frame_debug 之后)
  44..47   t_jni_return_ns_delta32   (uint32 LE, 相对 t_tpv_exit_ns 的增量, ns)
```

- 所有时间戳来自 `CLOCK_MONOTONIC`（Android 的 `System.nanoTime()` 和 C 的 `clock_gettime(CLOCK_MONOTONIC, ...)` 都是这一时钟），保证单调递增
- `t_jni_return_ns_delta32`：JNI 返回到 Java 的时间通常 < 4 秒，用 32 位增量省 4 字节；若真出现 overflow（>= 2³² ns ≈ 4.29 s）写 `0xFFFFFFFF` 标记溢出，离线分析时忽略该条
- 一条记录 48 字节；24 fps 一小时 = 86 400 条 ≈ 4 MB，磁盘可接受
- 被 CameraX `KEEP_ONLY_LATEST` 策略丢的帧**不**写记录（那一帧根本没看到）；但 APP 侧维护一个"skipped 计数器"，Stop 时写到 meta.json 里

**派生指标**（离线脚本计算）：

| 指标 | 公式 |
|---|---|
| tpv 纯耗时（**A2 的直接度量**）| `t_tpv_exit_ns - t_tpv_enter_ns` |
| JNI 入口到 C 调用开销 | `t_tpv_enter_ns - t_jni_enter_ns` |
| C 返回到 JNI 回 Java 开销 | `t_jni_return_ns_delta32` |
| Java→JNI→tpv 全链路 | `t_tpv_exit_ns + t_jni_return_ns_delta32 - t_camera_arrive_ns` |
| 帧间隔（有效帧率）| 相邻两条记录的 `t_camera_arrive_ns` 差 |

**验收门槛**：A2 过关 = `timing.bin` 里 tpv 纯耗时的 p95 ≤ 30 ms。

---

## 6. 对 tpv 库的侵入：`tpv_process_frame_debug`

### 6.1 新增类型

在 `include/tpv_internal.h` 尾部加：

```c
#ifdef TPV_DEBUG_FEATURES
typedef struct {
    tpv_Detection det;                         /* 生产路径的输出 */
    tpv_Features  features;                    /* 10 维特征向量 */
    int32_t       distances_sq[TPV_N_CLASSES]; /* 到每个模板的 d²（Q16.16） */
} tpv_DetectionDebug;
#endif
```

### 6.2 新增函数

在 `src/pipeline.c` 里加一个路径（`#ifdef TPV_DEBUG_FEATURES`），逻辑 = 生产路径 + 把每一步的内部数据也写入 `out`：

```c
#ifdef TPV_DEBUG_FEATURES
int tpv_process_frame_debug(const uint8_t *y, int w, int h,
                            tpv_DetectionDebug *out);
#endif
```

### 6.3 硬闸不受影响

- 默认生产构建（`make target`、`make size`）**不定义** `TPV_DEBUG_FEATURES`
- 这段代码被 `#ifdef` 完全剔除，`.text`/`.rodata` 不变
- 5 道硬闸的测试**不**加这个宏，HG1-5 全部仍在生产路径上验
- 只有 `make android-so` 目标会带 `-DTPV_DEBUG_FEATURES`；`libtpv.so`（arm64 debug）**不过 20 KB 闸**（这是测试用 .so，不是交付品）

### 6.4 新增一个测试

`tests/test_debug_api.c`（host 编译时条件加 `-DTPV_DEBUG_FEATURES`）：确认 `tpv_process_frame_debug` 对同一输入给出的 `det` 字段与 `tpv_process_frame` 一致（即 debug 路径不改变决策），且 `distances_sq` 数组长度与 `TPV_N_CLASSES` 匹配。

---

## 7. Android 工程拓扑

```
android/
├── settings.gradle.kts
├── build.gradle.kts                        顶级（versions catalog）
├── gradle.properties                       org.gradle.jvmargs, AGP version
├── gradle/wrapper/
└── app/
    ├── build.gradle.kts                    minSdk=24, targetSdk=34, abi=arm64-v8a only
    ├── proguard-rules.pro
    └── src/main/
        ├── AndroidManifest.xml             CAMERA permission, no BACK camera feature required
        ├── java/com/tpv/bench/
        │   ├── MainActivity.kt             入口：权限请求、Start/Stop/Export 按钮
        │   ├── CameraAdapter.kt            CameraX ImageAnalysis 封装
        │   ├── YuvAdapter.kt               YUV_420_888 → 640×480 packed Y
        │   ├── TpvNative.kt                JNI 调用封装 + 数据类
        │   ├── TriggerMachine.kt           §4 的状态机
        │   ├── RunRecorder.kt              §5 的落盘 + zip
        │   ├── OverlayView.kt              Canvas 绘制识别叠加
        │   └── Hud.kt                      FPS、状态、事件数的文字 HUD
        ├── cpp/
        │   ├── CMakeLists.txt              只负责编 tpv_jni.c；libtpv.so 作为外部 .so
        │   └── tpv_jni.c                   JNI glue（~80 行）
        ├── jniLibs/arm64-v8a/
        │   └── libtpv.so                   由 `make android-so` 复制而来
        └── res/
            └── layout/activity_main.xml
```

### 7.1 JNI 接口

Kotlin 侧（`TpvNative.kt`）：
```kotlin
data class TpvDetection(
    val status: Int, val classId: Int,
    val x: Int, val y: Int, val thetaX10: Int, val confidenceQ8: Int
)
data class TpvFeatures(
    val hu: IntArray, val perimRatio: Int,
    val eccentricity: Int, val m3AxisSign: Int
)
data class TpvDetectionDebug(
    val det: TpvDetection,
    val features: TpvFeatures,
    val distancesSq: IntArray
)

object TpvNative {
    init { System.loadLibrary("tpv") ; System.loadLibrary("tpv_jni") }
    external fun processFrameDebug(y: ByteArray, width: Int, height: Int): TpvDetectionDebug
    external fun binThreshold(): Int          // 读 extern const uint8_t tpv_bin_threshold
    external fun nClasses(): Int              // 读 TPV_N_CLASSES（编译进 libtpv.so 的那份）
}
```

C 侧（`tpv_jni.c`）：一个 `Java_com_tpv_bench_TpvNative_processFrameDebug` 函数，
用 `GetByteArrayRegion` 把 Y 拷贝进一个内部静态 buffer（避免每帧 new 数组），
调 `tpv_process_frame_debug`，把结果字段逐个塞进 Java 对象（用 `NewObject` + 事先缓存的 `jmethodID`）。

**SHA 的拿法——不走 JNI**（因为 `tpv_jni.c` 由 Android Studio 的 CMake 编、`libtpv.so` 由主 Makefile 编，两边 `-D` 宏不共享）：

- `so_sha256`：APP 启动时对 `applicationInfo.nativeLibraryDir/libtpv.so` 做 `MessageDigest.getInstance("SHA-256")`（纯 Kotlin）
- `model_data_sha256`：从 **APK 的 `assets/tpv_model_sha.txt`** 读一行字符串。这个文件由 `make android-so` 写入（见 §8），保证 .so 和 sha 永远同步
- 两者都在 RunRecorder 构造时读一次，传给 meta.json

### 7.2 YUV → Y 策略

CameraX 通过 `ImageAnalysis.Builder().setTargetResolution(Size(640, 480))` 请求 640×480 输出。实际得到的可能是最接近的原生分辨率（通常 640×480 本身，少数设备给 720×480 或 1280×960）。**YuvAdapter** 的任务：
- 从 `YUV_420_888` 第 0 个 plane（Y plane）取数据
- 若 `rowStride > width`，逐行拷贝去掉 stride padding
- 若分辨率 ≠ 640×480：中心裁剪到 4:3 → box-average 下采样到 640×480
- 输出一个**连续的 307200 字节 `ByteArray`**，交给 JNI

> 尽量让 CameraX 直接给 640×480，避免下采样的性能开销。只有在设备不支持该分辨率时才走裁剪+下采样路径。

---

## 8. Makefile 集成

主工程 `Makefile` 顶端新增：

```make
# arm64 Android debug .so for bench test APP
CC_TARGET_ARM64 ?= aarch64-linux-android24-clang

CFLAGS_TARGET_ARM64 = $(CFLAGS_COMMON) -Os -flto -ffreestanding \
                      -fno-exceptions -fno-asynchronous-unwind-tables \
                      -fomit-frame-pointer -fPIC -DTPV_DEBUG_FEATURES

# shasum on macOS, sha256sum on Linux — pick whichever is on PATH
SHA256 := $(shell command -v shasum >/dev/null 2>&1 && echo "shasum -a 256" || echo "sha256sum")
```

新增 target：

```make
build/libtpv-arm64-debug.so: $(SRCS) src/model_data.c | build
	$(CC_TARGET_ARM64) $(CFLAGS_TARGET_ARM64) -shared -o $@ $(SRCS) src/model_data.c

# android-so: copy the arm64 debug .so into the APK's jniLibs/, and write
# the model_data.c sha256 into an APK asset so the APP can read it at
# runtime (JNI side can't see the sha — tpv_jni.c and libtpv.so are built
# by different toolchains and can't share `-D` macros).
android-so: build/libtpv-arm64-debug.so
	mkdir -p android/app/src/main/jniLibs/arm64-v8a
	cp $< android/app/src/main/jniLibs/arm64-v8a/libtpv.so
	mkdir -p android/app/src/main/assets
	$(SHA256) src/model_data.c | awk '{print $$1}' > android/app/src/main/assets/tpv_model_sha.txt

android-apk: android-so
	cd android && ./gradlew assembleDebug
	@echo "APK at android/app/build/outputs/apk/debug/app-debug.apk"
```

产物对应关系：

| 文件 | 由谁写 | APP 怎么读 |
|---|---|---|
| `jniLibs/arm64-v8a/libtpv.so` | `make android-so` 的 `cp` | `System.loadLibrary("tpv")` |
| `assets/tpv_model_sha.txt` | `make android-so` 的 `shasum` | `assetManager.open("tpv_model_sha.txt").readBytes()` |
| so 自身的 sha256 | 运行时算 | `MessageDigest("SHA-256")` 喂 `libtpv.so` |

### 8.1 对现有目标的影响

- `make target`、`make size`、`make check-layout-target`：**完全不变**（仍是 armv7、生产宏关闭）
- `make test`、`make -C tools/calibrate test`：**完全不变**
- 新目标 `android-so` / `android-apk` 需要 NDK 里的 `aarch64-linux-android24-clang` 在 PATH 上；如果没装会直接 `command not found`，用户按 DEVELOPER.md §3 的指引装 NDK 就行（同一个 NDK 里 armv7 和 arm64 都有）

### 8.2 20 KB 硬闸

`libtpv-arm64-debug.so` **不过** 20 KB 闸（它是测试宿主，不是交付件）。生产 `libtpv-arm.so` 照常过闸。DEVELOPER.md 里会显式说明这一点。

---

## 9. UI

单 Activity，单屏，三块区域：

```
┌─────────────────────────────────────────────────┐
│ [Start]  [Stop]  [Export zip]   ⚙               │  ← 顶栏（⚙ = Settings 对话框）
├─────────────────────────────────────────────────┤
│                                                 │
│         ┌───────────────────┐                   │
│         │  相机预览         │                   │
│         │  + 叠加层         │                   │
│         └───────────────────┘                   │
│                                                 │
├─────────────────────────────────────────────────┤
│ State: CANDIDATE(2/3)   FPS: 24.1   skipped: 3  │  ← HUD
│ Events: 17                                      │
│ Last det_cls=254 conf=0 d²min=1.11e5            │  ← 帧级（AMBIGUOUS 用 d²min），与 .jpg 行 1 对齐
│ Last event_cls=2 flicker=true x=320 y=240 θ=-45°│  ← 事件级，与 .jpg 行 2 对齐
└─────────────────────────────────────────────────┘
```

- **State**：`IDLE` / `CANDIDATE(n/N)` / `COMMITTED(m/K)`
- **FPS**：过去 30 帧的移动平均
- **skipped**：被 CameraX `KEEP_ONLY_LATEST` 丢的帧累计数
- **Events**：本 run 累计事件数
- **Last 两行**：最近一次 COMMITTED 事件的摘要，**布局与 `.jpg` 叠加文本完全一致**——行 1 是帧级字段（`det_cls` / `conf` / `d²` 或 `d²min`，按 §5.5 的三档模板取），行 2 是事件级字段（`event_cls` / `flicker`）加位姿。行 1 的文字颜色按 §5.5 颜色规则的三档（`{0..4}` 类别色 / AMBIGUOUS 黄 / REJECTED 红）；行 2 固定中性灰
- **为什么不合并成 `cls=<det_cls>(→<event_cls>)` 这种紧凑格式**：合并后 `conf` / `d²` 会紧挨着视觉上看起来像是跟 `event_cls` 配对，重新糊化 §5.3 刚拆清的双字段语义。两行显示强制读者分开看帧级和事件级

**反馈动画**：每次 COMMITTED 触发，在预览叠加层闪一下绿色描边 **300 ms**，不冻结预览；同时 HUD 的 Events 数字加 1。

**Settings（⚙）**：三项 `N_stable` / `K_empty` / `M_drift_px`，默认值如 §4.3。**只在未 Start（即 run 尚未开始或已 Stop）时可编辑**；run 进行中按钮与三项输入框灰掉。理由：一个 run 内混用多组 N/K/M 会让 `log.jsonl` 无法还原"某条事件是在哪组门槛下产生的"——要么禁改、要么事件行按事件记参数，选最简单的那个（禁改）。想换参数就 Stop → 改 → 重新 Start，新 run 的 `meta.json.trigger` 如实记录新值。

**权限**：首次 Start 时请求 `CAMERA`。拒绝后 Start 按钮常灰，顶部红色文字提示"需要相机权限，请在系统设置里开启"。

**屏幕方向（landscape-only）**：`MainActivity` 在 `AndroidManifest.xml` 里锁死 `android:screenOrientation="landscape"`。背摄像头 buffer 原生是 640×480 landscape，`PreviewView` 与 `OverlayView` 用直接的 `width / nativeW`、`height / nativeH` 线性缩放（见 §5.5 / §8.4）。Activity 若为 portrait，CameraX 会把 buffer 旋转 90°/270° 显示，而 Overlay 仍按 buffer 坐标绘制，圆圈 / 轴会跑到错误位置；同时 portrait 下 `setTargetResolution(Size(640, 480))` 有可能被解释为 480×640 返回，触发 tpv 的 `TPV_BAD_INPUT`。本 spec 不要求 portrait，锁 landscape 是最简单且正确的方案。

---

## 10. 存储与运维

### 10.1 路径

- 运行中：`context.filesDir/runs/run_<ts>/` （APP 私有，系统自动随 APP 卸载清除）
- 打包后：`context.filesDir/runs/run_<ts>.zip`（同路径）
- 日志（非事件，debug 用）：`logcat` 自带，不落文件

### 10.2 取回 run zip

Debug APK 签名即可直接用 run-as 取：

```bash
adb shell run-as com.tpv.bench ls files/runs/
adb exec-out run-as com.tpv.bench cat files/runs/run_2026-04-23T16:12:08Z.zip > run.zip
```

### 10.3 磁盘保护

每次 Start 前检查 `context.filesDir` 剩余空间。预估：
- 一条 COMMITTED 事件 ≈ 500 KB（300 KB raw Y + ~150 KB JPEG + ~1 KB JSON）→ 1000 条 ≈ 500 MB
- `timing.bin` ≈ 4 MB/小时 @ 24 fps

整体上事件磁盘占比是大头。若剩余空间 < 1 GB，Start 按钮弹窗提示"建议清理旧 runs"。不做自动删除——数据不可再生。

### 10.4 崩溃安全

- `log.jsonl`：每条事件 `flush() + sync()`
- `.y`/`.jpg`：写完 `close()`
- `timing.bin`：每 **100 条记录**批量 `flush()`，代价是被杀时最坏丢 100 条（≈ 4 秒 @ 24 fps）；比每条 `flush` 便宜两个数量级，可接受

APP 被杀的情况下，已经落盘的事件和时延记录都在磁盘。Stop 时的 zip 打包是额外步骤，失败不影响原目录保全。

---

## 11. 显式 defer / 不覆盖

以下问题**不**在本 spec 范围内，留给将来或另立 spec：

1. **产线 UI**：操作员向、指示灯、工单号、NG 筐联动 —— 等算法在真机验收后再议
2. **远程上报**：把事件实时推到服务器做集中监控 —— 需要网络层设计，不在上机测试阶段
3. **外置 USB 相机**：某些产线会用固定位的工业相机 —— 需要 UVC 适配，超范围
4. **在 APP 内跑标定**：交互流程复杂，PC 侧 `tools/calibrate` 已够用
5. **内核级性能 profiling**：本 APP 的 `timing.bin` 已足够回答 spec §A2（p95 tpv ≤ 30 ms）；更细粒度的热点分析（单个 C 函数占比）走 simpleperf，不在 APP 内做
6. **多模型 A/B**：同一 APK 内切换多个 `libtpv.so` 对同一帧打分 —— 潜在有用但增加复杂度，先不做

---

## 12. 开放问题（不阻塞 plan 编写）

以下问题在实现时再定，本 spec 不锁死：

- **叠加 JPG 的具体分辨率**：CameraX 默认给预览多大就多大。如果设备预览是 1920×1080，单张 JPG ~200KB；如果 720×480 则 ~80KB。实现时按实际数据微调 JPG 质量（60–90 之间）以控制磁盘占用。
- **FPS 与帧丢弃**：CameraX `STRATEGY_KEEP_ONLY_LATEST` 遇到 tpv 延迟 > 1 帧间隔时会自动丢最旧的。这是可接受行为（事件驱动不要求所有帧都过）。HUD 上已经单独显示 `skipped` 计数（见 §9）；开放的是门槛——如果 `skipped / total_frames` 长期 > 某比例（例如 10%），应不应该自动提示用户"设备跟不上"。目前不做，留给回归分析。
- **零事件 run**：如果整 run 一次都没 COMMITTED，Stop 时仍正常打包（zip 里只有 meta.json、空 log.jsonl 和可能有内容的 timing.bin）——避免误删。
- **5 色类别调色板**（§5.5 引用）：实现时挑一组高区分度、色弱友好的颜色，建议起点 tab10 前 5 色 (`#1F77B4`, `#FF7F0E`, `#2CA02C`, `#D62728`, `#9467BD`)。注意 `#D62728` 是红色，和 REJECTED 的 `#D0021B` 过于接近——若选 tab10，把第 4 色替换成别的（例如 `#8C564B` 棕色）。调色板最终选型在实现时定，锁到 `OverlayView` 的一个常量数组里即可。

---

## 13. 硬约束清单

- **孤立性**：本 spec 的实现**不修改** `tiny-pick-vision` 的任何生产路径代码（`src/*.c` 现有函数签名、Makefile 现有 target、测试套件）。所有改动是**加法**：新增条件宏保护的 debug 函数、新增 Makefile target、新增 `android/` 子目录。
- **arm64-only**：APK 只打 `arm64-v8a`。armv7 设备跑不了，给出清晰报错（`UnsatisfiedLinkError` 的 message 已经够明显）。
- **构建顺序**：`make android-so` 依赖 `src/model_data.c`，也就是说**必须先跑过标定工具**才能生成 APP。`model_data.c` 缺失时 make 报 `No rule to make target` —— 同现有 `build/replay` 行为一致。

---

## 14. 成功判据

本 spec 实现完成后，以下四件事必须可复现：

1. **Smoke — 回答"能不能跑"**：把 APK 装到一台 arm64 Android 7.0+ 设备上，授予相机权限，Start → 视场放一本书 → 3 秒内屏幕上出现 COMMITTED 绿闪 → Stop 后 run 目录里有至少一条 jsonl + 一对 `.y`/`.jpg` + 非空 `timing.bin`
2. **回放一致性 — 回答"跑得对不对"**：把 `000001.y` 通过 `adb pull` 拿回 PC，用 `build/replay` 喂同一帧，得到的 tpv_Detection 与 APP `log.jsonl` 里的 **`detection.*` 字段**（含 `detection.class_id`、`x`、`y`、`theta_x10`、`confidence_q8`、`status`）**完全一致**（bit-identical）。**不比 `event_class_id` / `class_id_histogram` / `flicker`**——后者是 APP 侧的窗口合成量，不是单帧 tpv 输出，replay 语义上没法复现
3. **A2 直接验证 — 回答"跑多快"**：从 `timing.bin` 解析出每帧的 `(t_tpv_exit_ns − t_tpv_enter_ns)`，在至少 1000 条记录上计算 **p95 ≤ 30 ms**。这一判据**不依赖**事件是否发生——即使整 run 零事件也能成立
4. **抖动可见性 — 回答"失效模式能不能被记录到"**：在同一 run 里故意放一个会让算法抖动的物体（例如 REJECTED ↔ AMBIGUOUS 间摆动），至少 1 条事件必须落盘且 `flicker == true`、`class_id_histogram` 里有 ≥ 2 个键
