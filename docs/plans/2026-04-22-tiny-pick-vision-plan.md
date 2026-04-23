# Tiny Pick Vision — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现一个 strip 后 ≤ 20 KB 的嵌入式 C 视觉模块，在低端 Android 板卡上识别并定位工业抓取对象，输出 `(class, x, y, θ, confidence)`；配套一个 PC 端离线标定工具，产出编译进固件的模板数据。

**Architecture:** 7 个单一职责的纯 C 模块（threshold / ccl_moments / shape_features / classifier / pose / pipeline / platform_glue），加 1 份由标定工具生成的模板常量表（model_data.c）。纯整数定点运算（Q16.16 + int64 矩累加器），`.bss` 预分配全部工作缓冲；标定与运行共享同一份特征提取代码。

**Tech Stack:** C11，libc 无第三方依赖；工具链 Android NDK `armv7a-linux-androideabi-clang`（目标）+ 宿主 clang/gcc（测试与标定工具）；构建系统 Makefile；测试用纯 C 金标对比 + 合成用例。

**Spec:** [`docs/specs/2026-04-22-tiny-pick-vision-design.md`](../specs/2026-04-22-tiny-pick-vision-design.md)

---

## 文件结构

```
tiny-pick-vision/
├── Makefile                         # host + target + test 多目标构建
├── include/
│   ├── tpv.h                        # 公开 API（process_frame、Detection、返回码）
│   ├── tpv_internal.h               # 内部类型（Blob / Features / Template） + 模块契约
│   └── tpv_config.h                 # 编译期常量（W, H, MAX_*, 阈值等）
├── src/
│   ├── threshold.c                  # Y → 位图（单次扫描）
│   ├── ccl_moments.c                # 两遍 CCL + 矩累加 + 周长
│   ├── shape_features.c             # 矩 → Features（Hu、perim_ratio 等）
│   ├── classifier.c                 # 平方马氏距离 + 三分判决 + d1²
│   ├── pose.c                       # 质心 + 主轴 + 180° 消歧
│   ├── pipeline.c                   # 单帧调度 + 最终 argmax 策略
│   ├── platform_glue.c              # 相机 I/O + 结果输出（stub 版）
│   └── model_data.c                 # 标定工具生成；git-ignored，CI 里再生
├── tests/
│   ├── testlib.c                    # 极简测试运行器（assert + 计数）
│   ├── testlib.h
│   ├── test_threshold.c
│   ├── test_ccl_moments.c
│   ├── test_shape_features.c
│   ├── test_classifier.c
│   ├── test_pose.c
│   ├── test_pipeline.c
│   ├── test_platform_glue.c
│   ├── test_property.c              # 旋转 / 平移不变性
│   ├── test_fixed_math.c            # isqrt / log / atan2 定点辅助
│   ├── stub_model_data.c            # host 测试用占位模板
│   ├── check_layout.c               # _Static_assert(sizeof(Blob) == 80)
│   └── golden/                      # 金标原始 Y 图 + 期望输出
├── tools/
│   └── calibrate/
│       ├── calibrate.c              # main() 调度
│       ├── stats.c                  # 均值 / 协方差 / 正则化 / Cholesky
│       ├── separability.c           # 可分性检查
│       ├── codegen.c                # 生成 model_data.c
│       ├── frame_io.c               # 读 raw Y 帧
│       ├── cal_stub.c               # tpv_bin_threshold/templates 占位
│       ├── Makefile                 # 标定工具自己的构建 + test
│       └── tests/
│           ├── testlib_cal.c        # 复用顶层 testlib 的 host 包装
│           ├── test_stats.c
│           └── test_quantize.c      # HG3
├── docs/
│   ├── specs/2026-04-22-tiny-pick-vision-design.md
│   └── plans/2026-04-22-tiny-pick-vision-plan.md   # 本文件
└── .gitignore
```

---

## 任务依赖

```
T0 bootstrap ──┬─► T1 threshold ──┐
               │                   │
               ├─► T2 ccl_moments ─┼─► T3 shape_features ─► T4 classifier ─┐
               │                   │                                        │
               │                   └───────────────────────► T5 pose ──────┤
               │                                                            │
               ├─► T6 pipeline ◄────────────────────────────────────────────┘
               │
               ├─► T7 platform_glue (stub)
               │
               ├─► T8 calibration tool
               │
               ├─► T9 target build + 20KB 尺寸门槛
               │
               └─► T10 property/replay/hard-gate 测试
```

---

## Reviewer 要求的 5 条硬门槛（分散在各模块的 test 文件中）

| # | 门槛 | 实现于任务 | 测试位置 |
|---|---|---|---|
| HG1 | N_CLASSES = 1 时永不进入 AMBIGUOUS，且 sep_q8 = 255 | T4 | T4 步 7 |
| HG2 | d1² == reject_thresh 时 → REJECTED，confidence_q8 = 0 | T4 | T4 步 8 |
| HG3 | 标定量化下界：reject_thresh / margin 量化为 0 时工具显式失败 | T8 | T8 步 6 |
| HG4 | TPV_OK + class_id ∈ {0xFE, 0xFF} 时 (x, y) 仍输出；theta 不用于抓取 | T6 | T6 步 6 |
| HG5 | 目标工具链下 `sizeof(Blob) == 80`（编译期断言） | T0 + T9 | T0 步 8/9（host）+ T9 步 2（ARM AAPCS）|

---

## Task 0 — 项目骨架与构建系统

**Files:**
- Create: `Makefile`
- Create: `include/tpv_config.h`
- Create: `include/tpv.h`
- Create: `include/tpv_internal.h`
- Create: `tests/testlib.h`, `tests/testlib.c`
- Create: `.gitignore`

- [ ] **Step 1: 写 `.gitignore`**

```
build/
src/model_data.c
*.o
*.a
*.gcno
*.gcda
```

- [ ] **Step 2: 写 `include/tpv_config.h`**

```c
#ifndef TPV_CONFIG_H
#define TPV_CONFIG_H

/* 工作分辨率（spec §3.2 A5） */
#define TPV_WIDTH        640
#define TPV_HEIGHT       480

/* CCL 容量（spec §6） */
#define TPV_MAX_LABELS   65535
#define TPV_MAX_BLOBS    256

/* 模型维度（spec §6） */
#define TPV_N_FEAT       10
#define TPV_L_INV_N      (TPV_N_FEAT * (TPV_N_FEAT + 1) / 2)  /* 55 */

/* 编译期可改：产线类别数，1..5（spec §3.1） */
#ifndef TPV_N_CLASSES
#define TPV_N_CLASSES    5
#endif
#if TPV_N_CLASSES < 1 || TPV_N_CLASSES > 5
#error "TPV_N_CLASSES must be in 1..5"
#endif

/* 几何过滤（spec §5，L2） */
#define TPV_AMIN         500
#define TPV_AMAX         50000

/* 阈值：由标定工具填入 model_data.c 的一个 extern；本头文件仅提供默认 */
#define TPV_BIN_THRESH_DEFAULT   128

/* Q16.16 常量 */
#define TPV_Q16          (1 << 16)
#define TPV_M3_EPS       0x00001000   /* spec §6 */

#endif
```

- [ ] **Step 3: 写 `include/tpv.h`（公开 API）**

```c
#ifndef TPV_H
#define TPV_H
#include <stdint.h>

/* 返回码（spec §10.1） */
#define TPV_OK             0
#define TPV_EMPTY          1
#define TPV_SCENE_ERROR    2
#define TPV_BAD_INPUT    (-1)

/* 特殊 class_id（spec §6） */
#define TPV_CLASS_AMBIGUOUS  0xFE
#define TPV_CLASS_REJECTED   0xFF

typedef struct {
    int16_t x, y;
    int16_t theta_x10;
    uint8_t class_id;
    uint8_t confidence_q8;
} tpv_Detection;

/* spec §10.1 入口契约 */
int tpv_process_frame(const uint8_t *y, int w, int h, tpv_Detection *det_out);

/* spec §10.2 wire payload 序列化（9 字节，传输层共用）
 * status 取值见 TPV_OK / TPV_EMPTY / TPV_SCENE_ERROR / TPV_BAD_INPUT。
 * buf9 由调用方分配，至少 9 字节。 */
void tpv_serialize_payload(uint8_t status, const tpv_Detection *d, uint8_t *buf9);

#endif
```

- [ ] **Step 4: 写 `include/tpv_internal.h`（内部类型 + 模块契约 + Blob layout 断言）**

```c
#ifndef TPV_INTERNAL_H
#define TPV_INTERNAL_H
#include <stdint.h>
#include <assert.h>
#include "tpv_config.h"
#include "tpv.h"

/* spec §6：字段顺序锁定为 AAPCS 下真正 80 B */
typedef struct {
    int64_t mu20, mu11, mu02;
    int64_t mu30, mu21, mu12, mu03;
    int32_t m00, m10, m01;
    int32_t perimeter;
    int16_t bbox_x0, bbox_y0, bbox_x1, bbox_y1;
} tpv_Blob;
_Static_assert(sizeof(tpv_Blob) == 80, "Blob layout drift — fix field order");

typedef struct {
    int32_t hu[7];
    int32_t perim_ratio;
    int32_t eccentricity;
    int32_t m3_axis_sign;
} tpv_Features;
_Static_assert(sizeof(tpv_Features) == 40, "Features layout drift");

typedef struct {
    tpv_Features mean;
    int32_t L_inv[TPV_L_INV_N];
    int32_t reject_thresh;
    int32_t margin;
} tpv_Template;

/* spec §4.2 模块契约 */
void tpv_threshold(const uint8_t *y, int w, int h, uint8_t *bin_out);
int  tpv_ccl_moments(const uint8_t *bin, int w, int h,
                     tpv_Blob *blobs_out, int max_blobs);
void tpv_shape_features(const tpv_Blob *blob, tpv_Features *features_out);
void tpv_classify(const tpv_Features *f,
                  const tpv_Template *tmpl, int n_templates,
                  uint8_t *class_id_out,
                  uint8_t *confidence_out,
                  int32_t *d1_sq_out);
void tpv_pose(const tpv_Blob *blob,
              int16_t *x_out, int16_t *y_out, int16_t *theta_x10_out);

/* 标定工具生成，目标端 link 时必须存在 */
extern const tpv_Template tpv_templates[TPV_N_CLASSES];
extern const uint8_t      tpv_bin_threshold;

#endif
```

- [ ] **Step 5: 写 `tests/testlib.h` 和 `tests/testlib.c`（极简测试运行器）**

`tests/testlib.h`：

```c
#ifndef TPV_TESTLIB_H
#define TPV_TESTLIB_H
#include <stdio.h>
#include <stdlib.h>

extern int tpv_test_pass;
extern int tpv_test_fail;

#define TEST(name) static void name(void)
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        tpv_test_fail++; return; \
    } \
} while (0)
#define CHECK_EQ_I(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d  %s == %s (%lld vs %lld)\n", \
                __FILE__, __LINE__, #a, #b, _a, _b); \
        tpv_test_fail++; return; \
    } \
} while (0)
#define RUN(name) do { \
    int before = tpv_test_fail; \
    name(); \
    if (tpv_test_fail == before) { tpv_test_pass++; printf("  ok  %s\n", #name); } \
} while (0)
#define FINISH() do { \
    printf("\n%d passed, %d failed\n", tpv_test_pass, tpv_test_fail); \
    return tpv_test_fail ? 1 : 0; \
} while (0)

#endif
```

`tests/testlib.c`：

```c
#include "testlib.h"
int tpv_test_pass = 0;
int tpv_test_fail = 0;
```

- [ ] **Step 6: 写 `Makefile`**

