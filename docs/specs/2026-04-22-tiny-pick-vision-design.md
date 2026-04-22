# Tiny Pick Vision — Design Spec

Date: 2026-04-22
Status: Draft (awaiting user review)

## 1. Background

An industrial robotic arm needs to identify and locate objects on a work surface so
it can pick them up. The vision code is an embedded C module running on the arm's
controller board (a very low-end Android board). The controller takes the output
`(class, x, y, θ, confidence)` and plans the pick.

This document is the design for that vision module only. The robot-side motion
planning, gripper control, and post-pick verification are out of scope.

## 2. Goals and Non-Goals

### Goals

| # | Requirement | Target |
|---|---|---|
| G1 | Binary size (stripped) | ≤ 20 KB |
| G2 | Third-party dependencies | None (libc and NDK toolchain only) |
| G3 | Target hardware | Worst case: 1-core ARMv7 @ ~800 MHz, 512 MB RAM |
| G4 | Per-frame latency | ≤ 30 ms on target hardware |
| G5 | End-to-end reliability | 6σ-capable *as part of a system* (see §7) |
| G6 | Determinism | Same input bits → same output bits |

### Non-Goals

- Generic object detection on unknown classes.
- Cluttered bin picking (stacked / occluded / 3D pose).
- On-device training or online addition of new object classes.
- Illumination robustness beyond the fixed work-cell lighting.
- Color-based discrimination (operate on Y channel only).

## 3. Scope and Assumptions

All of the following are **hard preconditions** — if violated, the module may
output `REJECTED` but is not required to be correct.

### 3.1 Scene

- Objects lie flat on a **fixed-color background board** (e.g., black anti-static mat).
- Objects **do not touch**, **do not stack**, and **do not overlap**. A minimum
  inter-object gap (≥ 2 px at working resolution, i.e. ≥ ~1.5 mm for a typical
  FOV) is enforced by the **upstream tooling / feeder / tray design**, not by
  the vision module. The rationale is that a reliable physical gap is cheaper
  and more provable than any blob-separation algorithm within the 20 KB
  budget; it also removes an entire class of failure modes (silently merged
  contours) from the 6σ accounting.
- **One pick per cycle**: the module picks the single most-confident target per frame.
- Object count per frame: 1 to ~30.
- Object classes: **≤ 5 known classes**, fixed at compile time for a given production run.
- Objects are **predominantly geometric** (regular or irregular polygons, rounded shapes).

### 3.2 Optics

- Camera is **eye-in-hand**, mounted on the arm's end-effector.
- Before each detection cycle the arm moves to a **fixed overhead pose**, so
  working distance and optical axis are constant per run.
- Sensor: external USB/UVC camera, Y-channel accessible (YUV or grayscale).
- Resolution: **640 × 480** working resolution. Higher-resolution cameras are
  downsampled to this before processing.
- Lighting: **fixed work-cell light source**, no direct sunlight, no ambient drift.

### 3.3 Default Assumptions (subject to user validation)

These were proposed during brainstorming and are adopted as defaults. The user
should validate them before implementation begins.

| Ref | Assumption |
|---|---|
| A1 | 6σ is achieved at the **system level**: algorithm gives a confidence and can reject; the robot verifies with force / seating / re-imaging feedback after pick. The algorithm itself is not expected to be 6σ on its own. |
| A2 | Cycle budget: **≤ 30 ms** for vision, leaving headroom for arm motion within a typical 100 ms/piece target. |
| A3 | Calibration happens **offline on a PC** using the identical feature-extractor code. Templates are embedded as `const` data in the firmware. No field retraining. |
| A4 | Output interface: single detection per frame as a compact binary frame over serial **or** JSON over TCP. Final choice deferred; both share the same payload schema. |
| A5 | Working resolution is fixed at **640 × 480**. |
| A6 | Toolchain: Android NDK `armv7a-linux-androideabi-clang`, `-Os -flto`, no RTTI, no exceptions, freestanding-friendly. |

## 4. Architecture

### 4.1 Module Layering and Size Budget

