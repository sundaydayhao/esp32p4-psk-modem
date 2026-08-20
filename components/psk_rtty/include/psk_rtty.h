#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PSK_RTTY_MODE_BPSK31 = 0,
    PSK_RTTY_MODE_QPSK31_USB,
    PSK_RTTY_MODE_QPSK31_LSB,
    PSK_RTTY_MODE_RTTY,
    PSK_RTTY_MODE_PSK63,
    PSK_RTTY_MODE_BPSK125,
    PSK_RTTY_MODE_BPSK250,
    PSK_RTTY_MODE_BPSK500,
    PSK_RTTY_MODE_BPSK1000,
    PSK_RTTY_MODE_BPSK63F,
    PSK_RTTY_MODE_12XPSK125,
    PSK_RTTY_MODE_6XPSK250,
    PSK_RTTY_MODE_2XPSK500,
    PSK_RTTY_MODE_4XPSK500,
    PSK_RTTY_MODE_2XPSK800,
    PSK_RTTY_MODE_2XPSK1000,
    PSK_RTTY_MODE_QPSK63,
    PSK_RTTY_MODE_QPSK125,
    PSK_RTTY_MODE_QPSK250,
    PSK_RTTY_MODE_QPSK500,
    PSK_RTTY_MODE_8PSK125,
    PSK_RTTY_MODE_8PSK250,
    PSK_RTTY_MODE_8PSK500,
    PSK_RTTY_MODE_8PSK1000,
    PSK_RTTY_MODE_8PSK125F,
    PSK_RTTY_MODE_8PSK250F,
    PSK_RTTY_MODE_8PSK500F,
    PSK_RTTY_MODE_8PSK1000F,
    PSK_RTTY_MODE_8PSK1200F,
    PSK_RTTY_MODE_COUNT
} psk_rtty_mode_t;

typedef void (*psk_rtty_char_cb_t)(char c, void *userdata);

typedef enum {
    PSK_RTTY_LOCK_ACQUIRE = 0,
    PSK_RTTY_LOCK_SIGNAL,
    PSK_RTTY_LOCK_FEC,
    PSK_RTTY_LOCK_READY,
} psk_rtty_lock_state_t;

typedef struct {
    psk_rtty_mode_t mode;
    int sample_rate_hz;
    int rx_tone_hz;
    psk_rtty_char_cb_t char_cb;
    void *userdata;
} psk_rtty_config_t;

typedef struct {
    float x;
    float y;
} psk_rtty_complex_t;

typedef struct {
    psk_rtty_config_t config;
    float nco_phase;
    float nco_phase_inc;
    float nco_cos;
    float nco_sin;
    float freq_error;
    float afc_limit;
    float afc_min;
    float afc_max;
    float fferr_ave;
    float fperr_ave;
    float q_freq_error;
    float nlp_k;
    float agc_ave;
    float dev_ave;
    float bpsk_average_amp;
    float psk8_last_phase_quality;
    float bit_phase_inc;
    float bit_phase_pos;
    int second_decim;
    float prev_symbol_phase;
    float symbol_clock;
    float symbol_period_samples;
    float i_acc;
    float q_acc;
    float sync_ave[21];
    float i0;
    float i1;
    float q0;
    float q1;
    psk_rtty_complex_t chan_dec1_queue[12][35];
    psk_rtty_complex_t chan_dec2_queue[12][35];
    psk_rtty_complex_t chan_bit_queue[12][65];
    psk_rtty_complex_t chan_freq_signal[12];
    psk_rtty_complex_t chan_bit_signal[12];
    int chan_dec1_state[12];
    int chan_dec2_state[12];
    int chan_bit_fir_state[12];
    float chan_mix_i_acc[12];
    float chan_mix_q_acc[12];
    int chan_mix_decim_count;
    psk_rtty_complex_t z1;
    psk_rtty_complex_t z2;
    float carrier_phase_inc[12];
    float carrier_rot_cos[12];
    float carrier_rot_sin[12];
    float carrier_cos[12];
    float carrier_sin[12];
    float carrier_i_acc[12];
    float carrier_q_acc[12];
    float carrier_i_prev[12];
    float carrier_q_prev[12];
    uint8_t carrier_has_prev[12];
    uint8_t carriers;
    uint8_t bits_per_symbol;
    uint8_t psk_family;
    uint8_t decision_directed_afc;
    uint8_t fec_enabled;
    uint8_t fec_punctured;
    uint8_t use_mfsk_varicode;
    uint32_t fec_pair_count;
    uint8_t fec_dual_branch;
    uint8_t fec_locked_branch;
    uint8_t fec_vote_candidate;
    uint8_t fec_vote_count;
    uint8_t fec_rxbitstate;
    uint8_t fec_bit_phase;
    uint8_t fec_last_encoded_bit;
    uint8_t fec_have_last_encoded_bit;
    uint8_t fec_pending_bits[2];
    uint8_t fec_pair_buf[2];
    uint8_t fec_pair_len;
    uint32_t fec_stream_bit_count;
    int fec_phase_metric[2];
    uint8_t fec_phase_search;
    uint8_t fec_phase_candidate;
    uint8_t fec_phase_align_pending;
    uint8_t fec_output_enabled;
    uint8_t fec_k;
    uint8_t vit_traceback;
    uint8_t vit_chunksize;
    uint8_t fec_settle_tracebacks;
    uint8_t fec_settle_half_interleaves;
    uint16_t fec_state;
    uint32_t fec_mask;
    uint32_t fec_poly1;
    uint32_t fec_poly2;
    uint16_t inlv_depth;
    uint16_t inlv_len;
    int samples_in_symbol;
    int sample_count;
    int afc_timer;
    int afc_enabled;
    int pk_pos;
    int new_pk_pos;
    int raw_pk_pos;
    int bit_pos;
    int warmup_symbols;
    int last_pk_pos;
    int sq_open;
    int sq_was_open;
    int sq_stable_symbols;
    int sq_level;
    int sq_thresh;
    int squelch_enabled;
    int squelch_speed;
    uint8_t lock_state;
    int on_count;
    int off_count;
    int p_count;
    int n_count;
    uint16_t bit_acc;
    int last_bit_zero;
    int rtty_figures;
    int rtty_bit_index;
    uint8_t rtty_shift_reg;
    uint32_t debug_bits;
    uint32_t debug_symbols;
    uint32_t debug_ones;
    uint32_t debug_zeros;
    uint32_t debug_delims;
    uint32_t debug_chars;
    uint32_t debug_unknown;
    int debug_fec_metric[2];
    uint8_t debug_fec_selected;
} psk_rtty_rx_t;

void psk_rtty_rx_init(psk_rtty_rx_t *rx, const psk_rtty_config_t *config);
void psk_rtty_rx_set_mode(psk_rtty_rx_t *rx, psk_rtty_mode_t mode);
void psk_rtty_rx_set_tone(psk_rtty_rx_t *rx, int tone_hz);
void psk_rtty_rx_feed(psk_rtty_rx_t *rx, const int16_t *samples, size_t count);