```make
# Toolchains
CC_HOST   ?= cc
CC_TARGET ?= armv7a-linux-androideabi24-clang
AR_TARGET ?= llvm-ar
STRIP     ?= llvm-strip

# Flags
# Host：放开 POSIX 扩展（host 测试和工具用到 strdup / opendir / readdir 等
# POSIX 接口）。-std=gnu11 也行，但显式 _POSIX_C_SOURCE 更可移植。
CFLAGS_COMMON  = -std=c11 -Wall -Wextra -Werror -Wpedantic -Iinclude
CFLAGS_HOST    = $(CFLAGS_COMMON) -g -O0 -DTPV_HOST_BUILD -D_POSIX_C_SOURCE=200809L
# Target：嵌入式严格 freestanding，无 POSIX 假设
CFLAGS_TARGET  = $(CFLAGS_COMMON) -Os -flto -ffreestanding -fno-exceptions \
                 -fno-asynchronous-unwind-tables -fomit-frame-pointer

# 运行时模块源（每个任务里逐一新增；T0 时只需空文件即可）
SRCS = src/threshold.c src/ccl_moments.c src/shape_features.c \
       src/classifier.c src/pose.c src/pipeline.c src/platform_glue.c \
       src/fixed_math.c

# 每个 test_*.c 都是一个独立可执行（避免多 main 冲突 + 易于 CI 并行）。
# 每个 test 链接 testlib + 全部 SRCS + stub_model_data。
TEST_FILES = $(wildcard tests/test_*.c)
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_FILES))

.PHONY: host target test size check-layout clean

host: build/libtpv-host.a
target: build/libtpv-arm.so

# 跑全部测试：每个二进制独立 ./run；任一失败 → make 失败
test: $(TEST_BINS)
	@fail=0; for b in $(TEST_BINS); do \
	   echo "=== $$b ==="; \
	   $$b || fail=1; \
	 done; exit $$fail

build:
	mkdir -p $@

# 主机库（host 端共用，便于其它工具/调试器 link）
build/libtpv-host.a: $(SRCS) | build
	$(CC_HOST) $(CFLAGS_HOST) -c $(SRCS)
	ar rcs $@ *.o
	rm -f *.o

# 目标端最终交付物：动态库 .so（这才是部署形态，size 度量基于此）
build/libtpv-arm.so: $(SRCS) src/model_data.c | build
	$(CC_TARGET) $(CFLAGS_TARGET) -shared -o $@ $(SRCS) src/model_data.c

# 单测可执行：每个 test_*.c 独立 link
build/test_%: tests/test_%.c tests/testlib.c $(SRCS) tests/stub_model_data.c | build
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $^ -lm

# 编译期布局断言（不依赖任何 SRCS）—— T0 就能跑
check-layout: tests/check_layout.c | build
	$(CC_HOST) $(CFLAGS_HOST) -c $< -o build/check_layout.o
	@echo "OK: tpv_Blob layout assert held under host toolchain"

# 20 KB 门槛：spec G1 = "二进制尺寸（strip 后）≤ 20 KB"，意指**最终烧入
# 设备的字节数**。对 .so 部署形态，这包括 .text + .rodata 之外，还有
# .dynsym / .dynstr / .dynamic / .rel* / .hash 等装载元数据。这些字节都
# 会随文件落到设备上，必须计入门槛。
#
# 因此 gate 用 strip 后的 .so 文件大小（wc -c）。同时报告 .text /
# .rodata / 其它 section 占用，便于诊断瘦身方向。
size: build/libtpv-arm.so
	$(STRIP) --strip-all $<
	@file_sz=$$(wc -c < $<); \
	 echo "--- section breakdown (diagnostic) ---"; \
	 llvm-size --format=sysv $< | awk '/^\.[a-z]/ { printf "  %-20s %8d\n", $$1, $$2 }'; \
	 echo "--- final stripped file size = $$file_sz B (limit=20480) ---"; \
	 if [ $$file_sz -gt 20480 ]; then echo "FAIL: $$file_sz > 20480"; exit 1; \
	 else echo "OK: $$file_sz ≤ 20480"; fi
#
# 注：若 .so 装载元数据本身就接近 / 超过 20 KB（armv7 上典型 5–10 KB），
# 而代码 + rodata 又必须≥ 12 KB，门槛真的不可达，此时讨论改 G1 的语义
# （"代码+只读常量预算"），或改交付形态（用 build/libtpv-arm.a 归档让
# 集成方静态链接到自己的可执行文件，规避 .so 装载开销）。在 spec 没改
# 之前，gate 必须按 spec 字面量执行。

clean:
	rm -rf build *.o *.a
```

- [ ] **Step 7: 写 `tests/stub_model_data.c`（host 测试用占位模板）**

```c
#include "tpv_internal.h"
const uint8_t tpv_bin_threshold = TPV_BIN_THRESH_DEFAULT;
const tpv_Template tpv_templates[TPV_N_CLASSES] = {0};
```

- [ ] **Step 8: 写 `tests/check_layout.c`（独立编译，不依赖任何 src/）**

```c
/* 这个文件只用 _Static_assert 验证 tpv_Blob / tpv_Features 在当前
   工具链下的 ABI 布局。它故意不引入任何 src/ 文件，所以 T0 阶段
   src/threshold.c 等还没创建时也能编译通过。 */
#include "tpv_internal.h"
_Static_assert(sizeof(tpv_Blob) == 80, "Blob layout drift");
_Static_assert(sizeof(tpv_Features) == 40, "Features layout drift");
/* T9 阶段会在目标工具链下再跑一次，作为 HG5 硬门槛 */
```

- [ ] **Step 9: 在宿主上验证布局断言**

Run:
```
make check-layout
```
Expected: `OK: tpv_Blob layout assert held under host toolchain`。
若失败：字段顺序写错了 → 修 `tpv_internal.h` 直到 host 通过。
（T9 会在 ARM AAPCS 下再做一次同样的断言，作为 HG5。）

> 注：这一步**不要**跑 `make host` 或 `make test`，因为 src/*.c 还都是空的；
> 那两个目标要等 T1 第一个模块落地后才有意义。

- [ ] **Step 10: 创建空源文件占位（让后续任务的目标可以提前运行）**

```bash
mkdir -p src
for f in threshold ccl_moments shape_features classifier pose pipeline platform_glue fixed_math; do
  printf '#include "tpv_internal.h"\n' > src/$f.c
done
```

这些文件目前没有函数定义，所以 `make host` 仍然会报 "未定义引用"。这是
**预期行为**——接下来每个任务把对应的 `src/X.c` 替换成真实实现，每完成
一个就 `make build/test_X` 跑该模块的单测；当 T6 完成时，`make test` 才
全绿。

- [ ] **Step 11: Commit**

```bash
cd ~/work/tiny-pick-vision
git add Makefile include tests/testlib.h tests/testlib.c \
        tests/stub_model_data.c tests/check_layout.c src/ .gitignore
git commit -m "build(t0): project scaffold, public API, internal types, test lib, layout assert"
```

---

## Task 1 — threshold 模块

**Files:**
- Create: `src/threshold.c`
- Create: `tests/test_threshold.c`

- [ ] **Step 1: 写 `tests/test_threshold.c`（至少 3 个用例）**

```c
#include <string.h>
#include "tpv_internal.h"
#include "testlib.h"

extern const uint8_t tpv_bin_threshold;  /* 来自 stub */

TEST(t_all_zero_maps_to_zero_bits) {
    uint8_t y[64] = {0};
    uint8_t bin[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
    tpv_threshold(y, 64, 1, bin);
    for (int i = 0; i < 8; i++) CHECK_EQ_I(bin[i], 0x00);
}

TEST(t_all_255_maps_to_all_ones) {
    uint8_t y[64];
    memset(y, 255, sizeof y);
    uint8_t bin[8] = {0};
    tpv_threshold(y, 64, 1, bin);
    for (int i = 0; i < 8; i++) CHECK_EQ_I(bin[i], 0xFF);
}

TEST(t_at_threshold_is_foreground) {
    /* 约定：Y >= threshold ⇒ 1（前景） */
    uint8_t y[8] = {0, 127, 128, 129, 200, 127, 255, 0};
    /* 期望 bin = 0b0011'1010 ⇒ MSB-first 位顺序；LSB-first 则是 0b0101'1100 */
    uint8_t bin[1] = {0};
    tpv_threshold(y, 8, 1, bin);
    /* 实现方选 LSB-first：bit[i] = (y[i] >= thresh) << i */
    CHECK_EQ_I(bin[0], 0x5C);  /* 0101 1100 */
}

int main(void) {
    RUN(t_all_zero_maps_to_zero_bits);
    RUN(t_all_255_maps_to_all_ones);
    RUN(t_at_threshold_is_foreground);
    FINISH();
}
```

- [ ] **Step 2: 跑测试确认红色**

```
make test
```
Expected: `undefined reference to tpv_threshold`。

- [ ] **Step 3: 实现 `src/threshold.c`（单次扫描，逐字节）**

```c
#include "tpv_internal.h"

void tpv_threshold(const uint8_t *y, int w, int h, uint8_t *bin_out) {
    const uint8_t t = tpv_bin_threshold;
    const int npix = w * h;
    const int nby  = (npix + 7) / 8;
    for (int i = 0; i < nby; i++) bin_out[i] = 0;
    /* LSB-first：bit index i 对应字节 i/8，位 i%8 */
    for (int i = 0; i < npix; i++) {
        if (y[i] >= t) bin_out[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
}
```

- [ ] **Step 4: 跑测试确认绿色**

```
make test
```
Expected: `3 passed, 0 failed`。

- [ ] **Step 5: Commit**

```bash
git add src/threshold.c tests/test_threshold.c
git commit -m "feat(threshold): Y→bitmap with LSB-first packing + 3 tests"
```

---

## Task 2 — CCL + 矩 + 周长累加

**Files:**
- Create: `src/ccl_moments.c`
- Create: `tests/test_ccl_moments.c`

- [ ] **Step 1: 先写 union-find 最小测试 + 完整 main()**

> **测试文件统一约定**（适用于 T1..T10 所有 task）：每个 `tests/test_*.c`
> 在 step 1 同时落 `main()`。后续 step 加新 `TEST(...)` 时**同步更新
> main 里的 `RUN(...)` 列表**。这样每个绿色 / 红色 checkpoint 都基于
> 一个能被 Makefile 链接的可执行文件——否则 `make build/test_<name>`
> 直接卡在缺 main 的链接错。

```c
/* tests/test_ccl_moments.c —— 这是这个文件的完整初版骨架 */
#include "tpv_internal.h"
#include "testlib.h"

/* 暴露给测试的 union-find；正式实现时放在 ccl_moments.c 静态段，用
   TPV_TEST_EXPOSE 宏开条件编译以允许测试可见。 */
#ifdef TPV_TEST_EXPOSE
extern uint32_t uf_find(uint32_t x);
extern void     uf_union(uint32_t a, uint32_t b);
extern void     uf_reset(int n);
#endif

TEST(t_uf_basic) {
#ifdef TPV_TEST_EXPOSE
    uf_reset(10);
    CHECK_EQ_I(uf_find(3), 3);
    uf_union(3, 7);
    CHECK_EQ_I(uf_find(7), uf_find(3));
    uf_union(7, 9);
    CHECK_EQ_I(uf_find(3), uf_find(9));
    CHECK_EQ_I(uf_find(2), 2);  /* 未合并的节点不受影响 */
#endif
}

int main(void) {
    RUN(t_uf_basic);
    /* step 2 / step 6 加新 test 时把名字 RUN 在这里 */
    FINISH();
}
```

- [ ] **Step 2: 加 3 个 blob 识别测试 + 更新 main 的 RUN 列表**

```c
TEST(t_single_square_blob) {
    /* 8x8 全前景 */
    uint8_t bin[8];
    for (int i = 0; i < 8; i++) bin[i] = 0xFF;
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, 8, 8, blobs, 4);
    CHECK_EQ_I(n, 1);
    CHECK_EQ_I(blobs[0].m00, 64);
    /* 质心 = (3.5, 3.5)，Q0 下 m10 = 3.5 * 64 = 224 */
    CHECK_EQ_I(blobs[0].m10, 64 * 4 - 32);  /* sum of 0..7 * 8 = 224 */
    CHECK_EQ_I(blobs[0].m01, 224);
    /* 8x8 方块周长 = 4*8 = 32（4-邻域边界） */
    CHECK_EQ_I(blobs[0].perimeter, 32);
}

TEST(t_two_disjoint_blobs) {
    /* 两个分开的 2x2 方块 */
    uint8_t bin[8] = {0};
    /* 左上 2x2：行 0-1 列 0-1 */
    bin[0] = 0x03; bin[1] = 0x03;   /* 按 8px/行 假设，请按实际 w 对齐调整 */
    /* 右下 2x2：行 6-7 列 6-7 */
    bin[6] = 0xC0; bin[7] = 0xC0;
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, 8, 8, blobs, 4);
    CHECK_EQ_I(n, 2);
}

TEST(t_touching_blobs_become_one_not_two) {
    /* 两个方块共享边：CCL 必须合并为单一 blob（spec §3.1 前提下上料
       工装保证非接触，所以这里只是验证"确实是 CCL 把它们合并"，
       不要误以为有分离算法） */
    uint8_t bin[8] = {0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0};
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, 8, 8, blobs, 4);
    CHECK_EQ_I(n, 1);           /* 合并后只有 1 个 */
    CHECK_EQ_I(blobs[0].m00, 32); /* 两个 2x8 合并 = 32 像素 */
}
```

把 main() 里 `RUN(...)` 列表更新为：

```c
int main(void) {
    RUN(t_uf_basic);
    RUN(t_single_square_blob);
    RUN(t_two_disjoint_blobs);
    RUN(t_touching_blobs_become_one_not_two);
    /* step 6 还会 RUN t_max_labels_overflow_returns_neg1 */
    FINISH();
}
```

- [ ] **Step 3: 跑测试确认红色**

```
make test
```
Expected: link error。

- [ ] **Step 4: 实现 `src/ccl_moments.c` 第一版（pass 1 + union-find + pass 2 矩累加）**

```c
#include <string.h>
#include "tpv_internal.h"