Every module is a pure function. No global mutable state. No dynamic allocation.
Every working buffer is a fixed-size `.bss` array.

```
┌────────────────────────────────────────────────┐
│ platform_glue.c     ~2 KB   Camera I/O, result │
│                             output transport    │
├────────────────────────────────────────────────┤
│ pipeline.c          ~1 KB   Per-frame scheduler │
├────────────────────────────────────────────────┤
│ threshold.c         ~0.5 KB Y → bitmap          │
│ ccl_moments.c       ~3 KB   CCL + moment sums   │
│ shape_features.c    ~1 KB   Moments → features  │
│ classifier.c        ~1 KB   Mahalanobis + reject│
│ pose.c              ~0.5 KB Pose + 180° disambig│
├────────────────────────────────────────────────┤
│ model_data.c        ~1 KB   Template constants  │
└────────────────────────────────────────────────┘
Subtotal:            ~10 KB
```

After `-Os -flto -s` the expected footprint is ~8 KB of `.text` + ~1.3 KB
`.rodata` (templates) = ~9.3 KB, leaving ~10 KB of headroom under the 20 KB cap.

### 4.2 Inter-Module Contracts

Each contract is a single pure C function. No module reads or writes memory
outside its declared outputs. This is what makes the system unit-testable and
replaceable part-by-part.

```c
void threshold(const uint8_t *y, int w, int h, uint8_t *bin_out);

int  ccl_moments(const uint8_t *bin, int w, int h,
                 Blob *blobs_out, int max_blobs);

void shape_features(const Blob *blob, Features *features_out);

void classify(const Features *features, const Template *templates, int n_templates,
              uint8_t *class_id_out, uint8_t *confidence_out);

void pose(const Blob *blob,
          int16_t *x_out, int16_t *y_out, int16_t *theta_x10_out);
```

## 5. Per-Frame Data Flow

```
  Y buffer (640×480, uint8)
     │
     ▼  threshold  — single pass, 8 px per int32 op
  bitmap (38.4 KB)
     │
     ▼  ccl_moments  — two-pass Rosenfeld-Pfaltz + union-find;
     │                 accumulates m00, m10, m01, μ20, μ11, μ02,
     │                 μ30, μ21, μ12, μ03, perimeter (4-neighbour),
     │                 and bbox per label
  Blob[N]
     │
     ▼  size filter:  Amin ≤ m00 ≤ Amax (both compile-time constants)
  valid Blob[K]
     │
     ▼  shape_features  — log|Hu[0..6]|, perim/√area, eccentricity,
     │                    μ3-along-principal-axis sign
  Features[K]
     │
     ▼  classify  — **squared** Mahalanobis distance against K templates;
     │              REJECT   if min_dist²             > reject_thresh
     │              AMBIGUOUS if (dist²₂ − dist²₁)     < margin
  (class_id, confidence)[K]
     │
     ▼  pose  (accepted blobs only)
  Detection[K]
     │
     ▼  argmax by confidence
  single Detection → platform_glue → robot controller
```

## 6. Data Structures

All fixed-size; all `.bss`.

