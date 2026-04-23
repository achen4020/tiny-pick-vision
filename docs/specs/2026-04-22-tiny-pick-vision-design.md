# Tiny Pick Vision — 设计规范

日期：2026-04-22
状态：草稿（待用户复核）

## 1. 背景

工业机械臂需要识别并定位放在工作面上的物品，以完成抓取动作。视觉模块是嵌入
在机械臂控制器（一块极低端 Android 板卡）上运行的纯 C 代码。控制器读取
本模块输出的 `(类别, x, y, θ, 置信度)` 后规划抓取。

本文件**只覆盖视觉模块**。机械臂侧的运动规划、夹爪控制、抓取后复核均不在
本规范范围内。

## 2. 目标与非目标

### 目标

| # | 需求 | 指标 |
|---|---|---|
| G1 | 二进制尺寸（strip 后） | ≤ 20 KB |
| G2 | 第三方依赖 | 无（仅 libc 与 NDK 工具链） |
| G3 | 目标硬件 | 最差：单核 ARMv7 @ ~800 MHz，512 MB RAM |
| G4 | 每帧延迟 | 目标板上 ≤ 30 ms |
| G5 | 端到端可靠性 | **系统级**达到 6σ 水平（见 §7） |
| G6 | 确定性 | 输入 bit 相同 → 输出 bit 相同 |

### 非目标

- 未知类别的通用物体检测。
- 料框乱堆 / 堆叠 / 遮挡 / 3D 位姿识别。
- 板端训练或在线增加新品类。
- 超出工位固定光源之外的光照鲁棒性。
- 基于色彩的区分（仅处理 Y 通道）。

## 3. 适用范围与前提

下列全部为**硬性前提**——若被违反，本模块可能输出 `REJECTED`，但不保证
结果正确。

### 3.1 场景

- 物品平放在**固定颜色的背景板**上（例如黑色防静电垫）。
- 物品**互不接触**、**不堆叠**、**不重叠**。最小间距（工作分辨率下 ≥ 2 px，
  典型 FOV 下 ≈ 1.5 mm）由**上料工装 / 送料机 / 料盘设计**保证，而非视觉
  模块。理由是：在 20 KB 预算下，靠物理可证的间距远比算法分水岭更稳；同时
  这从 6σ 账面上彻底移除了一整类失效模式（无声地把粘连轮廓并为一个 blob）。
- **每周期只抓一件**：模块在每帧中挑选置信度最高的一个目标。
- 每帧物品数：1 ~ ~30。
- 物品类别：**1 ~ 5 类已知**，对每条产线为固定值，编译期确定。支持单类别
  产线（N_CLASSES = 1），此时 AMBIGUOUS 判别退化失效（见 §6、§8、§9.1 的
  单类别条款）。
- 物品形状以**几何形为主**（规则或不规则多边形、带圆角的形状）。

### 3.2 光学

- 相机是 **eye-in-hand**（装在机械臂末端）。
- 每次检测周期开始前，机械臂走到**固定俯视位**，使工作距离和光轴对每条
  产线恒定。
- 传感器：外接 USB/UVC 相机，可直接取 Y 通道（YUV 或灰度）。
- 分辨率：**工作分辨率 640 × 480**。高分辨率相机在入口处降采样到此。
- 光照：**工位固定光源**，无直射阳光，无环境光漂移。

### 3.3 默认假设（待用户确认）

下列为头脑风暴中提出、并作为默认值采纳。实施前应由用户校验。

| 编号 | 假设 |
|---|---|
| A1 | 6σ 在**系统级**达成：算法给出置信度并可拒绝，机械臂侧在抓取后用力反馈 / 到位传感 / 二次成像复核。**算法本身不独立追求 6σ**。 |
| A2 | 节拍预算：视觉侧 **≤ 30 ms**，为机械臂动作在典型 100 ms/件中留余量。 |
| A3 | 标定在 **PC 端离线**进行，使用与运行时完全相同的特征提取代码；模板以 `const` 数据编译进固件；**不支持现场增加新品类**。 |
| A4 | 输出接口：每帧一条检测结果，采用串口二进制帧 **或** TCP JSON；两者共用同一 payload 模式，最终二选一待定。 |
| A5 | 工作分辨率固定为 **640 × 480**。 |
| A6 | 工具链：Android NDK `armv7a-linux-androideabi-clang`，`-Os -flto`，关闭 RTTI 与异常，向 freestanding 友好。 |