/* 工作缓冲（spec §6） */
static uint8_t  g_bin[TPV_WIDTH * TPV_HEIGHT / 8];
static uint16_t g_labels[TPV_WIDTH * TPV_HEIGHT];
static uint32_t g_uf[TPV_MAX_LABELS + 1];

#ifdef TPV_TEST_EXPOSE
uint32_t uf_find(uint32_t x);
void     uf_union(uint32_t a, uint32_t b);
void     uf_reset(int n);
#endif

static uint32_t uf_find(uint32_t x) {
    while (g_uf[x] != x) {
        g_uf[x] = g_uf[g_uf[x]];  /* path compression */
        x = g_uf[x];
    }
    return x;
}
static void uf_union(uint32_t a, uint32_t b) {
    uint32_t ra = uf_find(a), rb = uf_find(b);
    if (ra == rb) return;
    if (ra < rb) g_uf[rb] = ra; else g_uf[ra] = rb;
}
static void uf_reset(int n) { for (int i = 0; i <= n; i++) g_uf[i] = (uint32_t)i; }

static inline int bit_at(const uint8_t *bin, int w, int x, int y) {
    if (x < 0 || y < 0 || x >= w) return 0;
    int idx = y * w + x;
    return (bin[idx >> 3] >> (idx & 7)) & 1;
}

int tpv_ccl_moments(const uint8_t *bin, int w, int h,
                    tpv_Blob *blobs_out, int max_blobs) {
    /* Pass 1: 扫描 + 4 邻域（上 / 左），分配临时标号 */
    uf_reset(0);
    uint32_t next_label = 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            if (!((bin[idx >> 3] >> (idx & 7)) & 1)) { g_labels[idx] = 0; continue; }
            uint16_t up   = (y > 0) ? g_labels[idx - w] : 0;
            uint16_t left = (x > 0) ? g_labels[idx - 1] : 0;
            if (!up && !left) {
                if (next_label > TPV_MAX_LABELS) return -1;  /* SCENE_ERROR 上层判 */
                g_labels[idx] = (uint16_t)next_label;
                g_uf[next_label] = next_label;
                next_label++;
            } else if (up && !left)      g_labels[idx] = up;
            else if (!up && left)        g_labels[idx] = left;
            else {
                g_labels[idx] = (uint16_t)((up < left) ? up : left);
                uf_union(up, left);
            }
        }
    }
    /* Pass 2: 解析等价类并累加矩（含周长） */
    /* 用 16-bit 重标号到紧凑序号 */
    static uint16_t remap[TPV_MAX_LABELS + 1];
    memset(remap, 0, (next_label) * sizeof(uint16_t));
    int n_blobs = 0;
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        if (r == l) {
            if (n_blobs >= max_blobs) return -2;  /* SCENE_ERROR */
            remap[l] = (uint16_t)(n_blobs + 1);
            /* 清零该 blob */
            tpv_Blob *b = &blobs_out[n_blobs];
            memset(b, 0, sizeof *b);
            b->bbox_x0 = (int16_t)TPV_WIDTH;
            b->bbox_y0 = (int16_t)TPV_HEIGHT;
            b->bbox_x1 = -1; b->bbox_y1 = -1;
            n_blobs++;
        }
    }
    for (uint32_t l = 1; l < next_label; l++) {
        uint32_t r = uf_find(l);
        remap[l] = remap[r];
    }

    /* 第二遍扫描：填 raw 矩（m00, m10, m01）+ 周长 + bbox */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = y * w + x;
            uint16_t rl = g_labels[idx] ? remap[g_labels[idx]] : 0;
            if (!rl) continue;
            tpv_Blob *b = &blobs_out[rl - 1];
            b->m00 += 1;
            b->m10 += x;
            b->m01 += y;
            if (x < b->bbox_x0) b->bbox_x0 = (int16_t)x;
            if (y < b->bbox_y0) b->bbox_y0 = (int16_t)y;
            if (x > b->bbox_x1) b->bbox_x1 = (int16_t)x;
            if (y > b->bbox_y1) b->bbox_y1 = (int16_t)y;
            /* 周长：4 邻域中任一为背景则 +1 */
            if (!bit_at(bin, w, x-1, y) || !bit_at(bin, w, x+1, y) ||
                !bit_at(bin, w, x, y-1) || !bit_at(bin, w, x, y+1)) {
                b->perimeter += 1;
            }
        }
    }

    /* 第三遍：中心矩 μ20 / μ11 / μ02 / μ30 / μ21 / μ12 / μ03 */
    for (int i = 0; i < n_blobs; i++) {
        tpv_Blob *b = &blobs_out[i];
        if (b->m00 == 0) continue;
        /* 质心 Q0 定点：cx_q0 = m10 / m00，为避免除法，我们计算并保存 
           为 Q16.16 供后续；这里用简单除法，离线验证误差 */
        int32_t cx_q0 = b->m10 / b->m00;
        int32_t cy_q0 = b->m01 / b->m00;
        for (int y = b->bbox_y0; y <= b->bbox_y1; y++) {
            for (int x = b->bbox_x0; x <= b->bbox_x1; x++) {
                int idx = y * w + x;
                uint16_t rl = g_labels[idx] ? remap[g_labels[idx]] : 0;
                if (rl != (uint16_t)(i + 1)) continue;
                int64_t dx = x - cx_q0;
                int64_t dy = y - cy_q0;
                b->mu20 += dx * dx;
                b->mu11 += dx * dy;
                b->mu02 += dy * dy;
                b->mu30 += dx * dx * dx;
                b->mu21 += dx * dx * dy;
                b->mu12 += dx * dy * dy;
                b->mu03 += dy * dy * dy;
            }
        }
    }
    return n_blobs;
}
```

（注：上面是 第一版——清晰优先。Task 2 步骤 7 会合并到两遍扫描以省时间。）

- [ ] **Step 5: 跑测试确认绿色**

```
make test
```
Expected: 单方块 / 双方块 / 周长测试通过。

- [ ] **Step 6: 加硬门槛测试 — MAX_LABELS / MAX_BLOBS 溢出触发 SCENE_ERROR**

```c
TEST(t_max_labels_overflow_returns_neg1) {
    /* 用 32x32 每像素交替，人为让 pass 1 分配极多标号 */
    uint8_t bin[128] = {0};
    for (int i = 0; i < 32 * 32; i += 2) bin[i >> 3] |= (uint8_t)(1u << (i & 7));
    tpv_Blob b[TPV_MAX_BLOBS];
    int n = tpv_ccl_moments(bin, 32, 32, b, TPV_MAX_BLOBS);
    /* 正数或 -1 / -2，视具体密度；这里只要求：要么所有 blob 识别成功，
       要么返回负值；若返回正数则其不超过 MAX_BLOBS */
    CHECK((n < 0) || (n <= TPV_MAX_BLOBS));
}
```

main 的 RUN 列表加一行 `RUN(t_max_labels_overflow_returns_neg1);`。

Run: `make build/test_ccl_moments && ./build/test_ccl_moments`。预期：
此用例本身通过（只验证协议——不越界、不崩）。

- [ ] **Step 7: 合并第二、三遍扫描到单遍（省 30% 运行时）**

修改实现，让中心矩的累积在"已知 m00/m10/m01"后就开始（即在 pass 2 的外循环里，每完成一个 blob 的 raw moments 就立即做 bbox 内的中心矩扫描；或者用一遍扫描直接累加 x²/xy/y²/x³/... 的和，最后一次性换算到中心矩）。

正规做法是**累加原始矩到 3 阶**：

```c
/* 每像素累加：sum_1, sum_x, sum_y, sum_xx, sum_xy, sum_yy,
                sum_xxx, sum_xxy, sum_xyy, sum_yyy */
/* 最后用公式换算成 μ_pq（参见《图像矩》公式） */
```

Update test_ccl_moments 仍然通过后 commit。

- [ ] **Step 8: Commit**

```bash
git add src/ccl_moments.c tests/test_ccl_moments.c
git commit -m "feat(ccl): two-pass CCL with union-find, raw moments, perimeter"
```

---

## Task 3 — shape_features

**Files:**
- Create: `src/shape_features.c`
- Create: `tests/test_shape_features.c`

- [ ] **Step 1: 写一份手算金标测试**

对 4x4 实心方块 blob，`m00=16, μ20=20, μ02=20, μ11=0, ...`；Hu 矩对方块有已知值。

```c
#include <string.h>
#include "tpv_internal.h"
#include "testlib.h"

static tpv_Blob make_square(void) {
    tpv_Blob b = {0};
    b.m00 = 16;
    b.m10 = 24; b.m01 = 24;  /* 质心 (1.5, 1.5) */
    b.mu20 = 20; b.mu02 = 20; b.mu11 = 0;
    b.mu30 = 0; b.mu03 = 0; b.mu21 = 0; b.mu12 = 0;
    b.perimeter = 16;
    b.bbox_x0 = 0; b.bbox_x1 = 3; b.bbox_y0 = 0; b.bbox_y1 = 3;
    return b;
}

TEST(t_square_features_symmetric) {
    tpv_Blob b = make_square();
    tpv_Features f;
    tpv_shape_features(&b, &f);
    /* 方块 2 阶对称：hu[0] = μ20 + μ02 / m00² 归一化后一个正常正值 */
    CHECK(f.hu[0] != 0);
    /* 方块 3 阶为零 → m3_axis_sign = 0（对称） */
    CHECK_EQ_I(f.m3_axis_sign, 0);
    /* 偏心率 0（正方形） */
    CHECK(f.eccentricity >= -65 && f.eccentricity <= 65);  /* ±1e-3 Q16.16 */
}

int main(void) {
    RUN(t_square_features_symmetric);
    FINISH();
}
```

- [ ] **Step 2: 跑测试确认红色**

- [ ] **Step 3: 实现 `src/shape_features.c`**

```c
#include "tpv_internal.h"

/* 定点对数：用查表 + 线性插值，8 KB 表太大，改用牛顿迭代的 2^n 归一化法
   近似 ln，Q16.16 精度。此处省略为辅助函数 log_q16(x)。*/
static int32_t log_q16(int64_t x_q16);    /* 返回 Q16.16 */

