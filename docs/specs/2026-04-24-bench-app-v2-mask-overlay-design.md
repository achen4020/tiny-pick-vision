# Android Bench Test APP v2 — Mask Overlay + Diagnostics 设计文档

**日期**：2026-04-24
**父工程**：`tiny-pick-vision`
**前置 spec**：`docs/specs/2026-04-23-android-bench-test-app-design.md`（v1）
**状态**：待审

---

## 1. 背景与 v1 差距

v1 已在真机上跑通：`camera → YuvAdapter → tpv → TriggerMachine → RunRecorder → OverlayView` 全链路，A2 p95 = 4.84 ms、replay parity bit-identical。但 v1 的 overlay 只有"圆心 + 主轴 + 两行文字"，看参考 APP 截图（2026-04-22，工业产线同类工具）后发现差距主要在**可视化信息量**：

- 物品本身没高亮——只有一个圆点和一条线，不知道"识别到了多大、什么形状"
- 没有 ROI 概念——全画面扫描，杂背景里一堆小碎片轻易压过物品
- 没有诊断面板——二值化、连通域、面积过滤的中间结果无从检视
- 没有摆正的状态文字——`size/rotation` 这种"每帧能看"的识别摘要缺位

v2 对齐参考 APP 的形态，同时借机解决 v1 遗留的两个算法局限：(1) 二值化方向固定（亮=前景），导致"白底深物"场景完全识别不到；(2) 没有 ROI，杂背景帧直接走到 `TPV_SCENE_ERROR`（实测 74% 的丢帧率）。

v2 **不是** v1 的替代——v1 的核心契约（事件状态机、`.y`/`.jpg`/`log.jsonl`/`timing.bin` 事件格式、HG1-5 硬闸、20 KB size gate）全部保留；v2 是**可视化 + 算法**的加法。

---

## 2. 范围

### 2.1 In scope

- C 侧 debug API 扩展：导出 blob 的**像素 mask**（640×480 位图）
- 运行时 `dark_object_mode`：二值化反向（`Y < threshold` = 前景），支持白底深物
- 运行时 `roi: {x, y, w, h}`：只在 ROI 矩形内跑 CCL，外区域强制置零
- APP 主屏重绘：预览 + 黄色 ROI 框 + 物品上绿色半透明 mask 填充 + 红色中心点 + 主轴短线 + 顶部状态文字（size / bbox / grid / rotation）
- APP 诊断面板：2×3 共 6 个小图，覆盖 raw Y / ROI 内 / 二值化 / CCL 全部 blob / 面积过滤通过的 blob / 事件 mask
- 新按钮：`Diag` / `ROI` / `Clear`（对应参考图 Scan/Grids/Clear）
- `.mask` 文件落盘：每 COMMITTED 事件一张 38400 B raw bitmap
- `log.jsonl` 新字段：`detection.bbox`、`detection.area_px`、`detection.grid_8x8`、`artifacts.mask`
- `meta.json` 新字段：`tpv.dark_object_mode`、`tpv.roi`、`ui_version: "v2"`
- 向后兼容：v1 runs（无 `ui_version`）仍能被 `build/replay` 和未来的分析脚本解析

### 2.2 Out of scope

- **自适应 threshold（Otsu 等）**：v2 只做双向固定阈值。自适应 threshold 的耗时不确定，可能影响 A2 p95 预算；独立单开一个 v3
- **ROI 触摸拖动交互**：v2 用 Settings 里 4 个 EditText 填 `x/y/w/h`，触摸拖动留 v3
- **真标定流程改进**：v2 继续用 PC 侧 `tools/calibrate`；自动化采帧 + 在 APP 内触发 calibrate 留给产线形态
- **轮廓折线（polygon vertices）**：v2 只传全像素 mask；轮廓简化算法（Douglas-Peucker 等）开销和误差控制复杂，留 v3
- **生产 `.so`（armv7 release）的任何改动**：v2 一切新能力全部在 `-DTPV_DEBUG_FEATURES` 下，production path byte-identical、20 KB gate 不动

### 2.3 与 v1 的关系

- v1 的每一条硬约束（HG1-5、size gate、spec §14 的 4 条成功判据）在 v2 里继续有效
- v1 产生的 runs（meta.json 没 `ui_version` 字段）**继续可解析**：PC 侧工具以 "v1" 模式读；v2 runs 以 "v2" 模式读
- v1 的 TriggerMachine / RunRecorder 的事件模型（"每放一个物品恰好一条事件"）保留。Mask overlay 每帧实时刷，但 `.y`/`.jpg`/`.mask`/jsonl 写出仍以 COMMITTED 为触发

---

## 3. 架构增量