## 4. 架构

### 4.1 分层与尺寸预算

每个模块都是**纯函数**：相同入参 ⇒ 相同输出。**无堆分配**、**无跨帧隐式
状态**——工作缓冲以固定大小 `.bss` 数组形式分配（详见 §6 末尾），每次
`tpv_process_frame` 入口清零（§9.2 保证）后仅供当次帧内使用。从接口层面
观察，这些 `.bss` 数组等价于栈上的临时空间，之所以放 `.bss` 是为了避免
在 0.5 MB 量级的帧级缓冲上动用栈，而不是为了保留跨帧状态。

```
┌────────────────────────────────────────────────┐
│ platform_glue.c     ~2 KB   相机 I/O、结果输出  │
├────────────────────────────────────────────────┤
│ pipeline.c          ~1 KB   每帧调度            │
├────────────────────────────────────────────────┤
│ threshold.c         ~0.5 KB Y 图 → 位图         │
│ ccl_moments.c       ~3 KB   CCL + 矩累加        │
│ shape_features.c    ~1 KB   矩 → 特征           │
│ classifier.c        ~1 KB   马氏距离 + 拒绝     │
│ pose.c              ~0.5 KB 位姿 + 180° 消歧    │
├────────────────────────────────────────────────┤
│ model_data.c        ~1 KB   模板常量            │
└────────────────────────────────────────────────┘
小计：               ~10 KB
```

经 `-Os -flto -s` 后，预期 `.text` ≈ 8 KB、`.rodata`（模板）≈ 1.3 KB，
合计 ≈ 9.3 KB，在 20 KB 上限下还有约 10 KB 余量。

### 4.2 模块间契约

每个契约就是一个纯 C 函数。任一模块**不读不写**自己声明输出之外的内存。
正是这点让每个模块都能独立单测、也能整体替换。

```c
void threshold(const uint8_t *y, int w, int h, uint8_t *bin_out);

int  ccl_moments(const uint8_t *bin, int w, int h,
                 Blob *blobs_out, int max_blobs);

void shape_features(const Blob *blob, Features *features_out);

// classify 除了决策与置信度之外，还必须把最近类的平方距离 d1² 回传，
// 供上层在"全部被拒/歧义"时按 d1² 最小做兜底选择（见 §5 最终选择策略）。
void classify(const Features *features, const Template *templates, int n_templates,
              uint8_t *class_id_out,   // 0..4 / 0xFE / 0xFF
              uint8_t *confidence_out, // 0..255，公式见 §6
              int32_t *d1_sq_out);     // 最近类的平方马氏距离（Q16.16）

void pose(const Blob *blob,
          int16_t *x_out, int16_t *y_out, int16_t *theta_x10_out);
```

## 5. 单帧数据流