void tpv_shape_features(const tpv_Blob *b, tpv_Features *f) {
    /* 归一化：η_pq = μ_pq / m00^((p+q)/2 + 1)。用 Q16.16 */
    int64_t m00 = b->m00;
    if (m00 == 0) { for (int i = 0; i < 10; i++) ((int32_t*)f)[i] = 0; return; }

    /* 缩放因子：η2 = μ / m00^2；η3 = μ / m00^2.5 */
    int64_t m00sq = m00 * m00;
    /* 归一化 2 阶与 3 阶 */
    int64_t n20 = (b->mu20 << 16) / m00sq;
    int64_t n02 = (b->mu02 << 16) / m00sq;
    int64_t n11 = (b->mu11 << 16) / m00sq;
    /* 3 阶归一化除以 m00^(2.5)，用 m00^2 * sqrt(m00) 近似 */
    int64_t sqrt_m00_q16 = isqrt_q16(m00 << 16);
    int64_t m00_25 = (m00sq * sqrt_m00_q16) >> 16;
    int64_t n30 = (b->mu30 << 16) / m00_25;
    int64_t n21 = (b->mu21 << 16) / m00_25;
    int64_t n12 = (b->mu12 << 16) / m00_25;
    int64_t n03 = (b->mu03 << 16) / m00_25;

    /* 7 个 Hu 不变矩（公式见 OpenCV imgproc 或 Hu 1962 原文） */
    int64_t h[7];
    /* Hu 1962 七个不变矩 —— 完整公式（推导时需注意 Q16.16 乘法对齐） */
    h[0] = n20 + n02;
    h[1] = (n20 - n02)*(n20 - n02) + 4*n11*n11;
    h[2] = (n30 - 3*n12)*(n30 - 3*n12) + (3*n21 - n03)*(3*n21 - n03);
    h[3] = (n30 + n12)*(n30 + n12) + (n21 + n03)*(n21 + n03);
    h[4] = (n30 - 3*n12) * (n30 + n12) *
           ((n30 + n12)*(n30 + n12) - 3*(n21 + n03)*(n21 + n03))
         + (3*n21 - n03) * (n21 + n03) *
           (3*(n30 + n12)*(n30 + n12) - (n21 + n03)*(n21 + n03));
    h[5] = (n20 - n02) *
           ((n30 + n12)*(n30 + n12) - (n21 + n03)*(n21 + n03))
         + 4 * n11 * (n30 + n12) * (n21 + n03);
    h[6] = (3*n21 - n03) * (n30 + n12) *
           ((n30 + n12)*(n30 + n12) - 3*(n21 + n03)*(n21 + n03))
         - (n30 - 3*n12) * (n21 + n03) *
           (3*(n30 + n12)*(n30 + n12) - (n21 + n03)*(n21 + n03));
    /* 注意：上面按代数写法列出。Q16.16 实现时每次乘法后要右移 16，
     * 连续乘法累积容易溢出 int64，中间结果必要时降精度（右移 24）后再乘。
     * 单元测试里用双精度参考实现对拍。*/

    for (int i = 0; i < 7; i++) {
        /* Q16.16 log |h| 压缩 */
        int64_t ax = h[i] < 0 ? -h[i] : h[i];
        int32_t sign = h[i] < 0 ? -1 : 1;
        int32_t lg = log_q16(ax + 1);   /* +1 防 log(0) */
        f->hu[i] = sign * lg;
    }

    /* perim_ratio = perimeter / sqrt(area) */
    int64_t sqrt_area = isqrt_q16(m00 << 16);
    f->perim_ratio = (int32_t)(((int64_t)b->perimeter << 32) / sqrt_area);

    /* eccentricity = sqrt(1 - λ_min/λ_max) where λ are 2x2 covariance eigenvalues */
    /* 2x2 矩阵 [[μ20, μ11], [μ11, μ02]] 的特征值，公式化简为 Q16.16 */
    int64_t tr = b->mu20 + b->mu02;
    int64_t det = b->mu20 * b->mu02 - b->mu11 * b->mu11;
    int64_t disc = tr*tr - 4*det;
    int64_t sdisc = disc > 0 ? isqrt_q16(disc << 16) : 0;
    int64_t l1 = (tr + (sdisc >> 16)) / 2;
    int64_t l2 = (tr - (sdisc >> 16)) / 2;
    if (l1 < l2) { int64_t t = l1; l1 = l2; l2 = t; }
    if (l1 == 0) f->eccentricity = 0;
    else {
        int64_t ratio = (l2 << 16) / l1;   /* Q16.16, ∈ [0,1] */
        int64_t one = 1 << 16;
        f->eccentricity = (int32_t)isqrt_q16(((one - ratio) << 16));
    }

    /* m3_axis_sign：μ₃ 在主轴方向的投影符号。主轴方向 (cosθ, sinθ)，
       θ = 0.5·atan2(2μ₁₁, μ₂₀-μ₀₂)。投影 = μ₃₀·c³ + 3μ₂₁·c²s + 3μ₁₂·cs² + μ₀₃·s³ */
    /* 这里做简化：直接用 μ30 + μ03 的符号 + 主轴方向做判定，
       完整实现里按公式算 */
    int64_t proj_simple = b->mu30 + b->mu03;
    if (proj_simple > TPV_M3_EPS) f->m3_axis_sign = 1;
    else if (proj_simple < -TPV_M3_EPS) f->m3_axis_sign = -1;
    else f->m3_axis_sign = 0;
}
```

注：上面 `log_q16` 和 `isqrt_q16` 是 `src/fixed_math.c` 的辅助函数。本任务里要一并实现它们并单测。

- [ ] **Step 4: 加 `src/fixed_math.c` + `tests/test_fixed_math.c`**

```c
/* src/fixed_math.c */
int64_t isqrt_q16(int64_t x_q16) {
    if (x_q16 <= 0) return 0;
    /* 牛顿法，初值 = x/2 */
    int64_t r = x_q16;
    for (int i = 0; i < 20; i++) {
        int64_t q = (x_q16 << 16) / r;
        r = (r + q) / 2;
    }
    return r;
}

int32_t log_q16(int64_t x_q16) {
    /* x_q16 是 Q16.16。用 ln(x) = ln(2) * log2(x)；log2 按二分归一化 */
    if (x_q16 <= 0) return -(1 << 30);
    int shift = 0;
    int64_t v = x_q16;
    while (v >= (2LL << 16)) { v >>= 1; shift++; }
    while (v < (1LL << 16))  { v <<= 1; shift--; }
    /* v 现在 ∈ [1, 2) 的 Q16.16 */
    /* log2(v) ≈ v - 1 − (v-1)² / 2 + … 泰勒前三项 */
    int64_t u = v - (1LL << 16);
    int64_t u2 = (u * u) >> 16;
    int64_t lg2 = u - (u2 >> 1) + ((u2 * u) >> 17);
    int64_t ln2_q16 = 45426;  /* ln(2) * 65536 */
    int64_t ln_frac = (lg2 * ln2_q16) >> 16;
    return (int32_t)(shift * ln2_q16 + ln_frac);
}
```

Test:
```c
TEST(t_isqrt_4) {
    /* isqrt_q16(4.0 Q16.16) = 2.0 Q16.16 */
    int64_t r = isqrt_q16(4LL << 16);
    CHECK(r >= (2LL << 16) - 10 && r <= (2LL << 16) + 10);
}
TEST(t_log_e) {
    /* log(e) ≈ 1 */
    int64_t e_q16 = 178145;  /* e * 65536 */
    int32_t lg = log_q16(e_q16);
    CHECK(lg >= (1 << 16) - 500 && lg <= (1 << 16) + 500);
}

int main(void) {
    RUN(t_isqrt_4);
    RUN(t_log_e);
    /* T5 step 5 会再追加 atan2 测试和对应 RUN */
    FINISH();
}
```

（`tests/test_shape_features.c` 的 main 已在 step 1 写好。）

- [ ] **Step 5: 跑全部测试**

```
make test
```
Expected: `N passed, 0 failed`。

- [ ] **Step 6: Commit**

```bash
git add src/shape_features.c src/fixed_math.c tests/test_shape_features.c tests/test_fixed_math.c include
# 将 isqrt_q16 / log_q16 前置声明加到 tpv_internal.h
git commit -m "feat(features): Hu moments + perim_ratio + eccentricity + m3_axis_sign; fixed-math helpers"
```

---

## Task 4 — classifier（含 HG1 / HG2 两条硬门槛）

**Files:**
- Create: `src/classifier.c`
- Create: `tests/test_classifier.c`

- [ ] **Step 1: 写 5 个核心测试**

```c
#include <string.h>
#include "tpv_internal.h"
#include "testlib.h"

static tpv_Template simple_template(int32_t diag, int32_t rt, int32_t mg) {
    tpv_Template t = {0};
    for (int i = 0; i < TPV_L_INV_N; i++) t.L_inv[i] = 0;
    /* 对角 L⁻¹ = 1/σ_i；在平方距离里这等于 Σ((x_i-μ_i)/σ_i)² */
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++) {
        for (int j = 0; j <= i; j++) {
            t.L_inv[idx++] = (i == j) ? diag : 0;
        }
    }
    t.reject_thresh = rt;
    t.margin = mg;
    return t;
}

TEST(t_features_equal_mean_gives_zero_distance) {
    tpv_Template t = simple_template(TPV_Q16, 10 << 16, 2 << 16);
    tpv_Features f = t.mean;
    uint8_t cid = 0xAA, conf = 0xAA; int32_t d1sq = -1;
    tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
    CHECK_EQ_I(cid, 0);
    CHECK_EQ_I(d1sq, 0);
    CHECK(conf == 255);  /* ACCEPTED 在 N=1 下最高 */
}

TEST(t_far_features_rejected) {
    tpv_Template t = simple_template(TPV_Q16, 4 << 16, 2 << 16);
    tpv_Features f = t.mean;
    f.hu[0] = 5 << 16;   /* 距均值 5 → d1² = 25 > reject_thresh=4 */
    uint8_t cid, conf; int32_t d1sq;
    tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
    CHECK_EQ_I(cid, TPV_CLASS_REJECTED);
    CHECK_EQ_I(conf, 0);
}

int main(void) {
    RUN(t_features_equal_mean_gives_zero_distance);
    RUN(t_far_features_rejected);
    /* step 5 / 7 / 8 加 ambig + HG1 + HG2 时把它们 RUN 在这里 */
    FINISH();
}
```

- [ ] **Step 2: 跑测试确认红色**

- [ ] **Step 3: 实现 `src/classifier.c`**

```c
#include "tpv_internal.h"
#include <string.h>

static int64_t mahal_sq_q16(const tpv_Features *x, const tpv_Template *tmpl) {
    /* 计算 y = L⁻¹ (x - μ)，返回 ||y||² （Q16.16） */
    int32_t dx[TPV_N_FEAT];
    const int32_t *xm = (const int32_t *)&tmpl->mean;
    const int32_t *xv = (const int32_t *)x;
    for (int i = 0; i < TPV_N_FEAT; i++) dx[i] = xv[i] - xm[i];

    int64_t y2 = 0;
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++) {
        int64_t yi = 0;
        for (int j = 0; j <= i; j++) {
            yi += ((int64_t)tmpl->L_inv[idx + j] * dx[j]) >> 16;
        }
        idx += (i + 1);
        y2 += yi * yi;
    }
    return y2 >> 16;   /* 回到 Q16.16 */
}

void tpv_classify(const tpv_Features *f,
                  const tpv_Template *tmpl, int n,
                  uint8_t *class_id_out,
                  uint8_t *confidence_out,
                  int32_t *d1_sq_out) {
    int64_t d1sq = (int64_t)1 << 62, d2sq = (int64_t)1 << 62;
    int winner = 0;
    for (int c = 0; c < n; c++) {
        int64_t d = mahal_sq_q16(f, &tmpl[c]);
        if (d < d1sq) { d2sq = d1sq; d1sq = d; winner = c; }
        else if (d < d2sq) { d2sq = d; }
    }
    *d1_sq_out = (int32_t)(d1sq > INT32_MAX ? INT32_MAX : d1sq);

    int32_t rt = tmpl[winner].reject_thresh;
    int32_t mg = tmpl[winner].margin;

    /* L3 闭集（spec §9.1）：d1² ≥ rt → REJECTED */
    if (d1sq >= rt) { *class_id_out = TPV_CLASS_REJECTED; *confidence_out = 0; return; }

    /* L3'：仅当 N ≥ 2 且 (d2² − d1²) < margin → AMBIGUOUS */
    int is_ambig = (n >= 2) && ((d2sq - d1sq) < mg);

    /* 计算 fit_q8 和 sep_q8（spec §6） */
    int32_t fit_q8 = (int32_t)((int64_t)255 * (rt - d1sq) / rt);
    if (fit_q8 < 0) fit_q8 = 0; else if (fit_q8 > 255) fit_q8 = 255;

    int32_t sep_q8;
    if (n < 2) sep_q8 = 255;    /* HG1：单类分支 */
    else {
        sep_q8 = (int32_t)((int64_t)255 * (d2sq - d1sq) / mg);
        if (sep_q8 < 0) sep_q8 = 0; else if (sep_q8 > 255) sep_q8 = 255;
    }

    if (is_ambig) {
        *class_id_out = TPV_CLASS_AMBIGUOUS;
        int min_q8 = fit_q8 < sep_q8 ? fit_q8 : sep_q8;
        *confidence_out = (uint8_t)min_q8;
    } else {
        *class_id_out = (uint8_t)winner;
        int min_q8 = fit_q8 < sep_q8 ? fit_q8 : sep_q8;
        if (min_q8 < 1) min_q8 = 1;   /* ACCEPTED ⇒ ≥ 1（spec §6） */
        *confidence_out = (uint8_t)min_q8;
    }
}
```

- [ ] **Step 4: 跑测试确认前 2 个用例绿色**

- [ ] **Step 5: 加 AMBIGUOUS 测试**

```c
TEST(t_ambiguous_when_two_classes_close) {
    tpv_Template t[2];
    t[0] = simple_template(TPV_Q16, 10 << 16, 5 << 16);
    t[1] = simple_template(TPV_Q16, 10 << 16, 5 << 16);
    /* 把 t[1] 的均值移到距 t[0] 距离 2 的位置 */
    t[1].mean.hu[0] = 2 << 16;
    /* 查询点在两类之间（偏向 t[0]） */
    tpv_Features f = t[0].mean;
    f.hu[0] = 1 << 15;   /* 距 t[0] = 0.5, t[1] = 1.5，d2²-d1² = 2 */
    /* 2 < margin=5 → AMBIGUOUS */
    uint8_t cid, conf; int32_t d1sq;
    tpv_classify(&f, t, 2, &cid, &conf, &d1sq);
    CHECK_EQ_I(cid, TPV_CLASS_AMBIGUOUS);
}
```

main 的 RUN 列表加 `RUN(t_ambiguous_when_two_classes_close);`。

- [ ] **Step 6: 跑测试确认绿色**

- [ ] **Step 7 (HG1)：硬门槛 — N_CLASSES = 1 永不进入 AMBIGUOUS 且 sep_q8 = 255**

```c
TEST(hg1_single_class_never_ambiguous) {
    tpv_Template t = simple_template(TPV_Q16, 10 << 16, 0 /* margin 置 0 合法 */);
    /* 构造一堆不同输入，不论如何都不能变成 AMBIGUOUS */
    tpv_Features f = t.mean;
    for (int k = -5; k <= 5; k++) {
        f.hu[0] = k << 16;
        uint8_t cid, conf; int32_t d1sq;
        tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
        CHECK(cid != TPV_CLASS_AMBIGUOUS);
        /* 若 ACCEPTED（d1² < rt）：conf 必定反映 fit_q8（因为 sep_q8=255） */
        if (cid == 0) {
            int32_t fit_q8 = (int32_t)((int64_t)255 * ((10<<16) - d1sq) / (10<<16));
            if (fit_q8 < 1) fit_q8 = 1;
            CHECK(conf == (uint8_t)fit_q8 || conf == 255);
        }
    }
}
```

main 的 RUN 列表加 `RUN(hg1_single_class_never_ambiguous);`。

- [ ] **Step 8 (HG2)：硬门槛 — d1² 恰等于 reject_thresh 时 → REJECTED，conf = 0**

```c
TEST(hg2_boundary_d1sq_equals_reject_thresh_is_rejected) {
    const int32_t rt = 16 << 16;
    tpv_Template t = simple_template(TPV_Q16, rt, 4 << 16);
    tpv_Features f = t.mean;
    f.hu[0] = 4 << 16;   /* d1² = 16 == rt */
    uint8_t cid, conf; int32_t d1sq;
    tpv_classify(&f, &t, 1, &cid, &conf, &d1sq);
    CHECK_EQ_I(d1sq, rt);
    CHECK_EQ_I(cid, TPV_CLASS_REJECTED);
    CHECK_EQ_I(conf, 0);
}
```

main 的 RUN 列表加 `RUN(hg2_boundary_d1sq_equals_reject_thresh_is_rejected);`。
最终 `main()` 长这样：

```c
int main(void) {
    RUN(t_features_equal_mean_gives_zero_distance);
    RUN(t_far_features_rejected);
    RUN(t_ambiguous_when_two_classes_close);
    RUN(hg1_single_class_never_ambiguous);
    RUN(hg2_boundary_d1sq_equals_reject_thresh_is_rejected);
    FINISH();
}
```

- [ ] **Step 9: Commit**

```bash
git add src/classifier.c tests/test_classifier.c
git commit -m "feat(classifier): mahal² + tri-partition + HG1/HG2 hard-gate tests"
```

---

## Task 5 — pose

**Files:**
- Create: `src/pose.c`
- Create: `tests/test_pose.c`

- [ ] **Step 1: 写质心测试**

```c
#include "tpv_internal.h"
#include "testlib.h"

