#include "ap580_playout.h"

#include <string.h>

void ap580_playout_reset(ap580_playout_t *playout, uint32_t rate, uint32_t now)
{
    memset(playout, 0, sizeof(*playout));
    playout->rate = rate;
    playout->next_tick = now;
}

int ap580_playout_due(const ap580_playout_t *playout, uint32_t now)
{
    return (int32_t)(now - playout->next_tick) >= 0;
}

ap580_playout_result_t ap580_playout_next(ap580_playout_t *playout,
                                        const uint8_t *fresh_frame, uint32_t now,
                                        uint8_t *out)
{
    if (!ap580_playout_due(playout, now)) {
        return AP580_PLAYOUT_NONE;
    }
    if (fresh_frame) {
        memcpy(playout->last_frame, fresh_frame, AP580_PLAYOUT_FRAME_BYTES);
        playout->repeats = 0U;
        playout->active = 1U;
    } else if (!playout->active) {
        return AP580_PLAYOUT_NONE;
    } else if (playout->repeats >= AP580_PLAYOUT_MAX_REPEATS) {
        playout->active = 0U;
        return AP580_PLAYOUT_STOP;
    } else {
        ++playout->repeats;
    }

    memcpy(out, playout->last_frame, AP580_PLAYOUT_FRAME_BYTES);
    const uint32_t half_ms = ap580_frame_period_half_ms(playout->rate);
    const uint32_t step = half_ms / 2U + ((half_ms & 1U) & playout->half_ms_phase);
    playout->half_ms_phase ^= 1U;
    playout->next_tick += step;
    /* A stalled consumer must not burst old codec blocks to catch up. */
    if ((int32_t)(now - playout->next_tick) >= 0) {
        playout->next_tick = now + step;
    }
    return AP580_PLAYOUT_VOICE;
}