```
  Y 图 (640×480, uint8)
     │
     ▼  threshold  — 单次扫描，一次处理 8 px（int32 位并行）
  位图 (38.4 KB)
     │
     ▼  ccl_moments  — 两遍 Rosenfeld-Pfaltz + 并查集；
     │                 每个标号累加 m00, m10, m01, μ20, μ11, μ02,
     │                 μ30, μ21, μ12, μ03, perimeter（4 邻域）,
     │                 以及 bbox
  Blob[N]
     │
     ▼  尺寸过滤：Amin ≤ m00 ≤ Amax（均为编译期常量）
  有效 Blob[K]
     │
     ▼  shape_features — log|Hu[0..6]|, perim/√area, eccentricity,
     │                   μ3 沿主轴方向的符号
  Features[K]
     │
     ▼  pose — 对每个 size-filter 存活的 blob 计算 (x, y, θ)
     │         （pose 只依赖矩，先算好不花钱，也方便 REJECTED 帧操作员定位）
  带 pose 的 Blob[K]
     │
     ▼  classify — 对 K 个模板计算**平方**马氏距离；
     │             REJECT   当 min_dist² ≥ Template[winner].reject_thresh
     │             AMBIGUOUS 当 上一条不成立，且 N_CLASSES ≥ 2，且
     │                        (dist²₂ − dist²₁) < Template[winner].margin
     │             ACCEPTED  当上述两条都不成立
     │             判决顺序严格 L3 先于 L3'，详见 §9.1
     │             同时按 §6 公式计算 confidence_q8
  Detection[K]
     │
     ▼  最终选择（argmax 策略）：
     │   1) 若存在任何 ACCEPTED（class_id ∈ {0..4}）：
     │        在 ACCEPTED 之中按 confidence_q8 取 argmax → 返回 TPV_OK
     │   2) 否则若存在 AMBIGUOUS/REJECTED：
     │        在它们之中按 d1² 最小者（最接近某类的 blob）取胜 → 返回 TPV_OK，
     │        class_id = 0xFE / 0xFF，(x, y) 供操作员定位
     │   3) 否则：返回 TPV_EMPTY
     │   关键：REJECTED/AMBIGUOUS 绝不参与 ACCEPTED 的 argmax，
     │   高"置信"的拒绝永远压不过低"置信"的可抓物。
  单个 Detection → platform_glue → 机械臂控制器
```

## 6. 数据结构

全部固定大小；全部在 `.bss`。