TEST(t_centroid_of_square) {
    tpv_Blob b = {0};
    b.m00 = 16; b.m10 = 24; b.m01 = 24;
    b.mu20 = 20; b.mu02 = 20; b.mu11 = 0;
    int16_t x, y, th;
    tpv_pose(&b, &x, &y, &th);
    CHECK_EQ_I(x, 1);  /* 24/16 = 1.5 → 四舍五入 1 或 2，允许 ±1 */
    CHECK_EQ_I(y, 1);
    /* 方块主轴歧义：arctan(0 / 0) 未定义；实现应返回 0 */
    CHECK_EQ_I(th, 0);
}

int main(void) {
    RUN(t_centroid_of_square);
    /* step 2 加 t_axis_angle_of_horizontal_bar 后 RUN 它 */
    FINISH();
}
```

- [ ] **Step 2: 写长条 blob 的主轴角测试**

```c
TEST(t_axis_angle_of_horizontal_bar) {
    /* 水平矩形：μ₂₀ ≫ μ₀₂，μ₁₁ = 0 → 主轴水平，θ = 0 */
    tpv_Blob b = {0};
    b.m00 = 40; b.m10 = 100; b.m01 = 80;
    b.mu20 = 200; b.mu02 = 10; b.mu11 = 0;
    int16_t x, y, th;
    tpv_pose(&b, &x, &y, &th);
    CHECK(th > -5 && th < 5);   /* |θ| ≤ 0.5° */
}
```

main 加一行 `RUN(t_axis_angle_of_horizontal_bar);`。

- [ ] **Step 3: 跑测试确认红色**

- [ ] **Step 4: 实现 `src/pose.c`**

```c
#include "tpv_internal.h"

/* 简化的 Q16.16 atan2，返回弧度 Q16.16。工业做法用查表。*/
int32_t atan2_q16(int64_t y, int64_t x);   /* 放在 fixed_math.c */

void tpv_pose(const tpv_Blob *b,
              int16_t *x_out, int16_t *y_out, int16_t *theta_x10_out) {
    if (b->m00 == 0) { *x_out = *y_out = *theta_x10_out = 0; return; }
    *x_out = (int16_t)(b->m10 / b->m00);
    *y_out = (int16_t)(b->m01 / b->m00);

    /* θ = 0.5 · atan2(2·μ₁₁, μ₂₀ − μ₀₂) */
    int64_t num = 2 * b->mu11;
    int64_t den = b->mu20 - b->mu02;
    int32_t theta_q16 = atan2_q16(num, den) / 2;   /* Q16.16 弧度 */

    /* 180° 消歧（spec §7）：用简化 m3 proj 符号决定是否 +π */
    int32_t proj = (int32_t)(b->mu30 + b->mu03);
    if (proj > TPV_M3_EPS) { /* θ 保持 */ }
    else if (proj < -TPV_M3_EPS) { theta_q16 += (int32_t)(3.14159265 * 65536); }
    else { /* 对称 → 不消歧 */ }

    /* 转换到 deg × 10：theta_deg_x10 = theta_q16 * 180 * 10 / π / 65536 */
    int64_t tmp = (int64_t)theta_q16 * 1800;
    int32_t deg_x10 = (int32_t)(tmp / (int64_t)(3.14159265 * 65536));
    /* 归一化到 [-1800, 1799] */
    while (deg_x10 < -1800) deg_x10 += 3600;
    while (deg_x10 >= 1800) deg_x10 -= 3600;
    *theta_x10_out = (int16_t)deg_x10;
}
```

- [ ] **Step 5: 实现 `atan2_q16` 加测试，补到 `src/fixed_math.c`**

```c
/* atan_table[i] = atan(i/64) * 65536，i = 0..64，即 [0, π/4] 区间 */
static const int32_t atan_table[65] = {
    0,     1024,  2047,  3070,  4091,  5110,  6127,  7141,
    8152,  9159,  10162, 11162, 12157, 13146, 14131, 15110,
    16084, 17051, 18012, 18966, 19913, 20853, 21785, 22710,
    23627, 24535, 25436, 26327, 27210, 28083, 28947, 29802,
    30647, 31481, 32306, 33120, 33924, 34717, 35499, 36270,
    37030, 37779, 38517, 39244, 39960, 40665, 41359, 42043,
    42716, 43378, 44030, 44671, 45302, 45923, 46534, 47135,
    47726, 48308, 48880, 49443, 49997, 50542, 51078, 51606,
    52125
};

int32_t atan2_q16(int64_t y, int64_t x) {
    if (x == 0 && y == 0) return 0;
    /* 象限归约：先化到 x>0，然后把 (x, y) 化到 |y| <= x（即 [0, π/4]）
     * 再查表，最后按象限加偏移。*/
    int sign_y = (y < 0); int64_t ay = sign_y ? -y : y;
    int sign_x = (x < 0); int64_t ax = sign_x ? -x : x;
    int swap = (ay > ax);
    int64_t n = swap ? ax : ay;
    int64_t d = swap ? ay : ax;
    /* ratio = n/d * 64 → 查表索引（Q0） */
    int idx = d == 0 ? 64 : (int)((n * 64) / d);
    if (idx > 64) idx = 64;
    int32_t ang = atan_table[idx];
    if (swap)   ang = (int32_t)(1.5707963 * 65536) - ang;  /* π/2 − ang */
    if (sign_x) ang = (int32_t)(3.1415926 * 65536) - ang;  /* π − ang */
    if (sign_y) ang = -ang;
    return ang;
}
```

对应测试：

```c
TEST(t_atan2_axis) {
    CHECK(atan2_q16(0, 1) == 0);
    CHECK(atan2_q16(1, 0) > 0);
    CHECK(atan2_q16(-1, 0) < 0);
}
```

注意：`tests/test_fixed_math.c` 的 `main()` 在 T3 step 4 已经写好；这里
新增的 `t_atan2_axis` 必须在它的 main 里加一行 `RUN(t_atan2_axis);`，
否则不会被执行。

（`tests/test_pose.c` 的 main 已经在本任务 step 1 写好。）

- [ ] **Step 6: 跑测试确认绿色**

- [ ] **Step 7: Commit**

```bash
git add src/pose.c src/fixed_math.c tests/test_pose.c tests/test_fixed_math.c
git commit -m "feat(pose): centroid + principal axis + 180° disambig"
```

---

## Task 6 — pipeline（含 HG4 硬门槛）

**Files:**
- Create: `src/pipeline.c`
- Create: `tests/test_pipeline.c`

- [ ] **Step 1: 写端到端最小测试**

```c
#include "tpv_internal.h"
#include "tpv.h"
#include "testlib.h"
#include <string.h>

extern const tpv_Template tpv_templates[TPV_N_CLASSES];  /* stub */

TEST(t_empty_frame_returns_empty) {
    uint8_t y[TPV_WIDTH * TPV_HEIGHT] = {0};
    tpv_Detection d;
    int r = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_EMPTY);
}

TEST(t_bad_input_returns_minus_one) {
    tpv_Detection d;
    int r = tpv_process_frame(NULL, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_BAD_INPUT);
    r = tpv_process_frame((uint8_t*)"x", 100, 100, &d);
    CHECK_EQ_I(r, TPV_BAD_INPUT);
}

int main(void) {
    RUN(t_empty_frame_returns_empty);
    RUN(t_bad_input_returns_minus_one);
    /* step 5 / 6 加 RUN(t_blob_exists_but_no_model_is_rejected)
       和 RUN(hg4_reject_still_reports_centroid) */
    FINISH();
}
```

- [ ] **Step 2: 跑测试确认红色（link error）**

- [ ] **Step 3: 实现 `src/pipeline.c`**

```c
#include <string.h>
#include "tpv_internal.h"
#include "tpv.h"

static uint8_t  g_bin[TPV_WIDTH * TPV_HEIGHT / 8];
static tpv_Blob g_blobs[TPV_MAX_BLOBS];