```
┌─────────────────────────────────────────────────────────────────┐
│                        v2 APP (Kotlin)                           │
│                                                                  │
│  ┌──────────────┐   ┌──────────────────┐   ┌─────────────────┐  │
│  │ CameraX      │──▶│ TriggerMachine   │──▶│ RunRecorder v2  │  │
│  │ (unchanged)  │   │  (unchanged)     │   │ + .mask file    │  │
│  └──────┬───────┘   └──────────────────┘   │ + bbox/area_px  │  │
│         │                    ▲              │ + grid_8x8      │  │
│         ▼                    │              └─────────────────┘  │
│  ┌──────────────┐   ┌──────┴────────┐                           │
│  │ YuvAdapter   │──▶│ TpvNative     │ (mask[], ROI in args)     │
│  │ + NV21       │   │ v2 JNI        │                           │
│  └──────────────┘   └──────┬────────┘                           │
│                             │                                    │
│  ┌──────────────┐   ┌──────┴────────┐  ┌────────────────────┐   │
│  │ MainView:    │   │ OverlayView   │  │ DiagnosticsView    │   │
│  │ preview +    │   │ v2: mask fill │  │ 2×3 grid: raw /    │   │
│  │ ROI yellow + │   │ + red center  │  │ ROI / bin / CCL /  │   │
│  │ status text  │   │ + axis line   │  │ filtered / event   │   │
│  └──────────────┘   └───────────────┘  └────────────────────┘   │
└──────────────────────────────┬───────────────────────────────────┘
                               │ JNI
                               ▼
          ┌────────────────────────────────────────┐
          │ libtpv.so (arm64-v8a, -DTPV_DEBUG_…)  │
          │                                        │
          │  tpv_process_frame_debug_v2(           │
          │    y, w, h,                            │
          │    dark_object_mode,                   │
          │    roi_x, roi_y, roi_w, roi_h,         │
          │    *out_debug,                         │
          │    *out_timing_ns)                     │
          │                                        │
          │  out_debug->mask[640*480/8]   NEW      │
          │  out_debug->bin[640*480/8]    NEW      │
          │  out_debug->all_blobs_mask[]  NEW      │
          │  + everything from v1                  │
          └────────────────────────────────────────┘
```

差异说明：
- **C 侧**：`tpv_process_frame_debug_v2()` 新函数（`_v2` 后缀避免破 v1 ABI），新入参 `dark_object_mode` + `roi`，新出参 `mask / bin / all_blobs_mask`（三张位图为诊断面板铺路）
- **UI**：`OverlayView` 的 `onDraw` 重写——不再画圆，改为 mask bitmap 以 `#00FF00 alpha=120` 填充 + 红色实心中心点 + 短主轴
- **新 View**：`DiagnosticsView`（自定义 `View`）渲染 6 格诊断小图；默认 `visibility = GONE`，按 `Diag` 按钮切换
- **数据契约**：`RunRecorder.recordEvent()` 签名新增 `mask: ByteArray`，写 `NNNNNN.mask` 文件；jsonl 新字段

---

## 4. 算法层扩展（C 侧）

### 4.1 新 debug 类型

在 `include/tpv_internal.h` 的 v1 `tpv_DetectionDebug` 基础上新增 v2 类型（保留 v1 类型不动）：

```c
#ifdef TPV_DEBUG_FEATURES

/* v2 adds three bitmaps that let the Android diagnostic panel render
 * the pipeline's intermediate stages without re-running the C code:
 *   - bin[]          post-threshold binary image
 *   - all_blobs_mask[] all CCL components (any size) — the "raw CCL" view
 *   - mask[]         only the final winning blob (after AMIN/AMAX filter
 *                    and argmax-over-ACCEPTED selection)
 * Each is LSB-first packed bit per pixel, 640×480 ⇒ 38400 bytes. */
typedef struct {
    tpv_Detection det;
    tpv_Features  features;
    int32_t       distances_sq[TPV_N_CLASSES];

    /* v2 additions below */
    int16_t  bbox_x0, bbox_y0, bbox_x1, bbox_y1;   /* winning blob, 640×480 coords */
    int32_t  area_px;                               /* winning blob m00 */
    int32_t  grid_8x8;                              /* foreground cells in an 80×60 grid */
    uint8_t  bin[TPV_WIDTH * TPV_HEIGHT / 8];       /* post-threshold bitmap */
    uint8_t  all_blobs_mask[TPV_WIDTH * TPV_HEIGHT / 8]; /* all CCL blobs */
    uint8_t  mask[TPV_WIDTH * TPV_HEIGHT / 8];      /* winning blob only */
} tpv_DetectionDebugV2;

/* v2 entry point. v1's tpv_process_frame_debug stays valid and unchanged
 * (tests/test_debug_api.c continues to use it). The C-side function takes
 * no timing pointer — JNI brackets this call with clock_gettime on both
 * sides to populate FrameTiming, same as v1. */
int tpv_process_frame_debug_v2(
    const uint8_t *y, int w, int h,
    uint8_t bin_threshold,         /* v2 takes threshold as arg — see §5.5 */
    int dark_object_mode,          /* 0 = Y ≥ thr is fg; 1 = Y < thr is fg */
    int roi_x, int roi_y, int roi_w, int roi_h,
    tpv_DetectionDebugV2 *out);

#endif  /* TPV_DEBUG_FEATURES */
```

Struct size ≈ **115 KB** (three 38400 B bitmaps dominate; metadata fields add ~100 B). Exact `sizeof(tpv_DetectionDebugV2)` is locked by a unit-test assertion in `test_debug_api_v2.c` (`CHECK(sizeof(tpv_DetectionDebugV2) < 116 * 1024)`) so silent padding changes are caught. Allocated once in the JNI layer as a module-static, so no per-frame heap.

