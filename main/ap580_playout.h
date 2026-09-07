#pragma once

#include <stdint.h>
#include "ap580_protocol.h"

#define AP580_PLAYOUT_FRAME_BYTES 16U
#define AP580_PLAYOUT_MAX_REPEATS 2U

typedef struct {
    uint8_t last_frame[AP580_PLAYOUT_FRAME_BYTES];
    uint32_t rate;
    uint32_t next_tick;
    uint8_t half_ms_phase;
    uint8_t repeats;
    uint8_t active;
} ap580_playout_t;

typedef enum {
    AP580_PLAYOUT_NONE = 0,
    AP580_PLAYOUT_VOICE,
    AP580_PLAYOUT_STOP,
} ap580_playout_result_t;

void ap580_playout_reset(ap580_playout_t *playout, uint32_t rate, uint32_t now);
int ap580_playout_due(const ap580_playout_t *playout, uint32_t now);
/* Only supply a dequeued frame when due; NULL requests bounded gap concealment. */
ap580_playout_result_t ap580_playout_next(ap580_playout_t *playout,
                                        const uint8_t *fresh_frame, uint32_t now,
                                        uint8_t *out);