```c
// 字段顺序刻意把 int64 放在最前，避免 armv7a-linux-androideabi (AAPCS)
// 下因 8 字节对齐而插入 4 字节 padding；否则 sizeof(Blob) 会是 88 而不是 80。
// 下方 _Static_assert 锁住这一布局，任何误改都会在编译期失败。
typedef struct {
    int64_t mu20, mu11, mu02;                  // offset  0..23  2 阶中心矩（int64，见 §7 "矩位宽"行）
    int64_t mu30, mu21, mu12, mu03;            // offset 24..55  3 阶中心矩（int64，最坏可达 ~4e10）
    int32_t m00, m10, m01;                     // offset 56..67  0~1 阶原始矩（Amax ≤ 50000 px 时 int32 够用）
    int32_t perimeter;                         // offset 68..71  4 邻域边界像素数，CCL 第二遍累加
    int16_t bbox_x0, bbox_y0, bbox_x1, bbox_y1; // offset 72..79
} Blob;                                        // 24 + 32 + 12 + 4 + 8 = 80 B（AAPCS 下无 padding，结构体 8 字节对齐）
_Static_assert(sizeof(Blob) == 80, "Blob layout drift — fix field order");

#define N_FEAT  10
#define M3_EPS  /* Q16.16 */ 0x00001000  // 3 阶矩沿主轴投影的"接近零"阈值
                                         // 绝对值低于此值即判定为对称 → sign=0

typedef struct {
    int32_t hu[7];         // log|Hu_k|，Q16.16，带符号
    int32_t perim_ratio;   // 周长 / √面积，Q16.16
    int32_t eccentricity;  // Q16.16
    int32_t m3_axis_sign;  // 当前实现：恒为 0（详见 §7「180° 消歧」行的现状）。
                           //   语义槽位保留 −1 / 0 / +1，以便未来补回投影实现时
                           //   不必改动 Features 的 ABI。
} Features;                // 40 B
// 注意：m3_axis_sign 是离散值但同在特征向量中参与马氏距离。若某类物品训练样本
// 几乎全是 sign=0（纯对称），该维协方差会退化 → Cholesky 不可解。见 §8 第 4 步
// 的协方差正则化。

typedef struct {
    Features mean;
    int32_t  L_inv[N_FEAT*(N_FEAT+1)/2];  // 下三角 L⁻¹，Q16.16
    int32_t  reject_thresh;               // **平方**马氏距离，Q16.16
                                          // 标定保证 ≥ 1（见 §8 第 6 步下界检查）
    int32_t  margin;                      // AMBIGUOUS 判别阈值（平方距离单位，Q16.16）
                                          // N_CLASSES ≥ 2 时标定保证 ≥ 1；N_CLASSES = 1 时为 0
                                          // 见 §8 第 7 步下界检查及单类特判
} Template;                               // 40 + 55*4 + 4 + 4 = 268 B，×5 ≈ 1.34 KB
// 本规范全部距离默认为平方马氏距离，除非明确说明。
// 运行时任何地方都不做 sqrt，比较始终在平方空间进行。

typedef struct {
    int16_t x, y;            // blob 质心（像素）；对 ACCEPTED/AMBIGUOUS/REJECTED 三种都是几何质心，
                             // 始终有意义（见 §10.1 关于机械臂使用规则）
    int16_t theta_x10;       // 主轴角 × 10，范围 −900..899（mod π；未消歧，见 §7「180° 消歧」）。
                             //   仅当 class_id ∈ {0..4} 可用于抓取，且机械臂需自行决定 ±π。
    uint8_t class_id;        // 0..4 合法；0xFE = AMBIGUOUS；0xFF = REJECTED
    uint8_t confidence_q8;   // 0..255，值越大越可靠；定义见下
} Detection;                 // 8 B

// confidence_q8 的定义（运行时可直接算）：
//   令 d1² = 最近类的平方马氏距离
//       d2² = 次近类的平方马氏距离（仅 N_CLASSES ≥ 2 时有定义）
//       t   = Template[winner].reject_thresh
//       m   = Template[winner].margin（仅 N_CLASSES ≥ 2 时非零）
//
//   fit_q8 = clamp(⌊255 · (t − d1²) / t⌋, 0, 255)    // "离最近类均值多近"
//
//   sep_q8（N_CLASSES ≥ 2）= clamp(⌊255 · (d2² − d1²) / m⌋, 0, 255)
//   sep_q8（N_CLASSES = 1）= 255（恒定：单类产线无"次近类"概念）
//
//   按 class_id 分三种情况定 confidence_q8：
//     ACCEPTED  (class_id ∈ {0..4}) : max(1, min(fit_q8, sep_q8))  // 保证 ≥ 1
//     AMBIGUOUS (0xFE)              : min(fit_q8, sep_q8)          // 可为 0
//     REJECTED  (0xFF)              : 0（不走公式，直接置 0）
//
// 为什么 ACCEPTED 要 max(1, ...)：定点整除在 d1² 接近 reject_thresh 时会把
// fit_q8 舍入到 0，但此时按 §9.1 判决顺序仍算 ACCEPTED，这会与 §10.1 语义表
// "ACCEPTED ⇒ confidence_q8 ≥ 1"冲突。下界 1 用最小改动保住这一契约。
// REJECTED 不经公式：避免一个"高 sep_q8"的不可抓物得到非零置信度。

// .bss 工作缓冲，按 640×480 规格
//
// 两个独立的容量上限：
//   MAX_LABELS —— CCL 第一遍扫描期间临时标号数的最坏上限
//                 （受噪声驱动；匹配 uint16 标号空间）。
//   MAX_BLOBS  —— 并查集收敛后、真正暴露给上层的唯一 blob 数
//                 （预期 ≤ ~30；256 提供充足安全余量）。
// 任一上限逼近即触发 TPV_SCENE_ERROR，确保在任何缓冲越界前停下。
#define MAX_LABELS 65535
#define MAX_BLOBS  256

static uint8_t  g_bin[640*480/8];             //  38.4 KB
static uint16_t g_labels[640*480];            // 614.4 KB
static Blob     g_blobs[MAX_BLOBS];           //  20.0 KB  (256 × 80 B)
static uint32_t g_uf_parent[MAX_LABELS + 1];  // 262.1 KB  (65536 × 4 B)
// 工作集合计 ≈ 935 KB，远低于 512 MB 内存预算。
```

## 7. 关键算法决策