### 4.2 `bin_threshold` + `dark_object_mode`

`tpv_threshold()` 保持不变（v1 行为；仍读 `extern const uint8_t tpv_bin_threshold`）。v2 debug 路径**不**复用 `tpv_threshold()`——因为 v2 需要既允许运行期改阈值（UI 可调），又允许反向二值化。v2 在 pipeline 层加一个独立 wrapper，阈值从参数来、不从全局 const 来：

```c
/* in src/pipeline.c under #ifdef TPV_DEBUG_FEATURES */
static void threshold_v2(const uint8_t *y, int w, int h,
                          uint8_t threshold, int dark_mode,
                          uint8_t *bin_out) {
    const int npix = w * h;
    const int nby = (npix + 7) / 8;
    memset(bin_out, 0, nby);
    for (int i = 0; i < npix; i++) {
        int is_fg = dark_mode ? (y[i] < threshold) : (y[i] >= threshold);
        if (is_fg) bin_out[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}
```

生产 `tpv_process_frame()` 不变（它继续走 `tpv_threshold()` + 编译进 .so 的 `tpv_bin_threshold`）；`tpv_process_frame_debug_v2()` 调 `threshold_v2(y, w, h, bin_threshold_arg, dark_mode_arg, bin_out)`。

**Threshold 取值哪里来**（数据流）：
- Settings 里用户可改 `binThreshold`，写进 `SettingsState.binThreshold: Int`（**run-locked**，与 nStable/kEmpty/mDriftPx 同级：run 中不可改；想改就 Stop → 改 → Start）
- `onStartClicked` 时快照到 `SettingsSnapshot`
- `onFrame` 每次把 `snapshot.binThreshold` 作为入参传给 `TpvNative.processFrameDebugV2`
- JNI 传给 C 侧
- meta.json 记录 run 启动时的快照（`tpv.bin_threshold`）
- jsonl **不**每事件记录（run 内恒定）

### 4.3 ROI 裁剪

`tpv_process_frame_debug_v2` 在 threshold 后、CCL 前做一次"ROI 外强制置零"：

```c
/* Clear pixels outside [roi_x, roi_x+roi_w) × [roi_y, roi_y+roi_h) */
for (int y_ = 0; y_ < h; y_++) {
    if (y_ < roi_y || y_ >= roi_y + roi_h) {
        /* whole row outside ROI, clear it */
        for (int x = 0; x < w; x++) bin_out[...] &= ~...;
    } else {
        /* partial row: clear [0, roi_x) and [roi_x+roi_w, w) */
    }
}
```

ROI 默认值由 APP 传（spec §6.4 定义）；若 APP 传 `{0, 0, 640, 480}`，等价于不做 ROI。

### 4.4 三张 bitmap 的填写

**精确定义**（避免歧义，§11 R4 的 escape hatch）：

| 位图 | 1 bit 像素的意义 |
|---|---|
| `bin[]` | 被 `threshold_v2` 判为前景、**且** 在 ROI 内（ROI 外强制 0）——就是实际送进 CCL 的那一份 |
| `all_blobs_mask[]` | `bin[]` 的子集；仅包含被 CCL 成功分配 label 且触发成功（`ccl_moments` 返回 ≥ 0）的前景像素。`ccl_moments` 返回 `-1`（label 溢出 / scene_error）时**整张清零**；`TPV_EMPTY` 或 `TPV_BAD_INPUT` 时也整张清零 |
| `mask[]` | `all_blobs_mask[]` 的子集；仅包含最终 winner blob 的 label 对应像素。`rc != TPV_OK` 时整张清零 |

三者层层包含：`mask ⊆ all_blobs_mask ⊆ bin`（§4.6 的测试断言这条不变式）。

**CCL label 图的获取**：`tpv_ccl_moments` 目前只输出 blob 列表和 moments，不回填 label 图。v2 方案：

在 `src/ccl_moments.c` 内部已有的静态 label 数组上游增一个**可选输出参数**，不改 v1 语义：

```c
/* new signature in include/tpv_internal.h */
int tpv_ccl_moments(const uint8_t *bin, int w, int h,
                    tpv_Blob *blobs_out, int max_blobs,
                    uint16_t *labels_out);   /* NEW: NULL to skip */
```

生产调用点（`src/pipeline.c::tpv_process_frame`）改为传 `NULL`：

```c
int n = tpv_ccl_moments(g_bin, w, h, g_blobs, TPV_MAX_BLOBS, NULL);
```

这**改动一行**生产代码。v2 debug 调用点传一个 `uint16_t labels[640*480]` 静态 buffer（614400 B，在 `#ifdef TPV_DEBUG_FEATURES` 下分配），pipeline 拿着 `labels[]` + `winner index` 就能扫一遍填 `mask[]` 和 `all_blobs_mask[]`：

```c
for (int i = 0; i < npix; i++) {
    uint16_t L = labels[i];
    if (L == 0) continue;               /* background */
    out->all_blobs_mask[i>>3] |= (1u<<(i&7));
    if ((int)L == winner_label) {
        out->mask[i>>3] |= (1u<<(i&7));
    }
}
```

