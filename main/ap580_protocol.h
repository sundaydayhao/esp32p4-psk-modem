#pragma once

#include <stdint.h>

/* AP580's 0.6K and 2.4K modes both use seven voice bytes per UART frame. */
#define AP580_CODEC_RATE_AUTO        0U
#define AP580_CODEC_RATE_600         600U
#define AP580_CODEC_RATE_1200        1200U
#define AP580_CODEC_RATE_2400        2400U

/* The jumper cannot be read by the HPM6750. Match the current 0.6K setup. */
#define AP580_CODEC_RATE_DEFAULT     AP580_CODEC_RATE_600

static inline uint32_t ap580_frame_period_half_ms(uint32_t rate)
{
    /* The AP580 codec profile changes its UART frame cadence: measured
     * periods are 90 ms (0.6K), 67.5 ms (1.2K), and 22.5 ms (2.4K). Store
     * twice the period so the caller can alternate integer millisecond
     * deadlines without losing the half millisecond. */
    switch (rate) {
    case AP580_CODEC_RATE_600:
        return 180U;
    case AP580_CODEC_RATE_1200:
        return 135U;
    default:
        return 45U;
    }
}