| 方面 | 决策 | 理由 |
|---|---|---|
| 数值类型 | 特征 / 模板 / 距离用 `int32` 定点（Q16.16）；仅 2 阶 / 3 阶矩累加器用 `int64`（见下"矩位宽"行） | 确定性、无 FPU 依赖、跨编译器位级可复现 |
| CCL | 两遍 Rosenfeld-Pfaltz + 并查集 + 路径压缩 | 代码量最小且可证明正确；最坏情况行为可分析 |
| Hu 矩存储 | `sign(h)·log(|h|+ε)`，压缩为 Q16.16 | 原始 Hu 矩跨 10 个数量级；对数压缩保证马氏距离数值良态 |
| 距离 | **平方**马氏距离；按类预置 Cholesky 逆 `L⁻¹`；distance = ‖L⁻¹(x−μ)‖² | 自动按类、按维度归一化；平方形式免 sqrt，并且是判定阈值最自然的 χ² 类统计量 |
| Confidence | 按 class_id 分三段定义（详见 §6 公式区）：<br>· ACCEPTED：`max(1, min(fit_q8, sep_q8))`（保证 ≥ 1）<br>· AMBIGUOUS：`min(fit_q8, sep_q8)`（可为 0）<br>· REJECTED：恒为 0（不走公式） | 避免定点舍入把 ACCEPTED 的 confidence 拉到 0（违反 §10.1 契约）；同时禁止一个"高 sep_q8" 的不可抓物得到非零置信；argmax 只在 ACCEPTED 子集进行（§5） |
| AMBIGUOUS 阈值 `margin` | 每类在标定时独立推导，存于 `Template.margin` | margin 与 reject_thresh 是两个独立的判决量，需分别可调 |
| 周长 | CCL 第二遍中顺带累加：前景像素若任一 4 邻域为背景则 +1 | 每像素多一次 4 向判定；不需要额外一遍图像扫描 |
| 矩位宽 | 2 阶 / 3 阶中心矩用 `int64`；原始矩用 `int32` | 640×480 全帧矩形 μ₂₀ ≈ 1e10；Amax = 50000 px 时 |μ₃₀| 可达 ~4e10，`int32` 会静默回绕 |
| 180° 消歧 | **当前实现：未启用**。`pose.c` 输出 θ ∈ [-π/2, π/2]（即模 π），`Features.m3_axis_sign` 恒为 0；机械臂端按其抓爪的对称性自行决定要不要 +π。未来如启用：用 μ₃ 在主轴方向上的投影符号 | 完整投影需要 R = √(A²+B²) 与跨过 int64 的三阶矩立方乘积，要么用 `__int128` + 128-bit isqrt，要么放开运行时 fp（违反 §9.2）。本版本选择把 360° 抓取位姿语义下放到机械臂端，换取严格 int 算术与更小代码足迹 |
| 二值化阈值 | 标定一次后静态烧入 | 固定光照场景不需要 Otsu / 自适应，省代码 |
| 旋转处理 | 仅用旋转不变特征，不存旋转模板 | 模板表极小、匹配代价 O(K) |

## 8. 标定流程（PC 端离线）

一个独立的 PC 工具，使用**与嵌入式端同一份 C 源码**的特征提取器，产出
`model_data.c`。步骤：

1. 操作员将每类物品以不同角度摆放 30–50 次，通过同款相机采集帧。
2. 工具对每帧跑 `threshold → ccl_moments → shape_features`。
3. 对每类计算特征均值向量 μ_c 与协方差矩阵 Σ_c。
4. **协方差正则化**：Σ_c ← Σ_c + ε·diag(σ²_ref)，其中
   ε = 1e-4（默认），σ²_ref 是每维的"参考方差"（例如由训练样本的极差平方
   推出）。这一步专门处理 `m3_axis_sign` 在纯对称类上退化为 0 方差、导致
   Cholesky 失败的情况；同时吸收数值噪声，不改变健康类的分类行为。若
   正则化之后 Σ_c 仍不正定，工具**显式失败**（"该类特征维度明显冗余，
   请检查特征提取或训练样本"）。
5. 做 Cholesky 分解：Σ_c = L Lᵀ，存下三角 L⁻¹（每类 55 个数，转为 Q16.16）。
6. `reject_thresh_c` = （该类所有训练样本在类内**平方**马氏距离的最大值）
   × 安全系数（默认 1.5）。量化为 Q16.16 后存入 `Template[c].reject_thresh`。
   单位：平方距离（Q16.16）。**永远不开平方根**。
   **量化下界检查**：若实数值量化后 `< 1`（即 < 2⁻¹⁶ 平方距离），工具
   **显式失败**并报告：`"class c 的 reject_thresh 量化为 0——类内
   方差过小，可能是训练样本过少、样本完全一致、或 log-Hu 后数值塌陷；
   请检查原始特征分布后再重标定"`。原因：若放行，运行时 L3 的
   `d1² ≥ 0` 会把该类所有样本都判为 REJECTED。
