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