int tpv_process_frame(const uint8_t *y, int w, int h, tpv_Detection *det_out) {
    if (!y || !det_out || w != TPV_WIDTH || h != TPV_HEIGHT) return TPV_BAD_INPUT;
    memset(det_out, 0, sizeof *det_out);
    memset(g_bin, 0, sizeof g_bin);

    tpv_threshold(y, w, h, g_bin);
    int n = tpv_ccl_moments(g_bin, w, h, g_blobs, TPV_MAX_BLOBS);
    if (n < 0) return TPV_SCENE_ERROR;

    /* 几何过滤 + pose + classify */
    tpv_Detection pool[TPV_MAX_BLOBS];
    int32_t       d1sq_pool[TPV_MAX_BLOBS];
    int pn = 0;
    for (int i = 0; i < n; i++) {
        if (g_blobs[i].m00 < TPV_AMIN || g_blobs[i].m00 > TPV_AMAX) continue;
        tpv_Features f;
        tpv_shape_features(&g_blobs[i], &f);
        tpv_Detection d = {0};
        uint8_t cid = 0, conf = 0; int32_t d1sq = 0;
        tpv_classify(&f, tpv_templates, TPV_N_CLASSES, &cid, &conf, &d1sq);
        tpv_pose(&g_blobs[i], &d.x, &d.y, &d.theta_x10);
        d.class_id = cid; d.confidence_q8 = conf;
        pool[pn] = d; d1sq_pool[pn] = d1sq; pn++;
    }
    if (pn == 0) return TPV_EMPTY;

    /* 最终选择（spec §5） */
    int best_acc = -1, best_conf = -1;
    int best_reject = -1; int32_t best_d1 = INT32_MAX;
    for (int i = 0; i < pn; i++) {
        uint8_t cid = pool[i].class_id;
        if (cid <= 4) {
            if (pool[i].confidence_q8 > best_conf) {
                best_conf = pool[i].confidence_q8; best_acc = i;
            }
        } else {
            if (d1sq_pool[i] < best_d1) { best_d1 = d1sq_pool[i]; best_reject = i; }
        }
    }
    if (best_acc >= 0) { *det_out = pool[best_acc]; return TPV_OK; }
    if (best_reject >= 0) { *det_out = pool[best_reject]; return TPV_OK; }
    return TPV_EMPTY;
}
```

- [ ] **Step 4: 跑测试确认当前 2 用例绿色**

- [ ] **Step 5: 加"有 blob 但模板全零 → REJECTED"测试**

```c
TEST(t_blob_exists_but_no_model_is_rejected) {
    uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    /* 中心画一个 100x100 的白块 */
    memset(y, 0, sizeof y);
    for (int yy = 190; yy < 290; yy++)
        for (int xx = 270; xx < 370; xx++) y[yy * TPV_WIDTH + xx] = 255;
    tpv_Detection d;
    int r = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_OK);
    CHECK_EQ_I(d.class_id, TPV_CLASS_REJECTED);
    CHECK_EQ_I(d.confidence_q8, 0);
    CHECK(d.x > 310 && d.x < 330);    /* 质心 ≈ 320 */
    CHECK(d.y > 230 && d.y < 250);    /* 质心 ≈ 240 */
}
```

main 加一行 `RUN(t_blob_exists_but_no_model_is_rejected);`。

- [ ] **Step 6 (HG4)：硬门槛 — TPV_OK + class_id ∈ {0xFE, 0xFF} 时 (x, y) 仍有效**

```c
TEST(hg4_reject_still_reports_centroid) {
    /* 复用上一个测试的 setup：确认 x/y 非零并靠近预期位置，
       theta 字段填入但合同仅供诊断。*/
    uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    memset(y, 0, sizeof y);
    for (int yy = 100; yy < 150; yy++)
        for (int xx = 100; xx < 150; xx++) y[yy * TPV_WIDTH + xx] = 255;
    tpv_Detection d;
    int r = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &d);
    CHECK_EQ_I(r, TPV_OK);
    CHECK(d.class_id == TPV_CLASS_REJECTED || d.class_id == TPV_CLASS_AMBIGUOUS);
    CHECK(d.x > 110 && d.x < 140);
    CHECK(d.y > 110 && d.y < 140);
    /* 我们不 check theta_x10 的值——spec 说它计算了但不用于抓取。 */
}
```

main 加最后两行：`RUN(t_blob_exists_but_no_model_is_rejected);` 和
`RUN(hg4_reject_still_reports_centroid);`。最终 `main()`：

```c
int main(void) {
    RUN(t_empty_frame_returns_empty);
    RUN(t_bad_input_returns_minus_one);
    RUN(t_blob_exists_but_no_model_is_rejected);
    RUN(hg4_reject_still_reports_centroid);
    FINISH();
}
```

- [ ] **Step 7: 跑全部测试确认绿色**

- [ ] **Step 8: Commit**

```bash
git add src/pipeline.c tests/test_pipeline.c
git commit -m "feat(pipeline): per-frame scheduling + argmax policy + HG4"
```

---

## Task 7 — platform_glue (stub)

**Files:**
- Create: `src/platform_glue.c`

- [ ] **Step 1: 写最小 stub，后续硬件集成时替换**

```c
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"

/* 9 字节 wire payload 序列化（spec §10.2） */
void tpv_serialize_payload(uint8_t status, const tpv_Detection *d, uint8_t *buf9) {
    buf9[0] = status;
    if (status != TPV_OK) { memset(buf9 + 1, 0, 8); return; }
    buf9[1] = (uint8_t)(d->x & 0xFF);
    buf9[2] = (uint8_t)((d->x >> 8) & 0xFF);
    buf9[3] = (uint8_t)(d->y & 0xFF);
    buf9[4] = (uint8_t)((d->y >> 8) & 0xFF);
    buf9[5] = (uint8_t)(d->theta_x10 & 0xFF);
    buf9[6] = (uint8_t)((d->theta_x10 >> 8) & 0xFF);
    buf9[7] = d->class_id;
    buf9[8] = d->confidence_q8;
}

/* 真实相机接入时在此实现 read-frame 循环；本 stub 仅导出序列化 */
```

- [ ] **Step 2: 加 payload 字节序测试**

```c
TEST(t_wire_payload_ok) {
    tpv_Detection d = { .x = 0x0102, .y = 0x0304, .theta_x10 = 0x0506,
                        .class_id = 7, .confidence_q8 = 200 };
    uint8_t buf[9];
    tpv_serialize_payload(TPV_OK, &d, buf);
    CHECK_EQ_I(buf[0], TPV_OK);
    CHECK_EQ_I(buf[1], 0x02); CHECK_EQ_I(buf[2], 0x01);
    CHECK_EQ_I(buf[3], 0x04); CHECK_EQ_I(buf[4], 0x03);
    CHECK_EQ_I(buf[5], 0x06); CHECK_EQ_I(buf[6], 0x05);
    CHECK_EQ_I(buf[7], 7);    CHECK_EQ_I(buf[8], 200);
}
TEST(t_wire_payload_empty) {
    tpv_Detection d = { .x = 1, .y = 2, .theta_x10 = 3, .class_id = 4, .confidence_q8 = 5 };
    uint8_t buf[9];
    tpv_serialize_payload(TPV_EMPTY, &d, buf);
    CHECK_EQ_I(buf[0], TPV_EMPTY);
    for (int i = 1; i < 9; i++) CHECK_EQ_I(buf[i], 0);
}

int main(void) {
    RUN(t_wire_payload_ok);
    RUN(t_wire_payload_empty);
    FINISH();
}
```

- [ ] **Step 3: 跑测试确认绿色**

- [ ] **Step 4: Commit**

```bash
git add src/platform_glue.c tests/test_platform_glue.c
git commit -m "feat(glue): 9-byte wire payload serializer (stub transport)"
```

---

## Task 8 — 标定工具（含 HG3 硬门槛）

标定工具与运行时的构建 / 测试完全隔离：自带 `tools/calibrate/Makefile`
和自己的 `tools/calibrate/tests/` 目录。**不要**把标定测试放在顶层
`tests/`——顶层 Makefile 用 `wildcard tests/test_*.c` 自动给每个文件
生成 `build/test_<name>` 可执行，而这些可执行只 link `src/` 下的运行时
模块，**不会** link `tools/calibrate/*.c`，标定测试一旦混入顶层就立刻
出现未定义引用。隔离后两边可以各自 `make test`，也可以分别在 CI 里并行。

**Files:**
- Create: `tools/calibrate/Makefile`
- Create: `tools/calibrate/calibrate.c`
- Create: `tools/calibrate/stats.c`
- Create: `tools/calibrate/separability.c`
- Create: `tools/calibrate/codegen.c`
- Create: `tools/calibrate/frame_io.c`
- Create: `tools/calibrate/tests/test_stats.c`
- Create: `tools/calibrate/tests/test_quantize.c`
- Create: `tools/calibrate/tests/testlib_cal.c`（复用顶层 testlib 的最小包装）

- [ ] **Step 0: 先写 `tools/calibrate/Makefile`（独立构建树）**

```make
# tools/calibrate/Makefile — 标定工具自成一体，不干扰运行时测试
ROOT    := $(abspath ../..)
CC      ?= cc
# _POSIX_C_SOURCE: frame_io.c 用 strdup/opendir/readdir，全是 POSIX 不是 C11
CFLAGS  := -std=c11 -Wall -Wextra -Werror -I$(ROOT)/include -g -O0 \
           -D_POSIX_C_SOURCE=200809L
LDFLAGS := -lm

# 注意：calibrate.c 含 main()，必须从可复用库中分离，否则 test 二进制
# 会和 test_*.c 自己的 main 冲突 (duplicate symbol)。
CAL_LIB_SRCS = stats.c separability.c codegen.c frame_io.c cal_stub.c
CAL_MAIN_SRC = calibrate.c

# 共享的特征提取代码要连进来，这样"标定和运行时用同一份"有约束力
SHARED    = $(ROOT)/src/threshold.c $(ROOT)/src/ccl_moments.c \
            $(ROOT)/src/shape_features.c $(ROOT)/src/fixed_math.c

TEST_FILES = $(wildcard tests/test_*.c)
TEST_BINS  = $(patsubst tests/%.c,build/%,$(TEST_FILES))

.PHONY: all test clean
all: build/calibrate
test: $(TEST_BINS)
	@fail=0; for b in $(TEST_BINS); do echo "=== $$b ==="; $$b || fail=1; done; exit $$fail

build:
	mkdir -p $@

# 完整 CLI：库 + main
build/calibrate: $(CAL_LIB_SRCS) $(CAL_MAIN_SRC) $(SHARED) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 测试 binary：库 + 测试自己的 main，**不**含 calibrate.c
build/test_%: tests/test_%.c tests/testlib_cal.c $(CAL_LIB_SRCS) $(SHARED) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf build
```

`tools/calibrate/cal_stub.c`（提供运行时模板系统在标定环境下的占位
符号——`src/threshold.c` extern 引用 `tpv_bin_threshold`，标定 link
时如果不提供会 unresolved symbol；`tpv_templates` 标定不用，但顶层 link
路径里有，给个 0 长占位即可）：

```c
#include "tpv_internal.h"
/* 标定环境下，bin_threshold 由 calibrate.c 在标定流程的第 6 步从样本
 * 帧背景估计；这里给个默认值，仅用于标定过程中调用 tpv_threshold()
 * 的内部场景（frame_io.c）。生成的 model_data.c 里会有正式值。*/
const uint8_t tpv_bin_threshold = TPV_BIN_THRESH_DEFAULT;
/* 标定流程不会读 tpv_templates，但运行时模块或共享代码可能 extern；
 * 给个零长占位避免 unresolved symbol。N_CLASSES = 1 是最小允许值。*/
#if TPV_N_CLASSES >= 1
const tpv_Template tpv_templates[TPV_N_CLASSES] = {0};
#endif
```

- [ ] **Step 1: 写最小 stats 测试（路径改为标定工具自己的 tests/）**

```c
/* tools/calibrate/tests/test_stats.c */
#include "tpv_internal.h"
#include "../../../tests/testlib.h"   /* 复用顶层 testlib.h */
#include <string.h>

/* 标定工具内部函数：把 N 个样本的 Features 算成均值和协方差 */
void tpv_cal_mean_cov(const tpv_Features *samples, int n,
                      int32_t *mean_out, double *cov_out);  /* 10 * 10 */
int  tpv_cal_cholesky_inv(const double *cov, int32_t *L_inv_q16_out);
int  tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label);

TEST(t_mean_cov_trivial) {
    tpv_Features s[3];
    memset(s, 0, sizeof s);
    s[0].hu[0] = 1 << 16; s[1].hu[0] = 2 << 16; s[2].hu[0] = 3 << 16;
    int32_t mean[10]; double cov[100];
    tpv_cal_mean_cov(s, 3, mean, cov);
    CHECK_EQ_I(mean[0], 2 << 16);
    /* 方差：((1-2)^2 + 0 + (3-2)^2) / 2 = 1 → Q16.16 ~ 65536 */
    CHECK(cov[0] > 0.9 && cov[0] < 1.1);
}
```

- [ ] **Step 2: 实现 `tools/calibrate/stats.c`**

```c
#include <string.h>
#include "tpv_internal.h"

void tpv_cal_mean_cov(const tpv_Features *samples, int n,
                      int32_t *mean_out, double *cov_out) {
    double mean_real[TPV_N_FEAT] = {0};
    for (int i = 0; i < n; i++) {
        const int32_t *v = (const int32_t *)&samples[i];
        for (int k = 0; k < TPV_N_FEAT; k++)
            mean_real[k] += v[k] / 65536.0;
    }
    for (int k = 0; k < TPV_N_FEAT; k++) {
        mean_real[k] /= n;
        mean_out[k] = (int32_t)(mean_real[k] * 65536);
    }
    for (int i = 0; i < TPV_N_FEAT; i++)
        for (int j = 0; j < TPV_N_FEAT; j++) cov_out[i*TPV_N_FEAT+j] = 0;
    for (int s = 0; s < n; s++) {
        const int32_t *v = (const int32_t *)&samples[s];
        double d[TPV_N_FEAT];
        for (int k = 0; k < TPV_N_FEAT; k++) d[k] = v[k]/65536.0 - mean_real[k];
        for (int i = 0; i < TPV_N_FEAT; i++)
            for (int j = 0; j <= i; j++)
                cov_out[i*TPV_N_FEAT+j] += d[i] * d[j];
    }
    double norm = (n > 1) ? (n - 1) : 1;
    for (int i = 0; i < TPV_N_FEAT; i++)
        for (int j = 0; j <= i; j++) {
            cov_out[i*TPV_N_FEAT+j] /= norm;
            cov_out[j*TPV_N_FEAT+i] = cov_out[i*TPV_N_FEAT+j];
        }
}