**这违反了 v1 `tpv_ccl_moments` 签名不变的表述**——但不违反 v2 真正想守住的约束：**生产二进制行为 + 大小 + HG1-5 测试结果不变**。源码层加一个可空指针参数 + 生产调用点传 NULL，编译器优化下生产 .so 的 `.text` 变化可忽略（且 `make size` 实测继续 ≤ 20480 B）。硬约束 §7 相应调整。

### 4.5 生产路径约束（精确措辞）

**放宽说法**（相对 v1）：从"源码 byte-identical"→"生产行为 + 大小 + HG1-5 测试结果不变"。原因：§4.4 需要给 `tpv_ccl_moments` 加一个可空 `uint16_t *labels_out` 参数，生产调用点必须传 `NULL`——这改动了源码，但不改生产二进制行为。

**保证**：
- `src/pipeline.c::tpv_process_frame()` 行为（输入 → 输出）不变
  - 唯一源码 diff：`tpv_ccl_moments(..., NULL)` 传新的 `NULL` 参数
- `src/threshold.c::tpv_threshold()` 完全不变（v2 debug 路径走独立的 `threshold_v2` helper）
- `src/classifier.c::tpv_classify()` 完全不变
- `src/ccl_moments.c::tpv_ccl_moments()` 加一个可空出参；当参数为 `NULL` 时与 v1 行为等价
- `tpv_process_frame_debug_v2` 和所有静态 buffer（`labels[640*480]`, `bin[]`, `all_blobs_mask[]`, `mask[]`, `g_debug_v2_out`）全部在 `#ifdef TPV_DEBUG_FEATURES` 下
- HG1-5 `make test` 继续绿（34/34 + 新增 5 个 v2 测试 = 39/39）
- `make target && make size` 仍 ≤ 20480 B（实测 v1 为 13632 B；预期 v2 生产 .so ≤ 13700 B——`tpv_ccl_moments` 多一个 NULL check 分支、被编译器优化掉后增量 < 50 B）
- `tpv_process_frame` 的输入/输出在 v1 runs 上继续 bit-identical（`build/replay` v1 mode 不变）

### 4.6 新增测试

`tests/test_debug_api_v2.c`（host 端，带 `-DTPV_DEBUG_FEATURES`）：
- `t_v2_bright_square_matches_v1_decision`：亮底背景上一个亮方块 + `dark_mode=0`、ROI=全画面、`bin_threshold=128`，v2 的 `det.*` 与 v1 `tpv_process_frame_debug` 字段 bit-identical
- `t_v2_dark_object_mode_inverts_threshold`：**白底深物**场景（画面大部分 Y=200、中心一块 40×40 的 Y=30 物体）：
  - `dark_mode=0, threshold=128`：整张白底 Y>128 → 前景，占 ≈307200-1600 px 超 AMAX → 剔除；深物 Y<128 → 背景不参与 → 返回 `TPV_EMPTY`
  - `dark_mode=1, threshold=128`：整张白底 Y>128 → 背景；深物 Y<128 → 前景，40×40=1600 px ∈ [500, 50000] 通过几何过滤 → 返回 `TPV_OK`
- `t_v2_roi_clips_outside_blobs`：在 (100,100,200,200) 放一个小 blob，ROI={0,0,50,50}（不含 blob）→ TPV_EMPTY
- `t_v2_mask_matches_detection_area`：`popcount(mask[])` 与 `det.area_px` 在不大于 ±1 像素（CCL 边界对齐）
- `t_v2_all_blobs_superset_mask_superset_bin`：**三层包含不变式**——对所有 bit 位置 i，`mask[i] ⇒ all_blobs_mask[i] ⇒ bin[i]`

5 个测试，host 跑。

---

## 5. UI 重新设计

### 5.1 主屏布局（landscape，保留 v1 orientation lock）

```
┌────────────────────────────────────────────────────────────────────────┐
│ [Start] [Stop] [Export zip] [⚙]  [Diag] [ROI] [Clear]                  │  ← 顶栏(button row, left-align)
├────────────────────────────────────────────────────────────────────────┤
│ size:15290 [110×139] grid:150 rotation:-45°      FPS:24.1  skipped:3   │  ← status line
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│   ┌──────────────────────────────────────────────────────────────┐     │
│   │                                                              │     │
│   │                      camera preview                          │     │
│   │                                                              │     │
│   │     ┌───────────────────────────────┐ ← yellow ROI (2px)     │     │
│   │     │                               │                        │     │
│   │     │      ▓▓▓▓▓▓▓▓                 │ ← green fill (#00FF00  │     │
│   │     │     ▓▓▓▓●▓▓▓▓                 │   α=120) on blob mask   │     │
│   │     │      ▓▓▓▓▓▓▓▓                 │                        │     │
│   │     │        red axis line          │ ← red center dot +     │     │
│   │     │                               │   short axis line       │     │
│   │     └───────────────────────────────┘                        │     │
│   └──────────────────────────────────────────────────────────────┘     │
│                                                                        │
├────────────────────────────────────────────────────────────────────────┤
│ Last event: #42 cls=2 event_cls=2 flicker=false                        │  ← single-line HUD
└────────────────────────────────────────────────────────────────────────┘
```

