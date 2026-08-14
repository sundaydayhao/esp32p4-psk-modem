#pragma once

#include <stddef.h>
#include <stdint.h>

#include "psk_rtty.h"

typedef enum {
    PSK_MODE_FAMILY_BPSK = 0,
    PSK_MODE_FAMILY_QPSK,
    PSK_MODE_FAMILY_8PSK,
} psk_mode_family_t;

typedef enum {
    PSK_MODE_BPSK31 = 0,
    PSK_MODE_BPSK63,
    PSK_MODE_BPSK63F,
    PSK_MODE_BPSK125,
    PSK_MODE_BPSK250,
    PSK_MODE_BPSK500,
    PSK_MODE_BPSK1000,
    PSK_MODE_12XPSK125,
    PSK_MODE_6XPSK250,
    PSK_MODE_2XPSK500,
    PSK_MODE_4XPSK500,
    PSK_MODE_2XPSK800,
    PSK_MODE_2XPSK1000,
    PSK_MODE_QPSK31,
    PSK_MODE_QPSK63,
    PSK_MODE_QPSK125,
    PSK_MODE_QPSK250,
    PSK_MODE_QPSK500,
    PSK_MODE_8PSK125,
    PSK_MODE_8PSK250,
    PSK_MODE_8PSK500,
    PSK_MODE_8PSK1000,
    PSK_MODE_8PSK125F,
    PSK_MODE_8PSK250F,
    PSK_MODE_8PSK500F,
    PSK_MODE_8PSK1000F,
    PSK_MODE_8PSK1200F,
    PSK_MODE_COUNT
} psk_mode_t;

typedef struct {
    psk_mode_t id;
    const char *name;
    psk_mode_family_t family;
    float baud;
    uint8_t bits_per_symbol;
    uint8_t carriers;
    uint16_t dcd_bits;
    uint8_t fec;
    uint8_t punctured;
    uint8_t decode_supported;
    psk_rtty_mode_t backend_mode;
} psk_mode_info_t;

const psk_mode_info_t *psk_mode_info(psk_mode_t mode);
const psk_mode_info_t *psk_mode_info_at(size_t index);
size_t psk_mode_count(void);
const char *psk_mode_family_name(psk_mode_family_t family);
const char *psk_mode_name(psk_mode_t mode);
int psk_mode_can_decode(psk_mode_t mode);