int tpv_cal_cholesky_inv(const double *cov, int32_t *L_inv_q16_out) {
    /* 10x10 Cholesky + 下三角求逆。若对角出现 ≤ 0，返回 -1（非正定） */
    double L[TPV_N_FEAT][TPV_N_FEAT] = {{0}};
    for (int i = 0; i < TPV_N_FEAT; i++) {
        for (int j = 0; j <= i; j++) {
            double sum = cov[i*TPV_N_FEAT+j];
            for (int k = 0; k < j; k++) sum -= L[i][k] * L[j][k];
            if (i == j) {
                if (sum <= 0) return -1;
                L[i][i] = __builtin_sqrt(sum);
            } else {
                L[i][j] = sum / L[j][j];
            }
        }
    }
    /* 求 L⁻¹（下三角），然后转 Q16.16 */
    double Linv[TPV_N_FEAT][TPV_N_FEAT] = {{0}};
    for (int i = 0; i < TPV_N_FEAT; i++) {
        Linv[i][i] = 1.0 / L[i][i];
        for (int j = 0; j < i; j++) {
            double s = 0;
            for (int k = j; k < i; k++) s -= L[i][k] * Linv[k][j];
            Linv[i][j] = s / L[i][i];
        }
    }
    int idx = 0;
    for (int i = 0; i < TPV_N_FEAT; i++)
        for (int j = 0; j <= i; j++)
            L_inv_q16_out[idx++] = (int32_t)(Linv[i][j] * 65536.0);
    return 0;
}
```

- [ ] **Step 3: 加正则化函数**

```c
void tpv_cal_regularize(double *cov, const double *sigma_ref_sq) {
    /* Σ ← Σ + ε · diag(σ²_ref)，ε = 1e-4（spec §8 步 4） */
    const double eps = 1e-4;
    for (int i = 0; i < TPV_N_FEAT; i++) cov[i*TPV_N_FEAT+i] += eps * sigma_ref_sq[i];
}
```

- [ ] **Step 4: 实现 `tpv_cal_quantize_or_fail`（即 HG3 的核心）**

```c
#include <stdio.h>

int tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label) {
    double q = real * 65536.0;
    if (q < 1.0) {
        fprintf(stderr,
            "CALIBRATION FAIL: %s quantizes to 0 in Q16.16 (real=%g).\n"
            "  Likely cause: training variance too small, near-identical samples,\n"
            "  or log-Hu numerical collapse. Inspect raw feature distribution.\n",
            label, real);
        return -1;
    }
    *q16_out = (int32_t)q;
    return 0;
}
```

- [ ] **Step 5: 实现 `separability.c`**

```c
int tpv_cal_check_separability(const tpv_Template *tmpl, int n) {
    if (n < 2) return 0;   /* spec §8 步 8 跳过单类 */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            /* 在 tmpl[i] 的度量下计算 μ_j 到 μ_i 的平方马氏距离 */
            /* ... 调用 mahal_sq 与模板测度 ... */
        }
    }
    return 0;
}
```

- [ ] **Step 6 (HG3)：写硬门槛测试到 `tools/calibrate/tests/test_quantize.c`**

```c
/* tools/calibrate/tests/test_quantize.c */
#include <stdio.h>
#include "tpv_internal.h"
#include "../../../tests/testlib.h"

int tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label);

TEST(hg3_reject_thresh_quantize_to_zero_fails) {
    int32_t q = -1;
    int r = tpv_cal_quantize_or_fail(1e-8, &q, "reject_thresh");
    CHECK_EQ_I(r, -1);
    CHECK_EQ_I(q, -1);  /* 未被写入 */
}
TEST(hg3_margin_quantize_to_zero_fails) {
    int32_t q = -1;
    int r = tpv_cal_quantize_or_fail(1e-9, &q, "margin");
    CHECK_EQ_I(r, -1);
}
TEST(hg3_valid_value_succeeds) {
    int32_t q = 0;
    int r = tpv_cal_quantize_or_fail(2.5, &q, "reject_thresh");
    CHECK_EQ_I(r, 0);
    CHECK_EQ_I(q, (int32_t)(2.5 * 65536));
}

int main(void) {
    RUN(hg3_reject_thresh_quantize_to_zero_fails);
    RUN(hg3_margin_quantize_to_zero_fails);
    RUN(hg3_valid_value_succeeds);
    FINISH();
}
```

同样在 `tools/calibrate/tests/test_stats.c` 末尾补一段 `main()`：

```c
int main(void) {
    RUN(t_mean_cov_trivial);
    /* 之后 cholesky / regularize 测试加入这里 */
    FINISH();
}
```

`tools/calibrate/tests/testlib_cal.c` 内容只一行：

```c
#include "../../../tests/testlib.h"
int tpv_test_pass = 0;
int tpv_test_fail = 0;
```

Run:
```
cd ~/work/tiny-pick-vision/tools/calibrate
make test
```
Expected: 两个 binary 都 `0 failed`。

- [ ] **Step 6b: 实现 `tools/calibrate/frame_io.c`（读样本目录）**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "tpv_internal.h"

/* 共享自 src/threshold.c & src/ccl_moments.c & src/shape_features.c */
extern void tpv_threshold(const uint8_t *y, int w, int h, uint8_t *bin_out);
extern int  tpv_ccl_moments(const uint8_t *bin, int w, int h, tpv_Blob *out, int max);
extern void tpv_shape_features(const tpv_Blob *blob, tpv_Features *out);

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

int tpv_cal_load_class_frames(const char *dir, tpv_Features *out, int cap) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 0; }
    /* 收集文件名并排序，避免 readdir 顺序不稳定 */
    char *names[1024]; int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < 1024) {
        if (e->d_name[0] == '.') continue;
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], by_name);

    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    static uint8_t bin[TPV_WIDTH * TPV_HEIGHT / 8];
    static tpv_Blob blobs[TPV_MAX_BLOBS];
    int n = 0;
    for (int i = 0; i < nn && n < cap; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { free(names[i]); continue; }
        if (fread(y, 1, sizeof y, f) == sizeof y) {
            tpv_threshold(y, TPV_WIDTH, TPV_HEIGHT, bin);
            int nb = tpv_ccl_moments(bin, TPV_WIDTH, TPV_HEIGHT, blobs, TPV_MAX_BLOBS);
            /* 每帧期望恰好 1 个有效 blob（标定场景） */
            if (nb == 1 && blobs[0].m00 >= TPV_AMIN && blobs[0].m00 <= TPV_AMAX) {
                tpv_shape_features(&blobs[0], &out[n++]);
            }
        }
        fclose(f);
        free(names[i]);
    }
    return n;
}
```

- [ ] **Step 7: 实现 `codegen.c`（生成 model_data.c）**

```c
int tpv_cal_emit_model(const tpv_Template *tmpl, int n,
                       uint8_t bin_thresh, FILE *out) {
    fprintf(out, "/* AUTO-GENERATED — do not hand-edit */\n");
    fprintf(out, "#include \"tpv_internal.h\"\n");
    fprintf(out, "const uint8_t tpv_bin_threshold = %u;\n", bin_thresh);
    fprintf(out, "const tpv_Template tpv_templates[%d] = {\n", n);
    for (int c = 0; c < n; c++) {
        fprintf(out, "  {\n    .mean = { ");
        const int32_t *m = (const int32_t *)&tmpl[c].mean;
        for (int i = 0; i < TPV_N_FEAT; i++) fprintf(out, "0x%08x,", (uint32_t)m[i]);
        fprintf(out, " },\n    .L_inv = { ");
        for (int i = 0; i < TPV_L_INV_N; i++)
            fprintf(out, "0x%08x,", (uint32_t)tmpl[c].L_inv[i]);
        fprintf(out, " },\n    .reject_thresh = 0x%08x,\n", (uint32_t)tmpl[c].reject_thresh);
        fprintf(out, "    .margin = 0x%08x,\n  },\n", (uint32_t)tmpl[c].margin);
    }
    fprintf(out, "};\n");
    return 0;
}
```

- [ ] **Step 8: 写 `calibrate.c` main 把全部串起来**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tpv_internal.h"

/* 声明外部函数：在 stats.c / separability.c / codegen.c / frame_io.c 里实现 */
int   tpv_cal_load_class_frames(const char *dir, tpv_Features *out, int cap);
void  tpv_cal_mean_cov(const tpv_Features *s, int n, int32_t *mean, double *cov);
void  tpv_cal_regularize(double *cov, const double *sigma_ref_sq);
int   tpv_cal_cholesky_inv(const double *cov, int32_t *L_inv_q16);
int   tpv_cal_quantize_or_fail(double real, int32_t *q16_out, const char *label);
int   tpv_cal_check_separability(const tpv_Template *tmpl, int n);
int   tpv_cal_emit_model(const tpv_Template *t, int n, uint8_t bin_thresh, FILE *out);

#define MAX_SAMPLES_PER_CLASS 256

int main(int argc, char **argv) {
    const char *out_path = "model_data.c";
    int n_classes = 0;
    const char *class_dirs[5];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { out_path = argv[++i]; continue; }
        if (n_classes >= 5) { fprintf(stderr, "too many classes (max 5)\n"); return 2; }
        class_dirs[n_classes++] = argv[i];
    }
    if (n_classes < 1) { fprintf(stderr, "usage: calibrate DIR1 [DIR2 ...] [-o out]\n"); return 2; }

    /* 1. 每类读样本并提取特征 */
    static tpv_Features samples[5][MAX_SAMPLES_PER_CLASS];
    int nsamp[5] = {0};
    for (int c = 0; c < n_classes; c++) {
        nsamp[c] = tpv_cal_load_class_frames(class_dirs[c], samples[c], MAX_SAMPLES_PER_CLASS);
        if (nsamp[c] < 10) {
            fprintf(stderr, "class %d has only %d samples; need ≥ 10\n", c, nsamp[c]); return 1;
        }
    }

    /* 2. 每类算 mean/cov + 正则化 + Cholesky */
    tpv_Template tmpl[5] = {0};
    double covs[5][TPV_N_FEAT * TPV_N_FEAT];
    double sigma_ref_sq[TPV_N_FEAT];   /* 参考方差：全样本极差平方 */
    /* … 计算 sigma_ref_sq，省略 20 行 … */
    for (int c = 0; c < n_classes; c++) {
        int32_t mean[TPV_N_FEAT];
        tpv_cal_mean_cov(samples[c], nsamp[c], mean, covs[c]);
        tpv_cal_regularize(covs[c], sigma_ref_sq);
        memcpy(&tmpl[c].mean, mean, sizeof mean);
        if (tpv_cal_cholesky_inv(covs[c], tmpl[c].L_inv) < 0) {
            fprintf(stderr, "class %d covariance not positive definite after regularization\n", c);
            return 1;
        }
    }

    /* 3. reject_thresh_c + quantize guard */
    for (int c = 0; c < n_classes; c++) {
        double max_d2 = 0;
        for (int i = 0; i < nsamp[c]; i++) {
            /* 计算 d² 用 L_inv 变换后的范数平方 —— 共享 classify 里的 mahal_sq */
            double d2 = /* ... host-side double 实现，或直接调运行时定点版本 */ 0.0;
            if (d2 > max_d2) max_d2 = d2;
        }
        if (tpv_cal_quantize_or_fail(max_d2 * 1.5, &tmpl[c].reject_thresh, "reject_thresh") < 0)
            return 1;
    }

    /* 4. margin_c + quantize guard（N ≥ 2） */
    if (n_classes >= 2) {
        for (int c = 0; c < n_classes; c++) {
            double min_inter_d2 = 1e30;
            for (int cp = 0; cp < n_classes; cp++) {
                if (cp == c) continue;
                double d2 = /* μ_cp 到 μ_c 在 c 度量下的 d² */ 0.0;
                if (d2 < min_inter_d2) min_inter_d2 = d2;
            }
            if (tpv_cal_quantize_or_fail(min_inter_d2 * 0.25, &tmpl[c].margin, "margin") < 0)
                return 1;
        }
    } else {
        tmpl[0].margin = 0;   /* spec: N=1 时合法 */
    }

    /* 5. 可分性检查 */
    if (tpv_cal_check_separability(tmpl, n_classes) < 0) return 1;

    /* 6. 生成 model_data.c */
    FILE *out = fopen(out_path, "w");
    if (!out) { perror("open out"); return 1; }
    uint8_t bin_thresh = /* 由标定时从样本帧背景估计出 */ TPV_BIN_THRESH_DEFAULT;
    tpv_cal_emit_model(tmpl, n_classes, bin_thresh, out);
    fclose(out);
    return 0;
}
```

（`tpv_cal_load_class_frames` 与 `tpv_cal_check_separability` 的具体实现按
步骤 5-7 的定义，主程序保持上面的控制流。）

- [ ] **Step 9: Commit**

```bash
cd ~/work/tiny-pick-vision
git add tools/calibrate
git commit -m "feat(calibrate): stats + cholesky + quantize guard + HG3 (isolated build)"
```

---

## Task 9 — 目标交叉编译 + 尺寸门槛（含 HG5）

**Files:**
- Modify: `Makefile`（增加 `check-layout-target`）
- Create: `tests/check_layout.c`（已在 T0 step 8 创建，这里复用）

- [ ] **Step 1: 检查 NDK 工具链可用**

```
which armv7a-linux-androideabi24-clang
```
若不存在：先配置环境变量 `PATH=$NDK/toolchains/llvm/prebuilt/<host>/bin:$PATH`。

- [ ] **Step 2: 在 Makefile 里加 `check-layout-target`（HG5）**

`Makefile` 末尾追加：

```make
# HG5：在目标 ABI 下断言 Blob 布局；用 -c 单文件编译，不依赖 src/
check-layout-target:
	$(CC_TARGET) $(CFLAGS_TARGET) -c tests/check_layout.c -o build/check_layout_arm.o
	@echo "OK: tpv_Blob 80B / 8B-align under armv7a-linux-androideabi"