- **Status line**（顶栏下方，monospace 字体）：每帧实时刷
  - `size:<area_px>` = blob m00
  - `[<bbox_w>×<bbox_h>]` = 紧接 size 的方括号内
  - `grid:<grid_8x8>` = mask 下采样到 80×60 后的前景格子数
  - `rotation:<theta_deg>°` = `theta_x10 / 10.0`
  - 右侧 `FPS: ... skipped: ...` 与 v1 相同
- **Preview 区**：CameraX PreviewView；透明的 `OverlayView` 盖在上面
- **HUD 单行**：最近一次 COMMITTED 事件摘要（取代 v1 的两行 HUD）

TPV_EMPTY / SCENE_ERROR / BAD_INPUT / DROP 帧：status line 显示 `size:- [-×-] grid:- rotation:-°`；preview 上没 mask 填充、没中心点（沿用 v1 `clearLive()` 的思路）。

### 5.2 OverlayView v2 改造

保留 v1 `updateLive` / `onCommit` / `clearLive` / `reset` 接口语义，但 `LiveState` / `onDraw` 重写：

```kotlin
private data class LiveState(
    val d: TpvDetectionDebugV2,     // v2 struct with mask
    val crop: YuvAdapter.CropRect,
    val nativeW: Int, val nativeH: Int,
    val roi: YuvAdapter.CropRect,   // ROI in 640×480 coords
)
```

`onDraw` 里：
1. 画 yellow ROI 矩形（按照 roi 缩放到 view 空间）
2. 把 `d.mask` 解码成 `Bitmap(640, 480, ALPHA_8)`（或 ARGB_8888 预乘绿色），缩放盖到 view 上，`alpha = 120`
3. 画 red center dot（半径 ≈ crop.w × 0.015 → native coords → view coords）
4. 画 red axis short line（长度 ≈ crop.w × 0.04，比 v1 短）
5. **不画圆圈**（v1 的圆圈被 mask 填充替代）

Mask bitmap 生成助手（在 `OverlayPainter` 里新增）：

```kotlin
fun maskToBitmap(mask: ByteArray, w: Int, h: Int, argb: Int): Bitmap {
    val pixels = IntArray(w * h)
    for (i in 0 until w * h) {
        val bit = (mask[i shr 3].toInt() ushr (i and 7)) and 1
        pixels[i] = if (bit == 1) argb else 0
    }
    return Bitmap.createBitmap(pixels, w, h, Bitmap.Config.ARGB_8888)
}
```

bitmap 分配每帧一次（`640*480*4 = 1.2 MB` 一次），但生命周期只在 `onDraw` 内；若性能压力大可缓存 + 每帧只改像素。v1 A2 p95=4.84ms 有充足预算。

### 5.3 DiagnosticsView（诊断面板）

一个自定义 `View`，按 2 行 × 3 列布局 6 个小图；每个小图 = 一张从 `TpvDetectionDebugV2` 派生的 `Bitmap` 缩放显示 + 文字 label。

6 格内容：

| # | 位置 | 内容 | 数据来源 |
|---|---|---|---|
| 1 | (0,0) | **Raw Y** 640×480 灰度 | 当前帧 `adapted.y` |
| 2 | (0,1) | **ROI 内** 灰度（ROI 外涂深色） | raw Y × ROI mask |
| 3 | (0,2) | **Binarized** 黑白 | `d.bin[]`（ROI 已裁剪）|
| 4 | (1,0) | **All CCL blobs** 随机色标 | `d.all_blobs_mask[]`（单色 + 面积色映射）|
| 5 | (1,1) | **Filtered blob**（winning） | `d.mask[]` 绿色 |
| 6 | (1,2) | **Last event mask** | 最近一次 COMMITTED 事件的 `mask`（committed 前空白）|

每格下方带一小行 label（14sp），如 `raw 640×480` / `bin thr=128` / `winning 15290px`。

DiagnosticsView 默认 `visibility = GONE`；点顶栏 `Diag` 按钮时：
- 相机预览区缩成半高度
- DiagnosticsView 出现在下半屏
- 再点一次 `Diag` 还原

诊断视图与相机预览同线程（camera executor 上写，UI 线程读，仍然用 AtomicReference）。6 张 bitmap 每帧重建代价较高（6 × 640 × 480 × 4 = 7.4 MB/帧），所以：
- 诊断视图隐藏时不生成 bitmap（检查 `visibility` 再做工作）
- 显示时降采样到 160×120（每格 160×120×4 = 76 KB × 6 = 460 KB/帧），目测足够
- 限频到 10 Hz（不是每帧；UI 动画流畅够用）

### 5.4 按钮集

保留 v1: `Start` / `Stop` / `Export zip` / `⚙ Settings`。

新增：
- **`Diag`**：切换诊断面板显隐；run 中/外都可用
- **`ROI`**：打开 Settings 对话框的"ROI"分页（v2 的 Settings 改成多页），显示 4 个 EditText（x, y, w, h）。**run-locked**，与其它 Pipeline 设置一致（Stop 后才能改，避免事件间 ROI 不一致导致 replay 断裂）。按钮 run 中灰掉
- **`Clear`**：清掉 `lastCommittedEvent` + OverlayView + DiagnosticsView 格 6 的"last event mask"。run 中/外都可用

