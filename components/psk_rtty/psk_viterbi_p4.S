/* Exact signed-16 ACS for complementary convolutional polynomials.
 * All metric pointers and the four 8-lane branch-metric vectors are 16-byte
 * aligned. q0..q7 are caller-saved by the ESP32-P4 ABI / FreeRTOS port.
 * Equal metrics select predecessor 1, matching the portable decoder.
 */
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4
    .section .iram1,"ax"
    .global psk_viterbi_p4_acs
    .type psk_viterbi_p4_acs, @function
    .balign 4
    .option push
    .option norvc

/* a0/a1: lower/upper predecessor halves; a2: output; a3: packed decisions;
 * a4: group classes; a5: branch metrics [4][8]; a6: nstates/16;
 * a7: decision weights {1,2,4,8,16,32,64,128}. */
    .macro acs_eight lo, hi
    lbu t3, 0(a4)
    addi a4, a4, 1
    slli t3, t3, 4
    add t3, a5, t3
    esp.vld.128.ip q4, t3, 0
    esp.vadd.s16 q5, \lo, q4
    esp.vsub.s16 q6, \hi, q4
    esp.vcmp.gt.s16 q4, q5, q6
    esp.vmax.s16 q5, q5, q6
    esp.vst.128.ip q5, a2, 16
    esp.andq q4, q4, q7
    esp.movi.32.a q4, t3, 0
    esp.movi.32.a q4, t4, 1
    esp.movi.32.a q4, t5, 2
    esp.movi.32.a q4, t6, 3
    or t3, t3, t4
    or t5, t5, t6
    or t3, t3, t5
    srli t4, t3, 16
    or t3, t3, t4
    xori t3, t3, 255
    sb t3, 0(a3)
    addi a3, a3, 1
    .endm

psk_viterbi_p4_acs:
    mv t3, a7
    esp.vld.128.ip q7, t3, 0
1:
    esp.vldhbc.16.incp q0, q1, a0
    esp.vldhbc.16.incp q2, q3, a1
    acs_eight q0, q2
    acs_eight q1, q3
    addi a6, a6, -1
    bnez a6, 1b
    ret
    .size psk_viterbi_p4_acs, .-psk_viterbi_p4_acs
    .option pop
#endif