```c
typedef struct {
    int32_t m00, m10, m01;                    // raw moments 0–1 (int32 safe for Amax ≤ 50000 px)
    int64_t mu20, mu11, mu02;                 // central 2nd  (int64 — see P2-2 rationale in §7)
    int64_t mu30, mu21, mu12, mu03;           // central 3rd  (int64 — can exceed 1e10)
    int32_t perimeter;                        // 4-neighbour boundary-pixel count, accumulated in CCL pass 2
    int16_t bbox_x0, bbox_y0, bbox_x1, bbox_y1;
} Blob;                                       // 12 + 24 + 32 + 4 + 8 = 80 B

#define N_FEAT 10
typedef struct {
    int32_t hu[7];         // log|Hu_k|, Q16.16, signed
    int32_t perim_ratio;   // perimeter / sqrt(area), Q16.16
    int32_t eccentricity;  // Q16.16
    int32_t m3_axis_sign;  // +1 or -1 (stored as int32 for alignment)
} Features;                // 40 B

typedef struct {
    Features mean;
    int32_t  L_inv[N_FEAT*(N_FEAT+1)/2];  // inverse Cholesky lower-tri, Q16.16
    int32_t  reject_thresh;               // **squared** Mahalanobis distance, Q16.16
} Template;                               // 40 + 55*4 + 4 = 264 B, ×5 = 1.3 KB
// All distances in this spec are squared Mahalanobis unless explicitly noted.
// Runtime never computes sqrt — comparisons happen in squared space.

typedef struct {
    int16_t x, y;            // centroid, pixel units
    int16_t theta_x10;       // θ×10, range −1800..1799
    uint8_t class_id;        // 0..4 valid; 0xFE = AMBIGUOUS; 0xFF = REJECTED
    uint8_t confidence_q8;   // 0..255, higher is better
} Detection;                 // 8 B

// .bss working buffers, sized for 640×480
//
// Two independent caps:
//   MAX_LABELS — worst-case raw label count from CCL pass 1 (noise-driven;
//                matches uint16 label space).
//   MAX_BLOBS  — post-union unique blobs actually surfaced to higher layers
//                (expected ≤ ~30; 256 provides an ample safety margin).
// Either cap saturating triggers TPV_SCENE_ERROR, guaranteed before any
// buffer overrun.
#define MAX_LABELS 65535
#define MAX_BLOBS  256

static uint8_t  g_bin[640*480/8];             //  38.4 KB
static uint16_t g_labels[640*480];            // 614.4 KB
static Blob     g_blobs[MAX_BLOBS];           //  20.0 KB  (256 × 80 B)
static uint32_t g_uf_parent[MAX_LABELS + 1];  // 262.1 KB  (65536 × 4 B)
// Total working set ≈ 935 KB, well under the 512 MB RAM budget.
```

## 7. Key Algorithm Decisions

| Area | Decision | Rationale |
|---|---|---|
| Numeric type | `int32` fixed-point (Q16.16) for features, templates, and distances; `int64` for 2nd/3rd moment accumulators only (see "Moment bit-widths" row) | Deterministic, no FPU dependency, bit-reproducible across builds |
| CCL | Two-pass Rosenfeld-Pfaltz with union-find + path compression | Smallest code that is provably correct; worst-case behavior analyzable |
| Hu moment storage | `sign(h) * log(|h|+ε)` compressed to Q16.16 | Raw Hu moments span 10+ orders of magnitude; log compression keeps Mahalanobis numerically well-conditioned |
| Distance | **Squared** Mahalanobis; inverse Cholesky `L⁻¹` baked in per class; distance = ‖L⁻¹(x−μ)‖² | Per-class, per-dimension scaling automatic; squared form avoids any sqrt and is the natural χ²-like statistic for rejection thresholds |
| Perimeter | Accumulated in CCL pass 2: a foreground pixel contributes +1 if any 4-neighbour is background | One extra 4-way compare per pixel; no second image pass |
| Moment bit-widths | 2nd and 3rd central moments stored as `int64`; raw moments as `int32` | At 640×480 with a 307200-px blob, μ₂₀ ≈ 1e10; at Amax = 50000 px, |μ₃₀| can reach ~4e10. `int32` would silently wrap |
| 180° disambiguation | Sign of μ₃ projected onto the principal axis | Symmetric objects degenerate to zero → no disambiguation needed, which is correct |
| Threshold | Static, set once by calibration tool | Fixed lighting removes need for Otsu/adaptive; saves code |
| Rotation handling | Invariants only — no rotated templates stored | Keeps template table tiny and match cost O(K) |

## 8. Calibration (Offline, PC)

A separate PC tool, built from the **same C source** for the feature extractor,
produces `model_data.c`. Steps:

1. Operator places each object class 30–50 times at varied angles and captures
   frames via the same camera.
2. Tool runs `threshold → ccl_moments → shape_features` on each frame.
3. Per class, compute mean feature vector μ_c and covariance Σ_c.
4. Compute Cholesky factor L such that Σ_c = L Lᵀ, store L⁻¹ (lower-tri, 55 floats
   per class, converted to Q16.16).
