#pragma once

typedef enum {
    MODEM_MODE_BPSK31 = 0,
    MODEM_MODE_PSK63,
    MODEM_MODE_FT8,
    MODEM_MODE_FT4,
    MODEM_MODE_RTTY,
    MODEM_MODE_CW,
    MODEM_MODE_MFSK16,
    MODEM_MODE_COUNT
} modem_mode_t;