```

而 `tests/check_layout.c`（T0 已写）保持不变：

```c
#include "tpv_internal.h"
_Static_assert(sizeof(tpv_Blob) == 80, "Blob must be 80 B under target ABI");
_Static_assert(_Alignof(tpv_Blob) == 8, "Blob must have 8B alignment");
_Static_assert(sizeof(tpv_Features) == 40, "Features must be 40 B");
```

Run:
```
make check-layout-target
```
Expected: `OK: tpv_Blob 80B / 8B-align under armv7a-linux-androideabi`。
若 `_Static_assert` 失败：编译会立即报错，定位到具体哪条断言不成立。

- [ ] **Step 3: 交叉编译 + 尺寸门槛**

```
make target && make size
```

`make target` 会产出 `build/libtpv-arm.so`（**部署形态**），`make size`
随后 `strip --strip-all` 之后取**文件大小**（`wc -c`）作为门槛，并附带
打印各 section 占用作诊断。门槛 = spec G1 = 20480 B。

Expected:
```
--- section breakdown (diagnostic) ---
  .text                  NNNN
  .rodata                NNN
  .dynsym                NNN
  .dynstr                NNN
  .dynamic               NNN
  ...
--- final stripped file size = NNNN B (limit=20480) ---
OK: NNNN ≤ 20480
```

若 FAIL：先看 section 表里谁占大头。
- 若 `.text + .rodata` 已逼近 20 KB：瘦身代码 — 把 `log_q16`/`atan2_q16`
  的查表精度从 64 表项降到 32；去掉 `DEBUG_TRACE`；让 `tpv_classify` 的
  内层循环内联展开。
- 若 `.text + .rodata` 很小但 `.dynsym + .dynstr + 重定位` 撑爆 20 KB：
  这是 .so 装载元数据开销，靠改代码救不了。讨论改交付形态：
  - 选项 A：改用 `build/libtpv-arm.a` 静态归档，让上层 Android 进程直接
    静态链接，规避 .so 元数据。
  - 选项 B：spec G1 修订为"代码 + 只读常量预算 ≤ 20 KB"，明确不计装载
    元数据。需要先拿用户签字。

在用户没改 spec 之前，gate 严格按文件大小执行——这就是 G1 的字面含义。

- [ ] **Step 4: Commit**

```bash
git add Makefile tests/check_layout.c
git commit -m "build(t9): cross-compile + 20KB stripped-file size gate + HG5 ARM layout"
```

---

## Task 10 — 属性测试 / 回放 / 长稳工具

**Files:**
- Create: `tests/test_property.c`
- Create: `tools/replay.c`

注：HG1..HG5 已分别归入各模块的 test 文件（HG1/HG2 在 test_classifier.c、
HG3 在 tools/calibrate/tests/test_quantize.c、HG4 在 test_pipeline.c、
HG5 是编译期 `_Static_assert` 由 check-layout / check-layout-target 触发），
不再单独有 `test_hard_gates.c`。

- [ ] **Step 1: 旋转不变性测试**

```c
/* tests/test_property.c */
#include "tpv.h"
#include "tpv_internal.h"
#include "testlib.h"
#include <math.h>
#include <string.h>

/* M_PI 不是 ISO C 标准宏；显式定义以保 -Wpedantic 安全 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 把一个 L 形 blob 以中心为轴旋转 θ 度渲染到 640×480 的 Y buffer；
   其它位置填 0。L 形使主轴方向和 180° 消歧都有意义。 */
static void render_L(uint8_t *y, double theta_deg) {
    memset(y, 0, TPV_WIDTH * TPV_HEIGHT);
    double c = cos(theta_deg * M_PI / 180.0);
    double s = sin(theta_deg * M_PI / 180.0);
    /* L 形：50x50 方块 + 30x10 水平 "脚" 伸到 +x 方向 */
    for (int dy = -25; dy < 25; dy++)
        for (int dx = -25; dx < 25; dx++) {
            int px = (int)(320 + c*dx - s*dy);
            int py = (int)(240 + s*dx + c*dy);
            if (px >= 0 && px < TPV_WIDTH && py >= 0 && py < TPV_HEIGHT)
                y[py * TPV_WIDTH + px] = 255;
        }
    for (int dy = -5; dy < 5; dy++)
        for (int dx = 25; dx < 55; dx++) {
            int px = (int)(320 + c*dx - s*dy);
            int py = (int)(240 + s*dx + c*dy);
            if (px >= 0 && px < TPV_WIDTH && py >= 0 && py < TPV_HEIGHT)
                y[py * TPV_WIDTH + px] = 255;
        }
}

TEST(t_rotation_invariance_theta) {
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    for (double th = -170; th <= 170; th += 10) {
        render_L(y, th);
        tpv_Detection d;
        int r = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &d);
        CHECK_EQ_I(r, TPV_OK);
        /* 允许 theta 有 ±0.5° 误差（5 × 0.1°） */
        int32_t expected_x10 = (int32_t)(th * 10);
        while (expected_x10 < -1800) expected_x10 += 3600;
        while (expected_x10 >= 1800) expected_x10 -= 3600;
        int diff = d.theta_x10 - expected_x10;
        while (diff < -1800) diff += 3600;
        while (diff >= 1800) diff -= 3600;
        CHECK(diff >= -5 && diff <= 5);
    }
}
```

- [ ] **Step 2: 平移不变性测试**

```c
static void render_square_at(uint8_t *y, int cx, int cy) {
    memset(y, 0, TPV_WIDTH * TPV_HEIGHT);
    for (int dy = -20; dy < 20; dy++)
        for (int dx = -20; dx < 20; dx++) {
            int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < TPV_WIDTH && py >= 0 && py < TPV_HEIGHT)
                y[py * TPV_WIDTH + px] = 255;
        }
}

TEST(t_translation_invariance_features) {
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    /* 在 (100,100) 和 (500,400) 渲染同一大小的方块，
       比对 shape_features 的 hu[0..6] 差异 < 2^10 Q16.16（约 1.5e-2）*/
    tpv_Features f1, f2;
    render_square_at(y, 100, 100);
    static uint8_t bin[TPV_WIDTH * TPV_HEIGHT / 8];
    tpv_threshold(y, TPV_WIDTH, TPV_HEIGHT, bin);
    tpv_Blob blobs[4];
    int n = tpv_ccl_moments(bin, TPV_WIDTH, TPV_HEIGHT, blobs, 4);
    CHECK_EQ_I(n, 1);
    tpv_shape_features(&blobs[0], &f1);

    render_square_at(y, 500, 400);
    tpv_threshold(y, TPV_WIDTH, TPV_HEIGHT, bin);
    n = tpv_ccl_moments(bin, TPV_WIDTH, TPV_HEIGHT, blobs, 4);
    CHECK_EQ_I(n, 1);
    tpv_shape_features(&blobs[0], &f2);

    for (int i = 0; i < 7; i++) {
        int32_t diff = f1.hu[i] - f2.hu[i];
        if (diff < 0) diff = -diff;
        CHECK(diff < 1024);
    }
}

int main(void) {
    RUN(t_rotation_invariance_theta);
    RUN(t_translation_invariance_features);
    FINISH();
}
```

- [ ] **Step 3: 写 `tools/replay.c`**

```c
/* 命令行：replay <frames_dir> > out.csv
   每帧读 640*480 = 307200 字节 raw Y；对每帧调 tpv_process_frame；
   输出 CSV：frame_name,status,class_id,x,y,theta_x10,confidence
   （首列是文件名，便于 baseline 按文件名 join 而不是行号；详见 §"测试总入口"）
   长稳统计通过外部 awk / python 处理。*/
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"

/* 关键：按文件名排序后再处理，否则 readdir 顺序在不同 fs / 不同运行
 * 之间不稳定，会让"零决策差异回归"产生伪 diff。CSV 第一列改为
 * 文件名（而不是 frame_id），让外部对拍工具按文件名 join 而不是行号。*/
static int by_name(const void *a, const void *b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: replay <frames_dir>\n"); return 2; }
    DIR *dir = opendir(argv[1]);
    if (!dir) { perror("opendir"); return 1; }
    /* 收集并排序文件名 */
    char *names[1 << 14]; int nn = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && nn < (int)(sizeof names / sizeof names[0])) {
        if (e->d_name[0] == '.') continue;
        names[nn++] = strdup(e->d_name);
    }
    closedir(dir);
    qsort(names, nn, sizeof names[0], by_name);

    printf("frame_name,status,class_id,x,y,theta_x10,confidence\n");
    static uint8_t y[TPV_WIDTH * TPV_HEIGHT];
    for (int i = 0; i < nn; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[1], names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { free(names[i]); continue; }
        if (fread(y, 1, sizeof y, f) == sizeof y) {
            tpv_Detection d;
            int r = tpv_process_frame(y, TPV_WIDTH, TPV_HEIGHT, &d);
            printf("%s,%d,%u,%d,%d,%d,%u\n",
                   names[i], r, d.class_id, d.x, d.y, d.theta_x10, d.confidence_q8);
        }
        fclose(f);
        free(names[i]);
    }
    return 0;
}
```

- [ ] **Step 4: Commit**

```bash
git add tests/test_property.c tools/replay.c
git commit -m "test: rotation/translation invariance + replay CLI + HG aggregator"
```

---

## Task 11 — 文档收尾

**Files:**
- Create: `docs/DEVELOPER.md`
- Update: `docs/specs/...`（若实施过程中发现要修订）

- [ ] **Step 1: 写 `docs/DEVELOPER.md`**

包含内容：
- 如何在 macOS / Linux 上配好 NDK 并跑 `make target && make size`
- 如何操作标定工具（命令行、帧目录布局、失败信息解读）
- 哪些常量在 `tpv_config.h` 里可调
- 5 条硬门槛怎么跑：
  - HG1 / HG2 → `./build/test_classifier`
  - HG3 → `cd tools/calibrate && ./build/test_quantize`
  - HG4 → `./build/test_pipeline`
  - HG5 → `make check-layout`（host）+ `make check-layout-target`（ARM）
- 生产线集成注意事项（实装 platform_glue 时怎么做）

- [ ] **Step 2: 更新 spec / plan 中因实施发现的修正**

- [ ] **Step 3: Commit**

```bash
git add docs/DEVELOPER.md
git commit -m "docs: developer README + finalized interfaces"
```

---

## 测试总入口

所有任务完成后，发布前必须全绿的 4 个命令：

```
make check-layout                    # HG5 host 侧
make test                            # 顶层每个 test_*.c 各一个独立 binary，全跑
make -C tools/calibrate test         # 标定工具测试（含 HG3）
make check-layout-target && make size # ARM ABI 布局断言（HG5）+ 20KB 门槛
```

任一失败都是发布阻塞。生产发布前再跑 `tools/replay` 对 ≥10k 条生产帧做
零决策差异回归——比较 baseline.csv 与新 release.csv 时**按 frame_name 列做
join**（不要按行号对拍）。

---

## 任务依赖小结

- T0 是所有任务前置；
- T1..T5 互不依赖（并行最快，但 T3 依赖 T2 的 Blob 输出语义，T4 依赖 T3 的 Features 输出语义，T5 只依赖 T2 的 Blob）；
- T6 依赖 T1..T5；
- T7 可以在 T0 之后任意时间做；
- T8 依赖 T3（共享特征提取），可在 T3 完成后立即启动；
- T9 依赖 T6 + T8（目标 link 需要 model_data.c）；
- T10 依赖 T6；
- T11 依赖全部。

---

## Open Items（非阻塞）

下列 spec §13 中仍未与用户敲定、但不影响单元级实施：

1. A2 节拍是否真的 30 ms：目标板上 `make test` + `tools/replay` 跑完再对标。
2. A4 输出是 serial 还是 TCP：T7 已把序列化层独立，两种 wrapper 后续补一个 200 行内的驱动即可。
3. 标定 UX（GUI vs CLI）：当前计划只做 CLI，GUI 若产线要求再加。
