#include "tpv.h"
#include "testlib.h"

/* The wire status byte is independent from the in-process return code:
 * status_to_wire() maps OK/EMPTY/SCENE_ERROR/BAD_INPUT (-1) → 0/1/2/3. */
TEST(t_wire_payload_ok) {
    tpv_Detection d = { .x = 0x0102, .y = 0x0304, .theta_x10 = 0x0506,
                        .class_id = 7, .confidence_q8 = 200 };
    uint8_t buf[9];
    tpv_serialize_payload(TPV_OK, &d, buf);
    CHECK_EQ_I(buf[0], 0);             /* TPV_OK → wire 0 */
    CHECK_EQ_I(buf[1], 0x02); CHECK_EQ_I(buf[2], 0x01);
    CHECK_EQ_I(buf[3], 0x04); CHECK_EQ_I(buf[4], 0x03);
    CHECK_EQ_I(buf[5], 0x06); CHECK_EQ_I(buf[6], 0x05);
    CHECK_EQ_I(buf[7], 7);    CHECK_EQ_I(buf[8], 200);
}

TEST(t_wire_payload_empty) {
    tpv_Detection d = { .x = 1, .y = 2, .theta_x10 = 3, .class_id = 4, .confidence_q8 = 5 };
    uint8_t buf[9];
    tpv_serialize_payload(TPV_EMPTY, &d, buf);
    CHECK_EQ_I(buf[0], 1);             /* TPV_EMPTY → wire 1 */
    for (int i = 1; i < 9; i++) CHECK_EQ_I(buf[i], 0);
}

TEST(t_wire_payload_bad_input_maps_to_3) {
    /* Critical mapping: in-process TPV_BAD_INPUT = -1, wire byte = 3.
     * Without status_to_wire, casting -1 to uint8_t would emit 0xFF. */
    tpv_Detection d = {0};
    uint8_t buf[9] = {0xAA};
    tpv_serialize_payload(TPV_BAD_INPUT, &d, buf);
    CHECK_EQ_I(buf[0], 3);
    for (int i = 1; i < 9; i++) CHECK_EQ_I(buf[i], 0);
}

int main(void) {
    RUN(t_wire_payload_ok);
    RUN(t_wire_payload_empty);
    RUN(t_wire_payload_bad_input_maps_to_3);
    FINISH();
}
