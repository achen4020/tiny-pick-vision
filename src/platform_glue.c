#include <string.h>
#include "tpv.h"
#include "tpv_internal.h"

/* In-process return code → spec §10.2 wire byte. */
static uint8_t status_to_wire(int rc) {
    switch (rc) {
        case TPV_OK:           return 0;
        case TPV_EMPTY:        return 1;
        case TPV_SCENE_ERROR:  return 2;
        case TPV_BAD_INPUT:    return 3;
        default:               return 0xFF;   /* reserved: keep distinct from 0..3 */
    }
}

/* 9-byte little-endian wire payload (spec §10.2). When status != OK, the
 * detection fields (offsets 1..8) are zero-filled and the receiver must ignore
 * them. The leading byte is the only thing that distinguishes "no pickable
 * object" (EMPTY) from "vision subsystem silent" (no frame at all). */
void tpv_serialize_payload(int status, const tpv_Detection *d, uint8_t *buf9) {
    buf9[0] = status_to_wire(status);
    if (status != TPV_OK) { memset(buf9 + 1, 0, 8); return; }
    buf9[1] = (uint8_t)( d->x          & 0xFF);
    buf9[2] = (uint8_t)((d->x >> 8)    & 0xFF);
    buf9[3] = (uint8_t)( d->y          & 0xFF);
    buf9[4] = (uint8_t)((d->y >> 8)    & 0xFF);
    buf9[5] = (uint8_t)( d->theta_x10  & 0xFF);
    buf9[6] = (uint8_t)((d->theta_x10 >> 8) & 0xFF);
    buf9[7] = d->class_id;
    buf9[8] = d->confidence_q8;
}

/* Real camera I/O and transport (UART/TCP framing) plug in here. Not
 * implemented in T7 — the runtime API is decoupled from transport. */