7. `margin_c` 推导（仅当 N_CLASSES ≥ 2）：
   `margin_c = α × min_{c'≠c} d²(μ_c, μ_{c'})`（在 c 的度量下），其中
   α = 0.25（默认，可按产线调）。这是 AMBIGUOUS 判别阈值：运行时若最近
   类和次近类的平方距离差小于 `margin_winner`，判为 AMBIGUOUS。每类单独
   存一份是必要的，因为 c 的 margin 反映"别人离 c 有多近"，不是对称量。
   **量化下界检查**：若量化后 `< 1`（Q16.16），工具**显式失败**并报告：
   `"class c 的 margin 量化为 0——最近邻类的均值与 c 太接近，AMBIGUOUS
   判别会等价于除零；建议加特征或删类。第 8 步的可分性检查本应拦下
   该问题，若同时触发则说明 reject_thresh 也过小"`。原因：若放行，
   运行时 sep_q8 公式 `255 · (d2²−d1²) / m` 的分母为 0。
   **N_CLASSES = 1** 时单一类没有"别人"，`margin_0 = 0`；运行时凭"单类
   特判：sep_q8 = 255，永不进入 AMBIGUOUS"回避（见 §6、§9.1）。单类产线
   的 `margin_0 = 0` 是合法的，不触发上述下界检查——下界检查只对
   N_CLASSES ≥ 2 的 margin 生效。
8. 可分性检查（仅当 N_CLASSES ≥ 2）：对每对类别 (c_i, c_j)，在 c_i 的度量下
   计算 μ_j 到 μ_i 的**平方**马氏距离（再反过来一次）。若任何一对满足
   `min(distance²) < 2 × max(reject_thresh_i, reject_thresh_j)`，工具
   **显式失败**（"当前特征无法区分这些类别；请增改特征或调整品类组合"）。
   这一步是守门员，防止静默部署一个无法达到拒绝纪律的模型。全部比较均在
   平方距离空间进行。N_CLASSES = 1 时跳过本步（没有类间距离可查）。
9. 输出 `model_data.c`，其中只有一个 `const Template templates[N_CLASSES]`。

嵌入式运行时与 PC 标定时**共享同一份特征提取代码**，是 6σ 可追溯性的基石。

## 9. 错误处理与 6σ 拒绝策略

### 9.1 拒绝阶梯

| 层级 | 条件 | 输出 |
|---|---|---|
| L1 预处理 | 几何过滤后一个有效 blob 都没有 | `EMPTY` |
| L1 预处理 | CCL 第一遍临时标号 ≥ MAX_LABELS | `SCENE_ERROR` |
| L1 预处理 | 并查集收敛后 blob 数 ≥ MAX_BLOBS（硬顶 256，期望最大 ~30） | `SCENE_ERROR` |
| L2 几何 | blob 面积 ∉ [Amin, Amax] | 静默丢弃该 blob |
| L3 分类 | 最小**平方**马氏距离 ≥ `Template[winner].reject_thresh` | `REJECTED (0xFF)` |
| L3' 分类 | 上一条不成立，且 N_CLASSES ≥ 2，且 (dist²₂ − dist²₁) < `Template[winner].margin` | `AMBIGUOUS (0xFE)` |

两点重要约定：

1. **判决顺序严格为 L3 先于 L3'**：若一个 blob 同时满足"离最近类太远"与
   "两最近类难分"，记为 `REJECTED`（而非 AMBIGUOUS），因为它连"落在
   某类内"都没做到，更细的歧义讨论没意义。
2. **L3 边界是闭集**：用 `≥` 而不是 `>`，使得 d1² 恰等于 reject_thresh
   的 blob 判为 `REJECTED`。这样 ACCEPTED ⇔ d1² < reject_thresh 严格
   成立，配合 §6 的 `max(1, ...)` 保证 ACCEPTED 的 `confidence_q8 ≥ 1`，
   与 §10.1 语义表一致。