### 5.5 Settings 扩展

`SettingsState` v2 增加：
- `darkObjectMode: Boolean`（默认 **true**——为用户的白底深物场景优化，spec §1 遗留问题）
- `binThreshold: Int ∈ [0, 255]`（默认 128，来自 `TPV_BIN_THRESH_DEFAULT`；允许用户手调）
- `roiX / roiY / roiW / roiH: Int`（默认 `0, 0, 640, 480` = 全画面）

Settings 对话框改 3 页（Tab）：
- "Trigger": `nStable` / `kEmpty` / `mDriftPx`（v1 已有）
- "Pipeline": `darkObjectMode`（Switch）/ `binThreshold`（EditText or Slider）
- "ROI": 4 个 EditText

**全部 Settings run-locked**——整 run 内不可改。想换参数就 Stop → 改 → Start，新 run 的 meta.json 记录新值。这让每 run 内每帧用的参数完全一致，replay 只要依据 meta.json 一份快照就够，不需要 per-event 写 ROI/threshold/dark_mode 字段。

### 5.6 `.jpg` overlay 重绘

v2 的 renderOverlayJpeg：
1. 从 NV21 解出彩色 bitmap（v1 路径）
2. 画 yellow ROI 框
3. 画 green mask 半透明
4. 画 red center dot + short axis
5. Status line 文字作为左上角 overlay：`size:N [w×h] grid:M rot:θ°`
6. Line 2（v1 的 event_cls/flicker）保留在左下

---

## 6. 数据契约变化

### 6.1 `log.jsonl` 事件行（示例）

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
    "confidence_q8": 0,
    "bbox": {"x": 265, "y": 180, "w": 110, "h": 139},
    "area_px": 15290,
    "grid_8x8": 150
  },
  "event_class_id": 2,
  "class_id_histogram": {"2": 2, "254": 1},
  "flicker": true,
  "features": { "hu": [...], "perim_ratio": "0x...", "eccentricity": "0x...", "m3_axis_sign": 0 },
  "distances_sq": [...],
  "artifacts": {
    "raw_y": "000042.y",
    "overlay": "000042.jpg",
    "mask": "000042.mask"
  }
}
```

相对 v1 的 delta：
- `detection.bbox`（新）：`{x, y, w, h}` 整数，640×480 坐标系
- `detection.area_px`（新）：blob m00，整数
- `detection.grid_8x8`（新）：mask 8×8 下采样后的前景格子数
- `artifacts.mask`（新）：同目录下的 `.mask` 文件名

### 6.2 `.mask` 文件

- 每 COMMITTED 事件一个 `NNNNNN.mask` 文件
- **固定 38400 B** raw bitmap（`640 × 480 / 8`）
- LSB-first 位打包，和 `tpv_threshold` 的输出格式一致（bit index `i` ↔ byte `i/8`, bit `i%8`）
- `1` = 该像素在 winning blob 内；`0` = 不在
- 解压脚本：`python3 tools/visualize_mask.py event.mask > event_mask.png`（新工具，见 §10）

### 6.3 `meta.json` 新字段

```json
{
  "run_id": "...",
  "ui_version": "v2",
  "device": { ... },
  "tpv": {
    "so_sha256": "...", "model_data_sha256": "...",
    "n_classes": 5, "bin_threshold": 128,
    "dark_object_mode": true,
    "roi": {"x": 0, "y": 0, "w": 640, "h": 480}
  },
  "trigger": { ... },
  "camera": { ... },
  "runtime": { "skipped_frames": ..., "total_frames": ... }
}
```

- `ui_version`: `"v2"`（新；v1 无此字段，解析工具按 v1 兼容）
- `tpv.dark_object_mode`: bool
- `tpv.roi`: 同 camera.crop 结构

### 6.4 `build/replay` 兼容

`replay` CSV 输出新增两列：`bbox_area_px,grid_8x8`（v1 runs 这两列留空）。核心字段（`class_id / x / y / theta_x10 / confidence`）不变。

**Replay 模式 vs 算法参数来源**（严格约定，否则 §9 的 replay parity 无意义）：

| 模式 | 触发条件 | `bin_threshold` 来源 | `dark_object_mode` | `roi` | 算法路径 |
|---|---|---|---|---|---|
| **v1 mode** | run dir 的 `meta.json` 没有 `ui_version` 字段，或 `ui_version` 不是 `"v2"` | C 侧 `extern const uint8_t tpv_bin_threshold`（`src/model_data.c` 编译时确定）| 固定 `0`（不反向）| 固定全画面 | 调用 v1 `tpv_process_frame` → `tpv_threshold()` |
| **v2 mode** | `meta.json.ui_version == "v2"` | 从 `meta.json.tpv.bin_threshold` 读 | 从 `meta.json.tpv.dark_object_mode` 读 | 从 `meta.json.tpv.roi` 读 | 调用 v2 `tpv_process_frame_debug_v2(..., bin_threshold, dark_object_mode, roi_x, roi_y, roi_w, roi_h, ...)` |

工具用法：

```bash
./build/replay <run_dir>       # 自动读 meta.json，选对模式
./build/replay --v1 <dir>      # 强制 v1 mode（诊断用；忽略 meta.json）
./build/replay --v2 <dir> \
    --bin-threshold 137 \
    --dark-object-mode 1 \
    --roi 0,0,640,480          # 全部显式覆盖（诊断用）