5. `reject_thresh` = (max intra-class **squared** Mahalanobis distance observed
   across all training samples of that class) × safety_factor (default 1.5).
   Units: squared-distance (Q16.16). No sqrt is ever applied.
6. Separability check: for every pair of classes (c_i, c_j) compute the
   **squared** Mahalanobis distance of μ_j under c_i's metric (and vice
   versa). If `min(distance²) < 2 × max(reject_thresh_i, reject_thresh_j)`
   for any pair, the tool **fails loudly** ("classes not separable with
   current features; add/change features or change product mix"). This is
   the gatekeeper that prevents silently deploying a model that cannot meet
   the reject discipline. All comparisons are in squared-distance space.
7. Emit `model_data.c` as a single `const Template templates[N_CLASSES]`.

The tool and the runtime share the feature extractor so calibration-time and
runtime features are guaranteed identical.

## 9. Error Handling and 6σ Rejection Strategy

### 9.1 Rejection Ladder

| Layer | Check | Output |
|---|---|---|
| L1 pre | No blob survives geometric filter | `EMPTY` |
| L1 pre | Raw label count ≥ MAX_LABELS during CCL pass 1 | `SCENE_ERROR` |
| L1 pre | Unique blob count ≥ MAX_BLOBS after union (hard cap 256, expected max ~30) | `SCENE_ERROR` |
| L2 geom | Blob area ∉ [Amin, Amax] | Drop blob silently |
| L3 class | min **squared** Mahalanobis distance > reject_thresh | `REJECTED (0xFF)` |
| L3' class | (dist²₂ − dist²₁) < margin (margin in squared-distance units) | `AMBIGUOUS (0xFE)` |

### 9.2 Determinism Guarantees

- No floating-point arithmetic.
- No time, random, or thread-local state.
- All buffers zeroed on entry to `process_frame`.
- CCL label assignment order is purely a function of scan order, hence of input.

These guarantees let the team run **bit-identical replay** on any recorded frame,
which is essential for incident analysis and regression testing.

### 9.3 Traceability

Compile-time `DEBUG_TRACE` macro enables structured logs:

- Each detected blob: `{blob_id, bbox, features, top-3 distances, decision}`.
- Stripped from release builds — zero runtime cost.

### 9.4 Why the Algorithm Does Not Aim for 6σ on Its Own

Achieving 3.4 DPMO from a single-shot vision call is unrealistic and, worse,
unmeasurable (you cannot reliably validate 3.4 DPMO from a feasible sample
size). The design explicitly leaves the remaining sigmas to:

- **Reject-rather-than-guess** at the algorithm layer (keeps false-accepts low).
- **Post-pick verification** at the robot layer (force sensing, gripper
  encoder position, optional re-imaging, weight station).
- **Operator escalation** when the rejection rate exceeds a running threshold.

This follows standard practice on industrial lines and is the only
architecturally honest way to hit 6σ.

## 10. Interfaces

### 10.1 Runtime Entry Point

```c
// Return codes:
//   TPV_OK           (0)  → det_out populated; class_id carries the decision
//                            (0..4 valid, 0xFE AMBIGUOUS, 0xFF REJECTED).
//   TPV_EMPTY        (1)  → no blob passed the geometric filter; det_out zeroed.
//   TPV_SCENE_ERROR  (2)  → CCL exceeded MAX_LABELS (pass-1 raw-label overflow)
//                            OR post-union blob count exceeded MAX_BLOBS;
//                            det_out zeroed.
//   TPV_BAD_INPUT    (-1) → w/h mismatch with compile-time WxH or null pointer.
int tpv_process_frame(const uint8_t *y, int w, int h, Detection *det_out);
```

`EMPTY` and `SCENE_ERROR` are intentionally *not* encoded inside `Detection.class_id`
so that scene-level faults cannot be mistaken for per-object rejections; they are
first-class return codes that the controller must handle explicitly.

### 10.2 Output Payload (9 bytes, identical for all transports)

```
offset  bytes  field
  0      1    status     0=OK, 1=EMPTY, 2=SCENE_ERROR, 3=BAD_INPUT
  1      2    x          little-endian int16, pixels   (valid iff status==OK)
  3      2    y          little-endian int16, pixels   (valid iff status==OK)
  5      2    theta_x10  little-endian int16, deg × 10 (valid iff status==OK)
  7      1    class_id   0..4 normal; 0xFE AMBIGUOUS; 0xFF REJECTED (valid iff status==OK)
  8      1    confidence 0..255                         (valid iff status==OK)
```

The leading `status` byte mirrors the `tpv_process_frame` return code, so the
receiving controller can distinguish **"no pickable object in scene"**
(`status=EMPTY`) from **"vision subsystem silent"** (no frame received at all,
visible only at the transport layer via timeout). `class_id` is deliberately
reserved for *per-object* decisions only; scene-level faults never leak into
it.

When `status != OK`, offsets 1..8 are zero-filled and must be ignored by the
receiver.

Transport is platform-glue's responsibility. Serial and TCP/JSON wrappers are
both trivial and can be compiled in or out with a config flag; they are
expected to add their own framing (e.g., STX/length/CRC) around this 9-byte
logical payload as appropriate for the physical link.

### 10.3 Calibration Tool I/O

```
Input:  N_CLASSES * M frames of raw Y @ 640×480
Output: model_data.c containing `const Template templates[N_CLASSES]`
        report.txt with per-class separability metrics
```

## 11. Testing Strategy

| Layer | Method | Pass Criterion |
|---|---|---|
| Unit | Per-module golden-data tests on PC | 100% branch coverage |
| Property | Rotate/translate input; verify (x,y,θ) transform correctly | <1 px / <0.5° error |
| Synthetic | Procedurally generated templates with added Gaussian + salt-pepper noise | Classify rate >99.9% at expected SNR |
| Regression | 10k recorded production frames replayed | Zero decision change vs. golden baseline per release |
| Target | Cross-compile to ARM; time on real board | ≤30 ms/frame @ 640×480, p99 |
| Long-stability | ≥100k frames from line | FAR, FRR measured; reject rate and miss rate within spec |

## 12. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Two of the ≤5 classes have indistinguishable Hu features | Low | High (blocks product) | Calibration tool's separability check (§8 step 6) refuses to ship; add perimeter/area or 3rd-moment feature |
| Background board gets scuffed/dirty over time | Medium | Medium | Periodic re-calibration; L2 area filter absorbs small noise; add operator-visible reject-rate metric |
| Camera replaced with different optics | Low | High | Working resolution and overhead pose are fixed; recalibrate on any hardware change |
| 614.4 KB `g_labels` buffer is too large for some boards | Low | Medium | If needed, reduce working resolution to 320×240 (4× smaller) with no algorithm change |
| Production introduces a 6th class | — | — | Out of scope by A3; requires firmware rebuild |

## 13. Open Questions

Before implementation, the following from §3.3 require user confirmation or refinement:

1. **A1 (6σ strategy)**: confirm that post-pick verification exists on the robot side.
2. **A2 (latency budget)**: is 30 ms the right target, or is the cycle budget
   tighter / looser?
3. **A4 (output transport)**: pick one — serial binary, or TCP/JSON, or both.
4. **Object count upper bound**: expected per-frame max is ~30. `MAX_BLOBS`
   is set to 256 as a defensive hard cap after CCL union; `MAX_LABELS` is
   65535 for raw labels during CCL pass 1 (noise tolerance). Confirm 256
   unique-blobs is comfortable (scene never legitimately reaches it);
   otherwise lower it so overflow triggers `SCENE_ERROR` earlier.
5. **Calibration UX**: does the PC tool need a GUI for operators, or is a CLI
   plus existing capture tooling sufficient?

## 14. Transition to Implementation

Once this spec is approved, the next step is a detailed **implementation plan**
produced by the writing-plans skill. That plan will decompose each module into
concrete tasks with test checkpoints, in an order that keeps every intermediate
commit runnable and verifiable.