3. **N_CLASSES = 1 特判**：L3' 永远为假（不存在次近类），模块只会在
   ACCEPTED / REJECTED 之间二分。sep_q8 在运行时恒为 255（见 §6）。

### 9.2 确定性保证

- 无浮点运算。
- 无时间、随机、线程局部状态。
- `process_frame` 入口处所有缓冲清零。
- CCL 标号分配顺序完全由扫描顺序决定，即完全由输入决定。

这些保证让团队能对任一录制的帧做**位级复现**，这是事故分析和回归测试的前提。

### 9.3 可观测性

编译期宏 `DEBUG_TRACE` 打开结构化日志：

- 每个检出 blob：`{blob_id, bbox, features, top-3 distances, decision}`。
- Release 构建中完全剥离，运行时零开销。

### 9.4 算法为什么不自追求 6σ

单次视觉调用本身达到 3.4 DPMO 既不现实，也更糟——**无法在可行样本量下验证**。
本设计明确把剩余 sigma 留给：

- **算法层拒绝而非猜测**（把误接受率压低）。
- **机械臂层抓取后复核**（力反馈、夹爪编码器位置、可选二次成像、称重工位）。
- **运行统计拒绝率超过阈值时的人工介入**。

这是工业线的标准做法，也是唯一在架构上诚实的 6σ 达成路径。

## 10. 接口

### 10.1 运行时入口

```c
// 返回码：
//   TPV_OK           (0)  → det_out 有效；class_id 携带决策
//                            （0..4 合法；0xFE AMBIGUOUS；0xFF REJECTED）
//   TPV_EMPTY        (1)  → 无 blob 通过几何过滤；det_out 清零
//   TPV_SCENE_ERROR  (2)  → CCL 临时标号溢出（> MAX_LABELS），
//                            或并查集后 blob 数超过 MAX_BLOBS；det_out 清零
//   TPV_BAD_INPUT    (-1) → w/h 与编译期 WxH 不一致，或指针为空
int tpv_process_frame(const uint8_t *y, int w, int h, Detection *det_out);
```

`EMPTY` 和 `SCENE_ERROR` **故意不进**`Detection.class_id`，以免场景级故障被
误读为某个物体的"被拒绝"。它们是一等返回码，控制器必须显式处理。

**TPV_OK 下三种 class_id 的 payload 语义**（严格约定，配合 §5 最终选择策略）：

| class_id | x, y | theta_x10 | confidence_q8 | 机械臂应当 |
|---|---|---|---|---|
| 0..4（ACCEPTED） | 选中 blob 的质心（供抓取） | 主轴角 × 10，模 π（−900..899）；机械臂端自行决定 ±π，详见 §7「180° 消歧」 | ≥ 1（由 §6 的 `max(1, …)` 保证） | 按 (x, y, θ mod π) 实施抓取，必要时叠加 ±π |
| 0xFE（AMBIGUOUS） | "最接近某类的那个 blob"的质心（供操作员定位） | 计算得到但仅供参考，不用于抓取 | 反映决策分离度，典型值较低 | **禁止**抓取；告警 / 请求人工介入 |
| 0xFF（REJECTED）  | 同上 | 同上 | 为 0（不满足 fit 条件） | **禁止**抓取；告警 / 请求人工介入 |

(x, y) 对三种情况都是几何质心，始终有物理意义；theta 对后两种计算后填入
payload，但仅做操作员排障提示，机械臂逻辑不得据此下抓取动作。

### 10.2 输出 Payload（9 字节，传输层通用）

```
offset  bytes  字段
  0      1    status     0=OK, 1=EMPTY, 2=SCENE_ERROR, 3=BAD_INPUT
  1      2    x          小端 int16，像素   （仅 status==OK 有效）
  3      2    y          小端 int16，像素   （仅 status==OK 有效）
  5      2    theta_x10  小端 int16，度 ×10（仅 status==OK 有效）
  7      1    class_id   0..4 正常；0xFE AMBIGUOUS；0xFF REJECTED（仅 status==OK 有效）
  8      1    confidence 0..255            （仅 status==OK 有效）
```