```

**关键保证**：
- v1 runs 的 `.y` 喂 **v1 mode** → bit-identical 还原（§7 保留 v1 replay parity 硬约束）
- v2 runs 的 `.y` + **v2 mode 从 meta.json 读** → `det.*` 字段与 run 时 APP 的 `log.jsonl.detection.*` bit-identical（§9 criterion 3）
- v1 mode **从不**读 meta.json 的 v2 字段——即使存在也忽略，保证 v1 语义稳定
- v2 mode **必须**读 meta.json 的三个参数，否则 replay 对非默认设置的 run 是歧义的（reviewer 发现点）

### 6.5 v1 ↔ v2 runs 并存

同一设备 `files/runs/` 下可以同时有 v1 和 v2 runs。分析脚本根据 `meta.json.ui_version` 字段选路径：
- v1 run → 不解析 mask 文件、不读 bbox/area_px/grid_8x8
- v2 run → 全部字段可用

---

## 7. 硬约束

| 项 | 状态 |
|---|---|
| `tpv_process_frame` 输入/输出行为 | **不变**（唯一源码改动：传给 `tpv_ccl_moments` 的新 NULL 参数）|
| `tpv_threshold()` / `tpv_classify()` | 完全不变（源码一字不改） |
| `tpv_ccl_moments()` 签名 | 加一个可空 `uint16_t *labels_out`；NULL 时与 v1 语义等价 |
| `make target` armv7 .so | 20 KB size gate 不动；预期增量 < 50 B（实测 v1 = 13632 B） |
| HG1-5 硬闸 | `make test` 继续绿：34/34 + v2 新 5 条 → 39/39 |
| v1 replay parity | 保留（`build/replay` v1 mode 对 v1 runs bit-identical） |
| v1 runs 在 v2 APP 里解析 | 保留（meta.json 无 `ui_version` → v1 模式） |
| landscape 锁定 | 保留 |
| 线程契约（camera 单线程链路） | 保留 |

---

## 8. 实施顺序（子任务拆分）

| # | 名称 | 产物 | 依赖 |
|---|---|---|---|
| T-v2.1 | C 侧 `tpv_process_frame_debug_v2` API + 3 张 bitmap 填充 | `include/tpv_internal.h` / `src/pipeline.c` / `src/ccl_moments.c` diff；`tests/test_debug_api_v2.c` 5 个测试 | v1 完成（现状） |
| T-v2.2 | JNI `TpvNative.processFrameDebugV2` 签名 + `tpv_jni.c` 新实现 | `TpvNative.kt` 新 data class `TpvDetectionDebugV2`；`tpv_jni.c` 新 JNI 函数 | T-v2.1 |
| T-v2.3 | `RunRecorder` v2 扩展（`.mask` 文件 + jsonl 新字段 + meta `ui_version`）| `RunRecorder.kt` diff；`RunRecorderTest.kt` +2 测试（mask 写入、v2 meta） | T-v2.2 |
| T-v2.4 | `OverlayView` v2 重绘（yellow ROI + green mask fill + red dot + axis） | `OverlayPainter.kt` 新助手；`OverlayView.kt` 重写 | T-v2.2 |
| T-v2.5 | `DiagnosticsView` 2×3 诊断面板 + `Diag` 按钮切换 | 新 `DiagnosticsView.kt` + `DiagnosticsRenderer.kt`（JVM 测逻辑分离）| T-v2.2, T-v2.4 |
| T-v2.6 | Settings 3 页 + `ROI` 按钮 + `Clear` 按钮 + status line 文字 | `SettingsState.kt` 扩展；`activity_main.xml` 布局；`MainActivity.kt` 绑定新按钮 | T-v2.4, T-v2.5 |
| T-v2.7 | PC 工具：`tools/visualize_mask.py`（mask → PNG 可视化） | `tools/visualize_mask.py` + DEVELOPER.md §11 增一小节 | T-v2.3 |
| T-v2.8 | 真机验收（spec §9 的 7 条成功判据） | run zip + timing 分析 + visualize_mask 输出 | T-v2.1-7 |

实施工序：v2.1（unblock everything）→ v2.2/v2.3（数据管道） → v2.4/v2.5（视觉）→ v2.6（UI 绑定）→ v2.7（工具）→ v2.8（验收）。

---

## 9. 成功判据（真机）

v2 完成后必须可复现：

1. **Smoke**：v2 APK 装机、`dark_object_mode=true`、白底深物场景、点 Start 放一本深色书、3 秒内屏幕出现**绿色半透明 mask 覆盖在书的轮廓上** + 红色中心点 + 黄色 ROI 框 + 状态文字 `size:N [w×h] grid:M rotation:θ°`
2. **Replay parity (v1 mode)**：按 §6.4 的 v1 mode（`meta.json.ui_version` 为空或非 `"v2"` 时 `build/replay` 自动进入）把某个 v1 run 的 `.y` 喂给 v2 `build/replay`，得到的 `class_id/x/y/theta/conf` 与 v1 log.jsonl 一致（向后兼容）
3. **Replay parity (v2 mode)**：v2 run 的 `.y` + 同 mode 喂 replay，`class_id/x/y/theta/conf/area_px/bbox/grid_8x8` 一致
4. **Mask 保真**：`tools/visualize_mask.py 000001.mask > m.png`，用肉眼看 PNG 的白色区域形状确实对应物品
5. **Diagnostics 面板**：点 Diag，6 格都正确渲染（raw Y → bin → all blobs → winner 的链路一目了然）
6. **A2 不劣化**：mask/bin/all_blobs 的填写 + ROI 裁剪 + 反向 threshold 的开销总和不让 p95 超过 10 ms（目标：p95 ≤ 10 ms，远低于 30 ms gate）
7. **ROI 生效**：把 ROI 缩到画面右下角 10%，左上放物品 → 识别不到；移回左上 → 识别到

---

## 10. 开放问题 / defer 清单

| # | 问题 | 处理 |
|---|---|---|
| O1 | Otsu / 自适应 threshold | v3；耗时不确定、要预研 |
| O2 | ROI 触摸拖动交互 | v3；v2 先用 EditText 够用 |
| O3 | Mask 压缩（PNG RLE 等） | v2 用 raw 38400 B；一个 run 100 events ≈ 4 MB，可接受 |
| O4 | 诊断面板 6 格之外（骨架 / Hu 矩可视化 / 速度热图 …）| v3 按需加 |
| O5 | 轮廓折线格式（替代位图 mask）| v3；需要 contour tracing 算法 |
| O6 | v2 spec 落地后 v1 spec 命运 | v2 实施完一起 merge，v1 spec 加 "superseded by v2, kept for historical context" 头注 |
| O7 | `grid_8x8` 定义 | v2 采用"mask 在 80×60 网格化下前景格子数"；每 8×8 源像素块内**任一**前景即视为该格为前景（OR 聚合）。其他定义（多数票 / 面积阈值等）留 v3 |
| O8 | 真标定流程 | 单独作业，不在 v2 spec；DEVELOPER.md 加一节"采帧 → PC calibrate → 重建 APK"的 checklist |

---

## 11. 风险清单

| # | 风险 | 缓解 |
|---|---|---|
| R1 | 115 KB 的 `tpv_DetectionDebugV2` 结构 + 38 KB `.mask` 每事件落盘，run 规模变大 | 估算：100 events × (300 KB `.y` + 150 KB `.jpg` + 38 KB `.mask` + 1 KB jsonl) ≈ 49 MB + timing.bin 4 MB ≈ 55 MB/run，可接受 |
| R2 | OverlayView 每帧 alloc 1.2 MB bitmap | 复用一个预分配 ARGB bitmap，每帧只改像素；若 GC pressure 明显再缓存 |
| R3 | DiagnosticsView 6 bitmap + 降采样开销 | 仅在 visibility==VISIBLE 时生成；降采样到 160×120 + 限频 10Hz |
| R4 | CCL label 图暴露给 pipeline 破坏 ccl_moments.c 的封装 | 新出参用 `#ifdef TPV_DEBUG_FEATURES` 限制；生产路径传 NULL 跳过回填 |
| R5 | ROI 裁剪后 m00/重心是否仍符合 v1 几何语义 | 单测 t_v2_roi_clips_outside_blobs 验证；tpv 其它内部几何与 ROI 无关（只看 ROI 内的二值图）|
| R6 | `dark_object_mode` 反向后，现有标定 model 是否可用 | 否——标定和识别必须用同一 mode。标定工具未来需要记录并校验 mode；v2 先不做，由用户手动保证一致（DEVELOPER.md 加提示）|
| R7 | Android 16（devicce Android SDK=36）对 `bindToLifecycle` 等 API 行为变更 | v1 已在同设备跑通，兼容性已验证 |

---

## 12. Appendix — 参考 APP 截图映射对照表

（供追溯用）

| 参考图位置 | 意义（我的理解）| v2 对应实现 |
|---|---|---|
| 顶部大预览 + 黄色 ROI 框 | 实时相机 + 感兴趣区域 | 主屏 §5.1 |
| 白纸上半透明黄色填充 | 物品 mask 可视化 | OverlayView mask 填充 §5.2 |
| 白纸中心红点 | blob 重心 (x,y) | 红色中心点 §5.2 |
| 中部文字 `size:15290[110x139], grid:147, rotation:70` | 识别参数 | status line §5.1 |
| Scan / Grids / Clear 按钮 | 功能切换 | Diag / ROI / Clear §5.4 |
| 底部 8 个诊断小图 | pipeline 各阶段输出 | DiagnosticsView 6 格 §5.3 (v2 简化) |
| 其中"白底上黑色物品形状" | 二值化 + 面积过滤 | 诊断格 3, 4, 5 |
| 其中"白底上黄色物品形状" | 最终 winning blob mask | 诊断格 5 |
