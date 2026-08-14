#include "psk_modes.h"

#define MODE(_id, _name, _family, _baud, _symbits, _carriers, _dcd, _fec, _punctured, _supported, _backend) \
    {                                                                                                      \
        .id = (_id),                                                                                      \
        .name = (_name),                                                                                  \
        .family = (_family),                                                                              \
        .baud = (_baud),                                                                                  \
        .bits_per_symbol = (_symbits),                                                                    \
        .carriers = (_carriers),                                                                          \
        .dcd_bits = (_dcd),                                                                               \
        .fec = (_fec),                                                                                    \
        .punctured = (_punctured),                                                                        \
        .decode_supported = (_supported),                                                                 \
        .backend_mode = (_backend),                                                                       \
    }

static const psk_mode_info_t s_modes[PSK_MODE_COUNT] = {
    MODE(PSK_MODE_BPSK31, "BPSK31", PSK_MODE_FAMILY_BPSK, 31.25f, 1, 1, 32, 0, 0, 1, PSK_RTTY_MODE_BPSK31),
    MODE(PSK_MODE_BPSK63, "BPSK63", PSK_MODE_FAMILY_BPSK, 62.5f, 1, 1, 64, 0, 0, 1, PSK_RTTY_MODE_PSK63),
    MODE(PSK_MODE_BPSK63F, "BPSK63F", PSK_MODE_FAMILY_BPSK, 62.5f, 1, 1, 64, 1, 0, 1, PSK_RTTY_MODE_BPSK63F),
    MODE(PSK_MODE_BPSK125, "BPSK125", PSK_MODE_FAMILY_BPSK, 125.0f, 1, 1, 128, 0, 0, 1, PSK_RTTY_MODE_BPSK125),
    MODE(PSK_MODE_BPSK250, "BPSK250", PSK_MODE_FAMILY_BPSK, 250.0f, 1, 1, 256, 0, 0, 1, PSK_RTTY_MODE_BPSK250),
    MODE(PSK_MODE_BPSK500, "BPSK500", PSK_MODE_FAMILY_BPSK, 500.0f, 1, 1, 512, 0, 0, 1, PSK_RTTY_MODE_BPSK500),
    MODE(PSK_MODE_BPSK1000, "BPSK1000", PSK_MODE_FAMILY_BPSK, 1000.0f, 1, 1, 128, 0, 0, 1, PSK_RTTY_MODE_BPSK1000),
    MODE(PSK_MODE_12XPSK125, "12xPSK125", PSK_MODE_FAMILY_BPSK, 125.0f, 1, 12, 128, 0, 0, 1, PSK_RTTY_MODE_12XPSK125),
    MODE(PSK_MODE_6XPSK250, "6xPSK250", PSK_MODE_FAMILY_BPSK, 250.0f, 1, 6, 512, 0, 0, 1, PSK_RTTY_MODE_6XPSK250),
    MODE(PSK_MODE_2XPSK500, "2xPSK500", PSK_MODE_FAMILY_BPSK, 500.0f, 1, 2, 512, 0, 0, 1, PSK_RTTY_MODE_2XPSK500),
    MODE(PSK_MODE_4XPSK500, "4xPSK500", PSK_MODE_FAMILY_BPSK, 500.0f, 1, 4, 512, 0, 0, 1, PSK_RTTY_MODE_4XPSK500),
    MODE(PSK_MODE_2XPSK800, "2xPSK800", PSK_MODE_FAMILY_BPSK, 800.0f, 1, 2, 512, 0, 0, 1, PSK_RTTY_MODE_2XPSK800),
    MODE(PSK_MODE_2XPSK1000, "2xPSK1000", PSK_MODE_FAMILY_BPSK, 1000.0f, 1, 2, 1024, 0, 0, 1, PSK_RTTY_MODE_2XPSK1000),
    MODE(PSK_MODE_QPSK31, "QPSK31", PSK_MODE_FAMILY_QPSK, 31.25f, 2, 1, 32, 1, 0, 1, PSK_RTTY_MODE_QPSK31_USB),
    MODE(PSK_MODE_QPSK63, "QPSK63", PSK_MODE_FAMILY_QPSK, 62.5f, 2, 1, 64, 1, 0, 1, PSK_RTTY_MODE_QPSK63),
    MODE(PSK_MODE_QPSK125, "QPSK125", PSK_MODE_FAMILY_QPSK, 125.0f, 2, 1, 128, 1, 0, 1, PSK_RTTY_MODE_QPSK125),
    MODE(PSK_MODE_QPSK250, "QPSK250", PSK_MODE_FAMILY_QPSK, 250.0f, 2, 1, 256, 1, 0, 1, PSK_RTTY_MODE_QPSK250),
    MODE(PSK_MODE_QPSK500, "QPSK500", PSK_MODE_FAMILY_QPSK, 500.0f, 2, 1, 512, 1, 0, 1, PSK_RTTY_MODE_QPSK500),
    MODE(PSK_MODE_8PSK125, "8PSK125", PSK_MODE_FAMILY_8PSK, 125.0f, 3, 1, 128, 0, 0, 1, PSK_RTTY_MODE_8PSK125),
    MODE(PSK_MODE_8PSK250, "8PSK250", PSK_MODE_FAMILY_8PSK, 250.0f, 3, 1, 256, 0, 0, 1, PSK_RTTY_MODE_8PSK250),
    MODE(PSK_MODE_8PSK500, "8PSK500", PSK_MODE_FAMILY_8PSK, 500.0f, 3, 1, 512, 0, 0, 1, PSK_RTTY_MODE_8PSK500),
    MODE(PSK_MODE_8PSK1000, "8PSK1000", PSK_MODE_FAMILY_8PSK, 1000.0f, 3, 1, 1024, 0, 0, 1, PSK_RTTY_MODE_8PSK1000),
    MODE(PSK_MODE_8PSK125F, "8PSK125F", PSK_MODE_FAMILY_8PSK, 125.0f, 3, 1, 128, 1, 0, 1, PSK_RTTY_MODE_8PSK125F),
    MODE(PSK_MODE_8PSK250F, "8PSK250F", PSK_MODE_FAMILY_8PSK, 250.0f, 3, 1, 256, 1, 0, 1, PSK_RTTY_MODE_8PSK250F),
    MODE(PSK_MODE_8PSK500F, "8PSK500F", PSK_MODE_FAMILY_8PSK, 500.0f, 3, 1, 512, 1, 1, 1, PSK_RTTY_MODE_8PSK500F),
    MODE(PSK_MODE_8PSK1000F, "8PSK1000F", PSK_MODE_FAMILY_8PSK, 1000.0f, 3, 1, 1024, 1, 1, 1, PSK_RTTY_MODE_8PSK1000F),
    MODE(PSK_MODE_8PSK1200F, "8PSK1200F", PSK_MODE_FAMILY_8PSK, 16000.0f / 13.0f, 3, 1, 2048, 1, 1, 1, PSK_RTTY_MODE_8PSK1200F),
};

const psk_mode_info_t *psk_mode_info(psk_mode_t mode)
{
    if ((unsigned)mode >= PSK_MODE_COUNT) {
        return &s_modes[0];
    }
    return &s_modes[mode];
}

const psk_mode_info_t *psk_mode_info_at(size_t index)
{
    if (index >= PSK_MODE_COUNT) {
        return 0;
    }
    return &s_modes[index];
}

size_t psk_mode_count(void)
{
    return PSK_MODE_COUNT;
}

const char *psk_mode_family_name(psk_mode_family_t family)
{
    switch (family) {
    case PSK_MODE_FAMILY_QPSK:
        return "QPSK";
    case PSK_MODE_FAMILY_8PSK:
        return "8PSK";
    case PSK_MODE_FAMILY_BPSK:
    default:
        return "BPSK";
    }
}

const char *psk_mode_name(psk_mode_t mode)
{
    return psk_mode_info(mode)->name;
}

int psk_mode_can_decode(psk_mode_t mode)
{
    return psk_mode_info(mode)->decode_supported != 0U;
}