首字节 `status` 与 `tpv_process_frame` 的返回码一一对应。这样接收端能区分
**"场景中暂时无可抓物"**（`status=EMPTY`）和**"视觉子系统失联"**（根本未
收到帧，由传输层通过超时才能察觉）。`class_id` 严格只承担**单物体级**决策，
场景级故障永远不渗入 class_id。

当 `status != OK` 时，offset 1..8 全部填零，接收端必须忽略。

当 `status == OK` 且 `class_id ∈ {0xFE, 0xFF}` 时，(x, y) 仍然是有意义的
blob 质心（见 §10.1 语义表）。机械臂侧**不得**根据此时的 theta 下达抓取
动作——这是由 class_id 而非 status 控制的业务规则。

具体传输由 platform_glue 负责。串口二进制帧 / TCP JSON 包装器都十分简单，
可由配置开关编译取舍；物理层通常在这 9 字节逻辑 payload 外再加自己的
帧同步（例如 STX / 长度 / CRC）。

### 10.3 标定工具 I/O

```
输入：N_CLASSES × M 帧 640×480 原始 Y 图
输出：model_data.c，内含 `const Template templates[N_CLASSES]`
      report.txt，包含每类可分性指标
```

## 11. 测试策略

| 层级 | 手段 | 通过标准 |
|---|---|---|
| 单元 | PC 端每模块金标数据对比 | 100% 分支覆盖 |
| 属性 | 对输入做纯平移 / 纯旋转，验证 (x, y, θ) 输出变化符合预期 | <1 px / <0.5° 误差 |
| 合成 | 程序生成旋转模板，注入高斯 + 椒盐噪声 | 预期信噪比下分类率 >99.9% |
| 回归 | 回放 10k 条生产帧 | 每版与金标基线决策零差异 |
| 目标机 | 交叉编译到 ARM，在真实板卡计时 | p99 ≤ 30 ms/帧 @ 640×480 |
| 长稳 | 产线连续 ≥100k 帧 | 实测 FAR / FRR；拒绝率 / 漏抓率落在规格内 |

## 12. 风险

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| ≤5 类中有两类 Hu 特征难以区分 | 低 | 高（阻塞交付） | 标定工具的可分性检查（§8 第 8 步）直接拒绝出货；补特征（周长 / 面积 / 3 阶矩）|
| 背景板使用一段时间后有划痕 / 油污 | 中 | 中 | 定期重标定；L2 面积过滤吸收小噪声；暴露运行拒绝率指标给操作员 |
| 相机 / 镜头更换 | 低 | 高 | 工作分辨率与俯视位固定；任何硬件变更都必须重标定 |
| `g_labels` 614.4 KB 缓冲对某些板卡太大 | 低 | 中 | 必要时工作分辨率降到 320×240（小 4 倍），算法不变 |
| 产线新增第 6 类 | — | — | 超出 A3 范围；必须重新编译固件 |

## 13. 开放问题

下列条目来自 §3.3，实施前需用户拍板或细化：

1. **A1（6σ 策略）**：确认机械臂侧确有抓取后复核机制。
2. **A2（节拍）**：30 ms 是合适值，还是节拍更紧 / 更松？
3. **A4（输出传输）**：二选一——串口二进制，或 TCP/JSON。
4. **blob 数量上限**：预期每帧最大 ~30。`MAX_BLOBS` 设为 256 作为并查集后
   的防御性硬顶；`MAX_LABELS` 设为 65535 用于吸收 CCL 第一遍的噪声标号。
   请确认 256 这个上限足够宽松（正常场景绝不会触及）；如果偏保守，可下调
   以便更早触发 `SCENE_ERROR`。
5. **标定 UX**：PC 端工具要不要给操作员做 GUI，还是 CLI + 现有采集工具就够？

## 14. 过渡到实施

本规范通过后，下一步由 writing-plans 技能产出**详细实施计划**：按模块拆分
成有序任务，每步带测试检查点，保证每个中间提交都可编译、可运行、可验证。
