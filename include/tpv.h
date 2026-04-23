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

/* spec §10.1 entry contract */
int tpv_process_frame(const uint8_t *y, int w, int h, tpv_Detection *det_out);

/* spec §10.2 wire payload (9 bytes). status takes the in-process int return code
 * (incl. -1 = TPV_BAD_INPUT) and is mapped internally to wire byte 0/1/2/3. */
void tpv_serialize_payload(int status, const tpv_Detection *d, uint8_t *buf9);

#endif
