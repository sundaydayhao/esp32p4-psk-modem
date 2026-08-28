#pragma once

#include <stddef.h>
#include <stdint.h>

#include "psk_modes.h"
#include "psk_rtty.h"

#define PSK1200F_FRAME_HEADER_CAPACITY 64U
#define PSK1200F_FRAME_BASE64_CAPACITY 4096U
#define PSK1200F_FRAME_PAYLOAD_CAPACITY 3072U

typedef enum {
    PSK1200F_FRAME_SEARCH = 0,
    PSK1200F_FRAME_HEADER,
    PSK1200F_FRAME_DATA,
    PSK1200F_FRAME_RESYNC,
} psk1200f_frame_state_t;

typedef struct {
    psk1200f_frame_state_t state;
    char header[PSK1200F_FRAME_HEADER_CAPACITY];
    char base64[PSK1200F_FRAME_BASE64_CAPACITY];
    uint16_t header_len;
    uint16_t base64_len;
    uint16_t delivery_len;
    uint16_t delivery_pos;
    uint16_t expected_len;
    uint32_t expected_crc;
    uint32_t sequence;
    uint32_t next_sequence;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint32_t sync_events;
    uint8_t sequence_started;
} psk1200f_frame_t;

typedef struct {
    psk_mode_t mode;
    int sample_rate_hz;
    int rx_tone_hz;
    int squelch;
    int afc_enabled;
    int squelch_enabled;
} psk_rx_config_t;

typedef struct {
    psk_rx_config_t config;
    psk_rtty_rx_t rx;
    char *out;
    size_t out_size;
    size_t out_len;
    uint8_t backend_active;
    int fec_branch_override;
    psk1200f_frame_t frame1200f;
} psk_rx_t;

typedef struct {
    uint32_t symbols;
    uint32_t bits;
    uint32_t ones;
    uint32_t zeros;
    uint32_t delims;
    uint32_t chars;
    uint32_t unknown;
    int sq_open;
    int sq_level;
    int sq_thresh;
    int afc_enabled;
    int squelch_enabled;
    int stable_symbols;
    int warmup_symbols;
    int tone_hz;
    int selected_tone_hz;
    int fec_metric[2];
    uint8_t fec_selected;
    uint8_t lock_state;
    int timing_bins;
    int timing_peak;
    int timing_target;
    int carrier_offset_millihz;
    uint32_t timing_adjusts;
    uint32_t soft_ambiguous;
    uint32_t coarse_updates;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint32_t frame_sync_events;
    uint32_t fec_pairs[2];
    uint32_t fec_pair_count;
    uint8_t fec_vote_candidate;
    uint8_t fec_vote_count;
    uint8_t fec_output_enabled;
    uint8_t fec_locked_branch;
} psk_rx_debug_t;

void psk_rx_init(psk_rx_t *m, const psk_rx_config_t *config);
void psk_rx_reset(psk_rx_t *m);
void psk_rx_set_sample_rate(psk_rx_t *m, int sample_rate_hz);
void psk_rx_set_mode(psk_rx_t *m, psk_mode_t mode);
void psk_rx_set_tone(psk_rx_t *m, int tone_hz);
void psk_rx_set_afc(psk_rx_t *m, int enabled);
void psk_rx_set_squelch_enabled(psk_rx_t *m, int enabled);
void psk_rx_set_fec_branch_override(psk_rx_t *m, int branch);
psk_mode_t psk_rx_mode(const psk_rx_t *m);
int psk_rx_tone_hz(const psk_rx_t *m);
const char *psk_rx_mode_name(psk_mode_t mode);
const psk_mode_info_t *psk_rx_mode_info(const psk_rx_t *m);
int psk_rx_mode_supported(psk_mode_t mode);
void psk_rx_debug_snapshot(const psk_rx_t *m, psk_rx_debug_t *debug);
size_t psk_rx_process(psk_rx_t *m, const int16_t *samples, size_t count,
                         char *out, size_t out_size);
