#include "psk_rtty.h"
#include "psk_codec.h"

#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "PSKTables.h"
#include "pskcore_rx_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI 6.28318530717958647692f
#define PSKCORE_TS 0.032f
#define PSKCORE_AFC_OFF 0
#define PSKCORE_AFC_ON 1
#define PSKCORE_AFC_TIMELIMIT 10
#define PSKCORE_NLP_K 100.0f
#define PSK_TIMING_MAX_BINS 16
#define PHZ_180_BMIN 0.0f
#define PHZ_180_BMAX ((float)M_PI)
#define PHZ_0_BMIN ((float)M_PI)
#define PHZ_0_BMAX TWO_PI
#define PHZ_180_QMIN ((float)M_PI / 4.0f)
#define PHZ_180_QMAX (3.0f * (float)M_PI / 4.0f)
#define PHZ_0_QMIN (5.0f * (float)M_PI / 4.0f)
#define PHZ_0_QMAX (7.0f * (float)M_PI / 4.0f)
#define PHZ_ERR_SIGN_LIMIT 5
#define SQ_EMIT_STABLE_SYMBOLS 10
#define SQL_OPEN_HYSTERESIS 5
#define PSK_FAMILY_BPSK 0U
#define PSK_FAMILY_QPSK 1U
#define PSK_FAMILY_8PSK 2U
#define MAX_PSK_CARRIERS 12U
#define FEC_PUNCTURE 128U
#define VIT_MAX_TRACEBACK 192U
#define VIT_CHUNKSIZE 4U
#define VIT_DUAL_CHUNKSIZE 4U
#define VIT_MAX_STATES 32768U
#define FEC_BRANCH_VOTE_HYSTERESIS 64
#define FEC_BRANCH_LOCK_VOTES 16U
#define FEC_PHASE_EVAL_PAIRS 384U
#define RX_ACQUIRE_SYMBOLS 32U
#define DD_AFC_ACQUIRE_GAIN 0.06f
#define DD_AFC_TRACK_GAIN 0.001f
#define DD_AFC_TRACK_QUALITY_MARGIN 10
#define QPSK_AFC_MAX_DECISION_ERROR 0.48f
#define PSK8_AFC_MAX_DECISION_ERROR 0.40f
#define PSK8_NDA_ACQUIRE_GAIN 0.02f
#define PSK8_NDA_TRACK_GAIN 0.001f
#define PSK8_SOFT_SCALE 350.0f
#if defined(ESP_PLATFORM)
#define PSK_FAST_TEXT
#define PSK_FAST_BSS EXT_RAM_BSS_ATTR
#define PSK_SDRAM_BSS EXT_RAM_BSS_ATTR
#elif defined(__GNUC__) && defined(__riscv)
#define PSK_FAST_TEXT __attribute__((section(".fast")))
#define PSK_FAST_BSS __attribute__((section(".fast_ram.bss")))
#define PSK_SDRAM_BSS
#elif defined(__GNUC__) && !defined(_WIN32)
#define PSK_FAST_TEXT
#define PSK_FAST_BSS __attribute__((section(".dtcm_bss")))
#define PSK_SDRAM_BSS __attribute__((section(".ram_d2_bss")))
#else
#define PSK_FAST_TEXT
#define PSK_FAST_BSS
#define PSK_SDRAM_BSS
#endif

#if defined(ESP_PLATFORM)
static inline void psk_runtime_yield(uint16_t nstates, uint16_t state_base)
{
    if (nstates >= 32768U && state_base != 0U && (state_base & 0x1fffU) == 0U) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#else
static inline void psk_runtime_yield(uint16_t nstates, uint16_t state_base)
{
    (void)nstates;
    (void)state_base;
}
#endif

typedef struct {
    uint8_t (*decisions)[4096];
    int16_t *metrics_a;
    int16_t *metrics_b;
    uint16_t *sequence;
    uint8_t *inlv_table;
    uint32_t pair_count;
    uint8_t vit_step;
    uint16_t bit_acc;
    int metric_ave;
    int metric_ready;
    uint8_t rx_pair0[VIT_MAX_TRACEBACK];
    uint8_t rx_pair1[VIT_MAX_TRACEBACK];
} fec_branch_state_t;

static unsigned char s_varicode_decode[2048];
static unsigned char s_mfsk_decode[4096];
static int s_tables_ready;
static uint8_t s_vit_out_pair[VIT_MAX_STATES / 2U] PSK_FAST_BSS;
static uint8_t s_vit_decisions0[VIT_MAX_TRACEBACK][4096] PSK_SDRAM_BSS;
static uint8_t s_vit_decisions1[VIT_MAX_TRACEBACK][4096] PSK_SDRAM_BSS;
static int16_t s_vit_metrics0_a[VIT_MAX_STATES] PSK_FAST_BSS;
static int16_t s_vit_metrics0_b[VIT_MAX_STATES] PSK_FAST_BSS;
static int16_t s_vit_metrics1_a[VIT_MAX_STATES] PSK_FAST_BSS;
static int16_t s_vit_metrics1_b[VIT_MAX_STATES];
static uint16_t s_vit_sequence0[256];
static uint16_t s_vit_sequence1[256];
static uint8_t s_inlv_table0[2560];
static uint8_t s_inlv_table1[2560];
static fec_branch_state_t s_fec_branch[2] = {
    {.decisions = s_vit_decisions0, .metrics_a = s_vit_metrics0_a, .metrics_b = s_vit_metrics0_b,
     .sequence = s_vit_sequence0, .inlv_table = s_inlv_table0, .bit_acc = 1U},
    {.decisions = s_vit_decisions1, .metrics_a = s_vit_metrics1_a, .metrics_b = s_vit_metrics1_b,
     .sequence = s_vit_sequence1, .inlv_table = s_inlv_table1, .bit_acc = 1U},
};
static uint8_t s_vit_table_k;
static uint32_t s_vit_table_poly1;
static uint32_t s_vit_table_poly2;
static uint8_t s_vit_input_flip;

static float circular_distance(float a, float b);
static float channel_sync_energy(const psk_rtty_rx_t *rx);
static void finish_channel_psk_symbol(psk_rtty_rx_t *rx);
static float nearest_phase_distance(float angle, uint8_t states);
static void update_quality_squelch(psk_rtty_rx_t *rx, float dist, float step, int valid);
static void prepare_viterbi_tables(const psk_rtty_rx_t *rx, uint16_t nstates);
static void reset_fec_branches(psk_rtty_rx_t *rx);
static void reset_fec_branch(psk_rtty_rx_t *rx, uint8_t branch_index);
static void reset_fec_acquisition(psk_rtty_rx_t *rx);
static void update_fec_phase_search(psk_rtty_rx_t *rx, fec_branch_state_t *branch);
static uint32_t fec_ready_pairs_required(const psk_rtty_rx_t *rx);
static int is_rx_text_char(unsigned char ch);
static int fec_branch_selected(uint8_t branch_index);
static void update_fec_branch_lock(psk_rtty_rx_t *rx);
static int rx_signal_ready(const psk_rtty_rx_t *rx);
static int rx_fec_ready(const psk_rtty_rx_t *rx);
static void refresh_lock_state(psk_rtty_rx_t *rx);
static void update_decision_directed_afc(psk_rtty_rx_t *rx, float angle, uint8_t phase_idx,
                                         uint8_t phase_states, int valid);
static void update_8psk_nondat_afc(psk_rtty_rx_t *rx, float i, float q, int valid);

static const unsigned char s_rtty_table[0x20][2] = {
    {0, 0}, {'E', '3'}, {'\n', '\n'}, {'A', '-'}, {' ', ' '}, {'S', '\''}, {'I', '8'}, {'U', '7'},
    {'\r', '\r'}, {'D', '$'}, {'R', '4'}, {'J', 7}, {'N', ','}, {'F', '!'}, {'C', ':'}, {'K', '('},
    {'T', '5'}, {'Z', '"'}, {'L', ')'}, {'W', '2'}, {'H', '#'}, {'Y', '6'}, {'P', '0'}, {'Q', '1'},
    {'O', '9'}, {'B', '?'}, {'G', '&'}, {0, 0}, {'M', '.'}, {'X', '/'}, {0, 0}, {0, 0},
};

static const uint8_t s_gray_8psk_softbits[8][3] = {
    {25, 0, 25}, {0, 25, 230}, {0, 230, 230}, {25, 255, 25},
    {230, 255, 25}, {255, 230, 230}, {255, 25, 230}, {230, 0, 25},
};

static const unsigned short s_mfsk_varidecode[256] = {
    0x75C,0x760,0x768,0x76C,0x770,0x774,0x778,0x77C,0x0A8,0x780,0x7A0,0x7A8,0x7AC,0x0AC,0x7B0,0x7B4,
    0x7B8,0x7BC,0x7C0,0x7D0,0x7D4,0x7D8,0x7DC,0x7E0,0x7E8,0x7EC,0x7F0,0x7F4,0x7F8,0x7FC,0x800,0xA00,
    0x004,0x1C0,0x1FC,0x2D8,0x2A8,0x2A0,0x200,0x1BC,0x1F4,0x1F0,0x2B4,0x1E0,0x0A0,0x1D8,0x1D4,0x1E8,
    0x0E0,0x0F0,0x140,0x154,0x174,0x160,0x16C,0x1A0,0x180,0x1AC,0x1EC,0x1F8,0x2C0,0x1DC,0x2BC,0x1D0,
    0x280,0x0BC,0x100,0x0D4,0x0DC,0x0B8,0x0F8,0x150,0x158,0x0C0,0x1B4,0x17C,0x0F4,0x0E8,0x0FC,0x0D0,
    0x0EC,0x1B0,0x0D8,0x0B4,0x0B0,0x15C,0x1A8,0x168,0x170,0x178,0x1B8,0x2E8,0x2D0,0x2EC,0x2D4,0x2B0,
    0x2AC,0x014,0x060,0x038,0x034,0x008,0x050,0x058,0x030,0x018,0x080,0x070,0x02C,0x040,0x01C,0x010,
    0x054,0x078,0x020,0x028,0x00C,0x03C,0x06C,0x068,0x074,0x05C,0x07C,0x2DC,0x2B8,0x2E0,0x2F0,0xA80,
    0xAA0,0xAA8,0xAAC,0xAB0,0xAB4,0xAB8,0xABC,0xAC0,0xAD0,0xAD4,0xAD8,0xADC,0xAE0,0xAE8,0xAEC,0xAF0,
    0xAF4,0xAF8,0xAFC,0xB00,0xB40,0xB50,0xB54,0xB58,0xB5C,0xB60,0xB68,0xB6C,0xB70,0xB74,0xB78,0xB7C,
    0x2F4,0x2F8,0x2FC,0x300,0x340,0x350,0x354,0x358,0x35C,0x360,0x368,0x36C,0x370,0x374,0x378,0x37C,
    0x380,0x3A0,0x3A8,0x3AC,0x3B0,0x3B4,0x3B8,0x3BC,0x3C0,0x3D0,0x3D4,0x3D8,0x3DC,0x3E0,0x3E8,0x3EC,
    0x3F0,0x3F4,0x3F8,0x3FC,0x400,0x500,0x540,0x550,0x554,0x558,0x55C,0x560,0x568,0x56C,0x570,0x574,
    0x578,0x57C,0x580,0x5A0,0x5A8,0x5AC,0x5B0,0x5B4,0x5B8,0x5BC,0x5C0,0x5D0,0x5D4,0x5D8,0x5DC,0x5E0,
    0x5E8,0x5EC,0x5F0,0x5F4,0x5F8,0x5FC,0x600,0x680,0x6A0,0x6A8,0x6AC,0x6B0,0x6B4,0x6B8,0x6BC,0x6C0,
    0x6D0,0x6D4,0x6D8,0x6DC,0x6E0,0x6E8,0x6EC,0x6F0,0x6F4,0x6F8,0x6FC,0x700,0x740,0x750,0x754,0x758
};

typedef struct {
    uint8_t family;
    uint8_t bits_per_symbol;
    uint8_t carriers;
    uint8_t fec;
    uint8_t punctured;
    uint8_t mfsk_varicode;
    uint8_t fec_k;
    uint8_t dual_fec_branch;
    uint8_t decision_directed_afc;
    uint8_t fec_settle_tracebacks;
    uint8_t fec_settle_half_interleaves;
    uint16_t acquire_symbols;
    uint16_t interleave_depth;
    uint32_t fec_poly1;
    uint32_t fec_poly2;
    float baud;
} psk_mode_params_t;

static void init_tables(void)
{
    if (s_tables_ready) {
        return;
    }
    memset(s_varicode_decode, 0, sizeof(s_varicode_decode));
    memset(s_mfsk_decode, 0, sizeof(s_mfsk_decode));
    for (int ch = 0; ch < 256; ++ch) {
        const unsigned int code = VARICODE_TABLE[ch];
        if (code < sizeof(s_varicode_decode)) {
            s_varicode_decode[code] = (unsigned char)ch;
        }
        if (s_mfsk_varidecode[ch] < sizeof(s_mfsk_decode)) {
            s_mfsk_decode[s_mfsk_varidecode[ch]] = (unsigned char)ch;
        }
    }
    s_tables_ready = 1;
}

static psk_mode_params_t mode_params(psk_rtty_mode_t mode)
{
    psk_mode_params_t p = {
        .family = PSK_FAMILY_BPSK,
        .bits_per_symbol = 1,
        .carriers = 1,
        .fec = 0,
        .punctured = 0,
        .mfsk_varicode = 0,
        .fec_k = 0,
        .dual_fec_branch = 0,
        .decision_directed_afc = 0,
        .fec_settle_tracebacks = 1,
        .fec_settle_half_interleaves = 0,
        .acquire_symbols = RX_ACQUIRE_SYMBOLS,
        .interleave_depth = 0,
        .fec_poly1 = 0,
        .fec_poly2 = 0,
        .baud = 31.25f,
    };
    switch (mode) {
    case PSK_RTTY_MODE_PSK63:
        p.baud = 62.5f;
        break;
    case PSK_RTTY_MODE_BPSK63F:
        p.baud = 62.5f;
        p.fec = 1;
        p.mfsk_varicode = 1;
        p.fec_k = 7;
        p.dual_fec_branch = 1;
        p.acquire_symbols = 1;
        p.fec_poly1 = 0x6dU;
        p.fec_poly2 = 0x4fU;
        break;
    case PSK_RTTY_MODE_BPSK125:
        p.baud = 125.0f;
        break;
    case PSK_RTTY_MODE_BPSK250:
        p.baud = 250.0f;
        break;
    case PSK_RTTY_MODE_BPSK500:
        p.baud = 500.0f;
        break;
    case PSK_RTTY_MODE_BPSK1000:
        p.baud = 1000.0f;
        break;
    case PSK_RTTY_MODE_12XPSK125:
        p.baud = 125.0f;
        p.carriers = 12;
        break;
    case PSK_RTTY_MODE_6XPSK250:
        p.baud = 250.0f;
        p.carriers = 6;
        break;
    case PSK_RTTY_MODE_2XPSK500:
        p.baud = 500.0f;
        p.carriers = 2;
        break;
    case PSK_RTTY_MODE_4XPSK500:
        p.baud = 500.0f;
        p.carriers = 4;
        break;
    case PSK_RTTY_MODE_2XPSK800:
        p.baud = 800.0f;
        p.carriers = 2;
        break;
    case PSK_RTTY_MODE_2XPSK1000:
        p.baud = 1000.0f;
        p.carriers = 2;
        break;
    case PSK_RTTY_MODE_QPSK31_USB:
    case PSK_RTTY_MODE_QPSK31_LSB:
    case PSK_RTTY_MODE_QPSK63:
    case PSK_RTTY_MODE_QPSK125:
    case PSK_RTTY_MODE_QPSK250:
    case PSK_RTTY_MODE_QPSK500:
        p.family = PSK_FAMILY_QPSK;
        p.bits_per_symbol = 2;
        p.fec = 1;
        p.decision_directed_afc = 1;
        p.fec_k = 5;
        p.fec_poly1 = 0x17U;
        p.fec_poly2 = 0x19U;
        if (mode == PSK_RTTY_MODE_QPSK63) {
            p.baud = 62.5f;
        } else if (mode == PSK_RTTY_MODE_QPSK125) {
            p.baud = 125.0f;
        } else if (mode == PSK_RTTY_MODE_QPSK250) {
            p.baud = 250.0f;
        } else if (mode == PSK_RTTY_MODE_QPSK500) {
            p.baud = 500.0f;
        }
        break;
    case PSK_RTTY_MODE_8PSK125:
    case PSK_RTTY_MODE_8PSK250:
    case PSK_RTTY_MODE_8PSK500:
    case PSK_RTTY_MODE_8PSK1000:
    case PSK_RTTY_MODE_8PSK125F:
    case PSK_RTTY_MODE_8PSK250F:
    case PSK_RTTY_MODE_8PSK500F:
    case PSK_RTTY_MODE_8PSK1000F:
    case PSK_RTTY_MODE_8PSK1200F:
        p.family = PSK_FAMILY_8PSK;
        p.bits_per_symbol = 3;
        p.mfsk_varicode = 1;
        p.decision_directed_afc = 1;
        if (mode == PSK_RTTY_MODE_8PSK250 || mode == PSK_RTTY_MODE_8PSK250F) {
            p.baud = 250.0f;
        } else if (mode == PSK_RTTY_MODE_8PSK500 || mode == PSK_RTTY_MODE_8PSK500F) {
            p.baud = 500.0f;
        } else if (mode == PSK_RTTY_MODE_8PSK1000 || mode == PSK_RTTY_MODE_8PSK1000F) {
            p.baud = 1000.0f;
        } else if (mode == PSK_RTTY_MODE_8PSK1200F) {
            p.baud = 16000.0f / 13.0f;
        } else {
            p.baud = 125.0f;
        }
        if (mode >= PSK_RTTY_MODE_8PSK125F) {
            p.fec = 1;
            if (mode == PSK_RTTY_MODE_8PSK125F) {
                p.fec_k = 16;
                p.dual_fec_branch = 1;
                p.fec_poly1 = 0152711U;
                p.fec_poly2 = 0126723U;
                p.interleave_depth = 384;
            } else if (mode == PSK_RTTY_MODE_8PSK250F) {
                p.fec_k = 16;
                p.dual_fec_branch = 1;
                p.acquire_symbols = 0;
                p.fec_poly1 = 0152711U;
                p.fec_poly2 = 0126723U;
                p.interleave_depth = 512;
            } else if (mode == PSK_RTTY_MODE_8PSK500F) {
                p.fec_k = 13;
                p.fec_poly1 = 016461U;
                p.fec_poly2 = 012767U;
                p.interleave_depth = 640;
                p.punctured = 1;
            } else {
                p.fec_k = 7;
                p.fec_poly1 = 0x6dU;
                p.fec_poly2 = 0x4fU;
                p.interleave_depth = 512;
                p.punctured = 1;
            }
        }
        break;
    case PSK_RTTY_MODE_RTTY:
        p.baud = 45.45f;
        break;
    case PSK_RTTY_MODE_BPSK31:
    default:
        break;
    }
    if (p.interleave_depth != 0U) {
        p.fec_settle_half_interleaves = 2U;
    }
    if (p.punctured && p.mfsk_varicode && p.fec_k <= 7U) {
        p.fec_settle_tracebacks = 2U;
        p.fec_settle_half_interleaves = 5U;
    }
    return p;
}

static void reset_runtime(psk_rtty_rx_t *rx)
{
    const psk_mode_params_t params = mode_params(rx->config.mode);
    rx->nco_phase = 0.0f;
    rx->nco_phase_inc = TWO_PI * (float)rx->config.rx_tone_hz / (float)rx->config.sample_rate_hz;
    rx->nco_cos = 1.0f;
    rx->nco_sin = 0.0f;
    rx->freq_error = 0.0f;
    rx->afc_limit = 50.0f * TWO_PI / (float)rx->config.sample_rate_hz;
    rx->afc_min = rx->nco_phase_inc - rx->afc_limit;
    rx->afc_max = rx->nco_phase_inc + rx->afc_limit;
    if (rx->afc_min < 0.0f) {
        rx->afc_min = 0.0f;
    }
    rx->fferr_ave = 0.0f;
    rx->fperr_ave = 0.0f;
    rx->q_freq_error = 0.0f;
    rx->nlp_k = PSKCORE_NLP_K;
    rx->agc_ave = 0.0f;
    rx->dev_ave = 90.0f;
    rx->bpsk_average_amp = 0.0f;
    rx->psk8_last_phase_quality = 0.0f;
    rx->psk8_coarse_freq = 0.0f;
    rx->psk8_eighth_prev.x = 0.0f;
    rx->psk8_eighth_prev.y = 0.0f;
    rx->psk8_eighth_valid = 0U;
    rx->bit_phase_pos = 0.0f;
    rx->second_decim = (int)((float)rx->config.sample_rate_hz / (params.baud * 16.0f) + 0.5f);
    if (rx->second_decim < 1) {
        rx->second_decim = 1;
    }
    /* Legacy rates choose an integer decimation that yields 16
     * samples/symbol.  1200F is 16000/13 baud, leaving exactly 13 samples at
     * the 16 kHz receiver; keep its timing loop in the reachable 13-bin
     * domain instead of assuming all 16 legacy bins are present. */
    rx->timing_bins = (int)((float)rx->config.sample_rate_hz /
                            (params.baud * (float)rx->second_decim) + 0.5f);
    if (rx->timing_bins < 4) {
        rx->timing_bins = 4;
    } else if (rx->timing_bins > PSK_TIMING_MAX_BINS) {
        rx->timing_bins = PSK_TIMING_MAX_BINS;
    }
    rx->bit_phase_inc = PSKCORE_TS * params.baud * (float)rx->second_decim /
                        (float)rx->config.sample_rate_hz;
    rx->prev_symbol_phase = 0.0f;
    rx->symbol_clock = 0.0f;
    rx->symbol_period_samples = (float)rx->config.sample_rate_hz / params.baud;
    rx->i_acc = 0.0f;
    rx->q_acc = 0.0f;
    rx->samples_in_symbol = 0;
    rx->sample_count = 0;
    rx->afc_enabled = PSKCORE_AFC_ON;
    for (uint8_t car = 0; car < MAX_PSK_CARRIERS; ++car) {
        rx->chan_dec1_state[car] = PSKCORE_DEC4_LPFIR_LENGTH - 1;
        rx->chan_dec2_state[car] = PSKCORE_DEC4_LPFIR_LENGTH - 1;
        rx->chan_bit_fir_state[car] = PSKCORE_BITFIR_LENGTH - 1;
    }
    rx->pk_pos = rx->timing_bins / 3;
    rx->new_pk_pos = rx->pk_pos;
    rx->raw_pk_pos = rx->pk_pos;
    rx->timing_half_pos = (rx->pk_pos + rx->timing_bins / 2) % rx->timing_bins;
    rx->bit_pos = 0;
    rx->warmup_symbols = (int)params.acquire_symbols;
    rx->last_pk_pos = 0;
    rx->sq_open = 0;
    rx->sq_was_open = 0;
    rx->sq_stable_symbols = 0;
    rx->sq_level = 10;
    rx->sq_thresh = 35;
    rx->squelch_enabled = 1;
    rx->squelch_speed = 75;
    rx->on_count = 0;
    rx->off_count = 0;
    rx->p_count = 0;
    rx->n_count = 0;
    rx->bit_acc = 0;
    rx->last_bit_zero = 0;
    rx->rtty_figures = 0;
    rx->rtty_bit_index = 0;
    rx->rtty_shift_reg = 0;
    rx->i0 = 0.0f;
    rx->i1 = 0.0f;
    rx->q0 = 0.0f;
    rx->q1 = 0.0f;
    rx->carriers = params.carriers;
    rx->bits_per_symbol = params.bits_per_symbol;
    rx->psk_family = params.family;
    rx->decision_directed_afc = params.decision_directed_afc;
    rx->fec_enabled = params.fec;
    rx->fec_punctured = params.punctured;
    rx->use_mfsk_varicode = params.mfsk_varicode;
    rx->fec_pair_count = 0;
    rx->fec_dual_branch = params.dual_fec_branch;
    rx->fec_locked_branch = 0xffU;
    rx->fec_vote_candidate = 0xffU;
    rx->fec_vote_count = 0U;
    rx->fec_rxbitstate = 0;
    rx->fec_bit_phase = 0;
    rx->fec_last_encoded_bit = 0;
    rx->fec_have_last_encoded_bit = 0;
    rx->fec_pair_len = 0;
    rx->fec_stream_bit_count = 0;
    rx->fec_phase_metric[0] = 0;
    rx->fec_phase_metric[1] = 0;
    rx->fec_phase_search = 0U;
    rx->fec_phase_candidate = 0U;
    rx->fec_phase_align_pending = rx->fec_phase_search;
    rx->fec_output_enabled = rx->fec_phase_search ? 0U : 1U;
    rx->fec_k = params.fec_k;
    rx->fec_state = 0;
    rx->fec_mask = params.fec_k ? ((1UL << params.fec_k) - 1UL) : 0UL;
    rx->fec_poly1 = params.fec_poly1;
    rx->fec_poly2 = params.fec_poly2;
    rx->vit_traceback = params.fec_k ? (uint8_t)(params.fec_k * 12U) : 0U;
    if (rx->vit_traceback > VIT_MAX_TRACEBACK) {
        rx->vit_traceback = VIT_MAX_TRACEBACK;
    }
    rx->vit_chunksize = rx->fec_dual_branch ? VIT_DUAL_CHUNKSIZE : VIT_CHUNKSIZE;
    rx->fec_settle_tracebacks = params.fec_settle_tracebacks;
    rx->fec_settle_half_interleaves = params.fec_settle_half_interleaves;
    rx->debug_fec_metric[0] = 0;
    rx->debug_fec_metric[1] = 0;
    rx->debug_fec_selected = 0U;
    rx->inlv_depth = params.interleave_depth;
    rx->inlv_len = (uint16_t)(params.interleave_depth * 4U);
    if (rx->inlv_len > sizeof(s_inlv_table0)) {
        rx->inlv_len = sizeof(s_inlv_table0);
    }
    memset(rx->fec_pending_bits, 0, sizeof(rx->fec_pending_bits));
    memset(rx->fec_pair_buf, 0, sizeof(rx->fec_pair_buf));
    reset_fec_branches(rx);
    memset(rx->carrier_i_acc, 0, sizeof(rx->carrier_i_acc));
    memset(rx->carrier_q_acc, 0, sizeof(rx->carrier_q_acc));
    memset(rx->carrier_i_prev, 0, sizeof(rx->carrier_i_prev));
    memset(rx->carrier_q_prev, 0, sizeof(rx->carrier_q_prev));
    memset(rx->carrier_has_prev, 0, sizeof(rx->carrier_has_prev));
    const float spacing = 1.4f * params.baud;
    const float first = (float)rx->config.rx_tone_hz + (((-1.0f * (float)params.carriers) + 1.0f) * spacing * 0.5f);
    for (uint8_t car = 0; car < MAX_PSK_CARRIERS; ++car) {
        const float freq = first + (float)car * spacing;
        rx->carrier_phase_inc[car] = TWO_PI * freq / (float)rx->config.sample_rate_hz;
        rx->carrier_rot_cos[car] = cosf(rx->carrier_phase_inc[car]);
        rx->carrier_rot_sin[car] = sinf(rx->carrier_phase_inc[car]);
        rx->carrier_cos[car] = 1.0f;
        rx->carrier_sin[car] = 0.0f;
    }
    memset(rx->sync_ave, 0, sizeof(rx->sync_ave));
    memset(rx->chan_dec1_queue, 0, sizeof(rx->chan_dec1_queue));
    memset(rx->chan_dec2_queue, 0, sizeof(rx->chan_dec2_queue));
    memset(rx->chan_bit_queue, 0, sizeof(rx->chan_bit_queue));
    memset(rx->chan_freq_signal, 0, sizeof(rx->chan_freq_signal));
    memset(rx->chan_bit_signal, 0, sizeof(rx->chan_bit_signal));
    memset(rx->chan_mix_i_acc, 0, sizeof(rx->chan_mix_i_acc));
    memset(rx->chan_mix_q_acc, 0, sizeof(rx->chan_mix_q_acc));
    rx->chan_mix_decim_count = 0;
    memset(&rx->z1, 0, sizeof(rx->z1));
    memset(&rx->z2, 0, sizeof(rx->z2));
    refresh_lock_state(rx);
}

void psk_rtty_rx_init(psk_rtty_rx_t *rx, const psk_rtty_config_t *config)
{
    init_tables();
    memset(rx, 0, sizeof(*rx));
    rx->config = *config;
    reset_runtime(rx);
}

void psk_rtty_rx_set_mode(psk_rtty_rx_t *rx, psk_rtty_mode_t mode)
{
    if (mode >= PSK_RTTY_MODE_COUNT) {
        return;
    }
    rx->config.mode = mode;
    reset_runtime(rx);
}

void psk_rtty_rx_set_tone(psk_rtty_rx_t *rx, int tone_hz)
{
    if (tone_hz < 100) {
        tone_hz = 100;
    } else if (tone_hz > 3500) {
        tone_hz = 3500;
    }
    rx->config.rx_tone_hz = tone_hz;
    reset_runtime(rx);
}

static void emit(psk_rtty_rx_t *rx, char c)
{
    if (c && rx->config.char_cb) {
        rx->config.char_cb(c, rx->config.userdata);
    }
}

static void push_varicode_bit(psk_rtty_rx_t *rx, int bit)
{
    if (bit == 0) {
        if (rx->last_bit_zero) {
            rx->debug_delims++;
            if (rx_signal_ready(rx) && rx_fec_ready(rx) && rx->bit_acc < 2048) {
                const unsigned char ch = s_varicode_decode[rx->bit_acc];
                if (ch != 0) {
                    rx->debug_chars++;
                    emit(rx, (char)ch);
                } else if (rx->bit_acc != 0) {
                    rx->debug_unknown++;
                }
            }
            rx->bit_acc = 0;
            rx->last_bit_zero = 0;
        } else {
            rx->last_bit_zero = 1;
        }
        return;
    }

    if (rx->last_bit_zero) {
        rx->bit_acc <<= 1;
        rx->last_bit_zero = 0;
    }
    rx->bit_acc = (uint16_t)(((rx->bit_acc << 1) | 1U) & 0x07ffU);
}

static void push_mfsk_varicode_bit(psk_rtty_rx_t *rx, int bit)
{
    const int can_decode = rx_signal_ready(rx) && rx_fec_ready(rx);
    if (!can_decode) {
        rx->bit_acc = 1U;
        return;
    }

    rx->bit_acc = (uint16_t)(((rx->bit_acc << 1) | (bit ? 1U : 0U)) & 0x0fffU);
    if ((rx->bit_acc & 0x07U) == 1U) {
        const uint16_t code = rx->bit_acc >> 1;
        const unsigned char ch = code < sizeof(s_mfsk_decode) ? s_mfsk_decode[code] : 0U;
        rx->debug_delims++;
        if (ch != 0U && is_rx_text_char(ch)) {
            rx->debug_chars++;
            emit(rx, (char)ch);
        } else if (code != 0U) {
            rx->debug_unknown++;
        }
        rx->bit_acc = 1U;
    }
}

static int fec_branch_selected(uint8_t branch_index)
{
    fec_branch_state_t *branch = &s_fec_branch[branch_index];
    fec_branch_state_t *other = &s_fec_branch[branch_index ^ 1U];
    if (!branch->metric_ready || !other->metric_ready) {
        return 0;
    }
    return branch->metric_ave > other->metric_ave + FEC_BRANCH_VOTE_HYSTERESIS;
}

static void update_fec_branch_lock(psk_rtty_rx_t *rx)
{
    if (!rx->fec_dual_branch || rx->fec_locked_branch < 2U) {
        return;
    }
    const uint32_t required = fec_ready_pairs_required(rx);
    if (s_fec_branch[0].pair_count <= required || s_fec_branch[1].pair_count <= required) {
        return;
    }

    uint8_t candidate = 0xffU;
    if (fec_branch_selected(0U)) {
        candidate = 0U;
    } else if (fec_branch_selected(1U)) {
        candidate = 1U;
    }
    if (candidate >= 2U) {
        rx->fec_vote_candidate = 0xffU;
        rx->fec_vote_count = 0U;
        return;
    }
    if (candidate != rx->fec_vote_candidate) {
        rx->fec_vote_candidate = candidate;
        rx->fec_vote_count = 1U;
        return;
    }
    if (rx->fec_vote_count < FEC_BRANCH_LOCK_VOTES) {
        rx->fec_vote_count++;
    }
    if (rx->fec_vote_count >= FEC_BRANCH_LOCK_VOTES) {
        rx->fec_locked_branch = candidate;
        rx->debug_fec_selected = candidate;
    }
}

static void push_mfsk_varicode_bit_branch(psk_rtty_rx_t *rx, fec_branch_state_t *branch,
                                          uint8_t branch_index, int bit)
{
    const int can_decode = branch_index == rx->fec_locked_branch &&
                           branch->pair_count > fec_ready_pairs_required(rx) &&
                           rx_signal_ready(rx) &&
                           fec_branch_selected(branch_index);
    if (!can_decode) {
        branch->bit_acc = 1U;
        return;
    }

    branch->bit_acc = (uint16_t)(((branch->bit_acc << 1) | (bit ? 1U : 0U)) & 0x0fffU);
    if ((branch->bit_acc & 0x07U) == 1U) {
        const uint16_t code = branch->bit_acc >> 1;
        const unsigned char ch = code < sizeof(s_mfsk_decode) ? s_mfsk_decode[code] : 0U;
        rx->debug_delims++;
        if (ch != 0U && is_rx_text_char(ch)) {
            rx->debug_chars++;
            emit(rx, (char)ch);
        } else if (code != 0U) {
            rx->debug_unknown++;
        }
        branch->bit_acc = 1U;
    }
}

static void push_payload_bit(psk_rtty_rx_t *rx, int bit)
{
    if (rx->use_mfsk_varicode) {
        push_mfsk_varicode_bit(rx, bit);
    } else {
        push_varicode_bit(rx, bit);
    }
}

static uint8_t parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0x0fU;
    return (uint8_t)((0x6996U >> value) & 1U);
}

static void reset_fec_branch(psk_rtty_rx_t *rx, uint8_t branch_index)
{
    const uint16_t nstates = rx->fec_k ? (uint16_t)(1UL << (rx->fec_k - 1U)) : 0U;
    fec_branch_state_t *state = &s_fec_branch[branch_index];

    state->pair_count = 0;
    state->vit_step = 0;
    state->bit_acc = 1U;
    state->metric_ave = 0;
    state->metric_ready = 0;
    memset(state->inlv_table, FEC_PUNCTURE, sizeof(s_inlv_table0));
    if (nstates != 0U) {
        memset(state->metrics_a, 0, (size_t)nstates * sizeof(state->metrics_a[0]));
        memset(state->metrics_b, 0, (size_t)nstates * sizeof(state->metrics_b[0]));
    }
}

static void reset_fec_branches(psk_rtty_rx_t *rx)
{
    const uint8_t branch_count = rx->fec_dual_branch ? 2U : 1U;

    for (uint8_t branch = 0; branch < branch_count; ++branch) {
        reset_fec_branch(rx, branch);
    }
}

static void reset_fec_acquisition(psk_rtty_rx_t *rx)
{
    if (!rx->fec_enabled) {
        return;
    }

    rx->fec_pair_count = 0U;
    rx->fec_locked_branch = 0xffU;
    rx->fec_vote_candidate = 0xffU;
    rx->fec_vote_count = 0U;
    rx->fec_rxbitstate = 0U;
    rx->fec_bit_phase = 0U;
    rx->fec_last_encoded_bit = 0U;
    rx->fec_have_last_encoded_bit = 0U;
    rx->fec_pair_len = 0U;
    rx->fec_stream_bit_count = 0U;
    rx->fec_phase_metric[0] = 0;
    rx->fec_phase_metric[1] = 0;
    rx->debug_fec_metric[0] = 0;
    rx->debug_fec_metric[1] = 0;
    rx->debug_fec_selected = 0U;
    rx->bit_acc = 1U;
    memset(rx->fec_pending_bits, 0, sizeof(rx->fec_pending_bits));
    memset(rx->fec_pair_buf, 0, sizeof(rx->fec_pair_buf));
    reset_fec_branches(rx);
}

static uint32_t fec_ready_pairs_required(const psk_rtty_rx_t *rx)
{
    uint32_t required = (uint32_t)rx->vit_traceback * rx->fec_settle_tracebacks;
    required += ((uint32_t)rx->inlv_depth * rx->fec_settle_half_interleaves) / 2U;
    return required;
}

static int rx_signal_ready(const psk_rtty_rx_t *rx)
{
    return rx->warmup_symbols <= 0 && rx->sq_open &&
           rx->sq_stable_symbols >= SQ_EMIT_STABLE_SYMBOLS;
}

static int rx_fec_ready(const psk_rtty_rx_t *rx)
{
    if (!rx->fec_enabled) {
        return 1;
    }

    const uint32_t required = fec_ready_pairs_required(rx);
    if (!rx->fec_output_enabled || s_fec_branch[0].pair_count <= required) {
        return 0;
    }
    if (!rx->fec_dual_branch) {
        return 1;
    }
    return rx->fec_locked_branch < 2U &&
           s_fec_branch[rx->fec_locked_branch].pair_count > required;
}

static void refresh_lock_state(psk_rtty_rx_t *rx)
{
    if (rx->warmup_symbols > 0 || !rx->sq_open) {
        rx->lock_state = PSK_RTTY_LOCK_ACQUIRE;
    } else if (rx->sq_stable_symbols < SQ_EMIT_STABLE_SYMBOLS) {
        rx->lock_state = PSK_RTTY_LOCK_SIGNAL;
    } else if (!rx_fec_ready(rx)) {
        rx->lock_state = PSK_RTTY_LOCK_FEC;
    } else {
        rx->lock_state = PSK_RTTY_LOCK_READY;
    }
}

static int is_rx_text_char(unsigned char ch)
{
    return ch == '\r' || ch == '\n' || ch == '\t' || (ch >= 32U && ch <= 126U);
}

static void push_payload_bits_from_fec(psk_rtty_rx_t *rx, fec_branch_state_t *branch,
                                       uint8_t branch_index, uint8_t decoded)
{
    for (int bit = (int)rx->vit_chunksize - 1; bit >= 0; --bit) {
        const int payload_bit = (decoded >> bit) & 1U;
        if (rx->fec_dual_branch && rx->use_mfsk_varicode) {
            push_mfsk_varicode_bit_branch(rx, branch, branch_index, payload_bit);
        } else {
            push_payload_bit(rx, payload_bit);
        }
    }
}

static void update_fec_metric(fec_branch_state_t *branch, int metric)
{
    if (!branch->metric_ready) {
        branch->metric_ave = metric;
        branch->metric_ready = 1;
    } else {
        branch->metric_ave = ((branch->metric_ave * 19) + metric) / 20;
    }
}

static void update_fec_phase_search(psk_rtty_rx_t *rx, fec_branch_state_t *branch)
{
    if (rx->fec_phase_search == 0U || rx->fec_phase_search >= 3U ||
        branch->pair_count < FEC_PHASE_EVAL_PAIRS) {
        return;
    }

    const uint8_t candidate = rx->fec_phase_candidate;
    rx->fec_phase_metric[candidate] = branch->metric_ave;
    rx->debug_fec_metric[candidate] = branch->metric_ave;

    if (rx->fec_phase_search == 1U) {
        rx->fec_phase_search = 2U;
        rx->fec_phase_candidate = 1U;
        rx->fec_phase_align_pending = 1U;
        rx->fec_pair_len = 0U;
        rx->bit_acc = 1U;
        reset_fec_branch(rx, 0U);
        return;
    }

    if (rx->fec_phase_metric[1] > rx->fec_phase_metric[0]) {
        rx->fec_phase_candidate = 1U;
        rx->debug_fec_selected = 1U;
    } else {
        rx->fec_phase_candidate = 0U;
        rx->debug_fec_selected = 0U;
        rx->fec_phase_align_pending = 1U;
        rx->fec_pair_len = 0U;
        rx->bit_acc = 1U;
        reset_fec_branch(rx, 0U);
    }
    rx->fec_phase_search = 3U;
    rx->fec_output_enabled = 1U;
}

static void fec_deinterleave_pair(psk_rtty_rx_t *rx, fec_branch_state_t *state, uint8_t pair[2])
{
    if (rx->inlv_depth == 0U || rx->inlv_len < 4U) {
        return;
    }

    for (uint16_t k = 0; k < rx->inlv_depth; ++k) {
        const uint16_t base = (uint16_t)(k * 4U);
        state->inlv_table[base + 0U] = state->inlv_table[base + 1U];
        state->inlv_table[base + 1U] = pair[0];
        state->inlv_table[base + 2U] = state->inlv_table[base + 3U];
        state->inlv_table[base + 3U] = pair[1];
        pair[0] = state->inlv_table[base + 0U];
        pair[1] = state->inlv_table[base + 3U];
    }
}

static PSK_FAST_TEXT int branch_metric(uint8_t expected, uint8_t b0, uint8_t b1)
{
    int metric = 0;
    const uint8_t e0 = expected & 1U;
    const uint8_t e1 = (expected >> 1U) & 1U;
    if (b0 != FEC_PUNCTURE) {
        metric += e0 ? ((int)b0 - 128) : (128 - (int)b0);
    }
    if (b1 != FEC_PUNCTURE) {
        metric += e1 ? ((int)b1 - 128) : (128 - (int)b1);
    }
    return metric;
}

static PSK_FAST_TEXT void prepare_viterbi_tables(const psk_rtty_rx_t *rx, uint16_t nstates)
{
    if (s_vit_table_k == rx->fec_k &&
        s_vit_table_poly1 == rx->fec_poly1 &&
        s_vit_table_poly2 == rx->fec_poly2) {
        return;
    }

    for (uint32_t pred = 0; pred < ((uint32_t)nstates >> 1U); ++pred) {
        const uint32_t even_state = pred << 1U;
        const uint32_t odd_state = even_state + 1U;
        const uint8_t even_out = (uint8_t)(parity32(even_state & rx->fec_poly1) |
                                           (parity32(even_state & rx->fec_poly2) << 1U));
        const uint8_t odd_out = (uint8_t)(parity32(odd_state & rx->fec_poly1) |
                                          (parity32(odd_state & rx->fec_poly2) << 1U));
        s_vit_out_pair[pred] = (uint8_t)(even_out | (odd_out << 2U));
    }
    s_vit_input_flip = (uint8_t)(((rx->fec_poly1 & nstates) ? 1U : 0U) |
                                 ((rx->fec_poly2 & nstates) ? 2U : 0U));

    s_vit_table_k = rx->fec_k;
    s_vit_table_poly1 = rx->fec_poly1;
    s_vit_table_poly2 = rx->fec_poly2;
}

static PSK_FAST_TEXT int vit_get_decision(const fec_branch_state_t *branch, uint8_t step, uint16_t state)
{
    return (branch->decisions[step][state >> 3U] >> (state & 7U)) & 1U;
}

static PSK_FAST_TEXT uint8_t vit_traceback(psk_rtty_rx_t *rx, fec_branch_state_t *branch,
                                           uint16_t nstates, const int16_t *metrics, int *metric)
{
    int16_t best_metric = metrics[0];
    uint16_t state = 0;
    for (uint16_t i = 1; i < nstates; ++i) {
        if (metrics[i] > best_metric) {
            best_metric = metrics[i];
            state = i;
        }
    }

    uint8_t step = (uint8_t)((branch->vit_step + VIT_MAX_TRACEBACK - 1U) % VIT_MAX_TRACEBACK);
    branch->sequence[rx->vit_traceback] = state;
    for (int i = (int)rx->vit_traceback - 1; i >= 0; --i) {
        const int decision = vit_get_decision(branch, step, state);
        state = (uint16_t)(((uint32_t)state + (decision ? (uint32_t)nstates : 0UL)) >> 1U);
        branch->sequence[i] = state;
        step = (uint8_t)((step + VIT_MAX_TRACEBACK - 1U) % VIT_MAX_TRACEBACK);
    }

    uint8_t out = 0;
    for (uint8_t i = 0; i < rx->vit_chunksize; ++i) {
        out = (uint8_t)((out << 1U) | (branch->sequence[i] & 1U));
    }

    int path_metric = 0;
    uint8_t metric_step = (uint8_t)((step + 1U) % VIT_MAX_TRACEBACK);
    for (uint8_t i = 0; i < rx->vit_chunksize; ++i) {
        const uint16_t current_state = branch->sequence[i + 1U];
        const int decision = vit_get_decision(branch, metric_step, current_state);
        const uint16_t base_state = (uint16_t)(current_state & (nstates - 1U));
        const uint8_t packed = s_vit_out_pair[base_state >> 1U];
        uint8_t expected = (base_state & 1U) ? (uint8_t)(packed >> 2U) : (uint8_t)(packed & 3U);
        if (decision) {
            expected ^= s_vit_input_flip;
        }
        path_metric += branch_metric(expected, branch->rx_pair0[metric_step], branch->rx_pair1[metric_step]);
        metric_step = (uint8_t)((metric_step + 1U) % VIT_MAX_TRACEBACK);
    }
    *metric = path_metric;
    return out;
}

static PSK_FAST_TEXT int fec_decode_pair(psk_rtty_rx_t *rx, fec_branch_state_t *branch,
                                         uint8_t b0, uint8_t b1, uint8_t *decoded, int *metric)
{
    if (!rx->fec_k || rx->fec_k > 16U) {
        return 0;
    }

    const uint16_t nstates = (uint16_t)(1UL << (rx->fec_k - 1U));
    int16_t *prev = (branch->vit_step & 1U) ? branch->metrics_b : branch->metrics_a;
    int16_t *curr = (branch->vit_step & 1U) ? branch->metrics_a : branch->metrics_b;
    int16_t max_metric = -32768;
    int16_t metric_table[4];

    prepare_viterbi_tables(rx, nstates);
    for (uint8_t expected = 0; expected < 4U; ++expected) {
        metric_table[expected] = (int16_t)branch_metric(expected, b0, b1);
    }
    branch->rx_pair0[branch->vit_step] = b0;
    branch->rx_pair1[branch->vit_step] = b1;
    uint8_t *decision_row = branch->decisions[branch->vit_step];
    const uint16_t predecessor_half = (uint16_t)(nstates >> 1U);
    for (uint16_t state_base = 0; state_base < nstates; state_base = (uint16_t)(state_base + 8U)) {
        uint8_t decision_byte = 0;
        const uint16_t pred_base = (uint16_t)(state_base >> 1U);
        for (uint8_t pair = 0; pair < 4U; ++pair) {
            const uint16_t pred = (uint16_t)(pred_base + pair);
            const uint16_t state_even = (uint16_t)(state_base + pair * 2U);
            const uint16_t state_odd = (uint16_t)(state_even + 1U);
            const int16_t path0 = prev[pred];
            const int16_t path1 = prev[pred + predecessor_half];
            const uint8_t packed = s_vit_out_pair[pred];
            const uint8_t out_even = packed & 3U;
            const uint8_t out_odd = packed >> 2U;

            const int16_t even0 = (int16_t)(path0 + metric_table[out_even]);
            const int16_t even1 = (int16_t)(path1 + metric_table[out_even ^ s_vit_input_flip]);
            if (even0 > even1) {
                curr[state_even] = even0;
            } else {
                curr[state_even] = even1;
                decision_byte |= (uint8_t)(1U << (pair * 2U));
            }

            const int16_t odd0 = (int16_t)(path0 + metric_table[out_odd]);
            const int16_t odd1 = (int16_t)(path1 + metric_table[out_odd ^ s_vit_input_flip]);
            if (odd0 > odd1) {
                curr[state_odd] = odd0;
            } else {
                curr[state_odd] = odd1;
                decision_byte |= (uint8_t)(1U << (pair * 2U + 1U));
            }

            if (curr[state_even] > max_metric) {
                max_metric = curr[state_even];
            }
            if (curr[state_odd] > max_metric) {
                max_metric = curr[state_odd];
            }
        }
        decision_row[state_base >> 3U] = decision_byte;
        psk_runtime_yield(nstates, state_base);
    }

    if (max_metric > 12000) {
        for (uint16_t i = 0; i < nstates; ++i) {
            curr[i] = (int16_t)(curr[i] - max_metric);
        }
    }

    branch->vit_step = (uint8_t)((branch->vit_step + 1U) % VIT_MAX_TRACEBACK);
    branch->pair_count++;
    if ((branch->pair_count % rx->vit_chunksize) == 0U &&
        branch->pair_count > rx->vit_traceback) {
        *decoded = vit_traceback(rx, branch, nstates, curr, metric);
        return 1;
    }
    return 0;
}

static void decode_fec_pair_for_branch(psk_rtty_rx_t *rx, uint8_t branch_index, uint8_t current, uint8_t previous)
{
    fec_branch_state_t *branch = &s_fec_branch[branch_index];
    uint8_t pair[2] = {current, previous};
    if (rx->inlv_depth != 0U) {
        fec_deinterleave_pair(rx, branch, pair);
        const uint8_t tmp = pair[0];
        pair[0] = pair[1];
        pair[1] = tmp;
    } else {
        const uint8_t tmp = pair[0];
        pair[0] = pair[1];
        pair[1] = tmp;
    }

    uint8_t decoded = 0;
    int metric = 0;
    if (fec_decode_pair(rx, branch, pair[0], pair[1], &decoded, &metric)) {
        update_fec_metric(branch, metric);
        if (rx->fec_phase_search != 0U) {
            rx->debug_fec_metric[rx->fec_phase_candidate] = branch->metric_ave;
        } else {
            rx->debug_fec_metric[branch_index] = branch->metric_ave;
        }
        if (rx->fec_dual_branch) {
            update_fec_branch_lock(rx);
            if (rx->fec_locked_branch < 2U) {
                rx->debug_fec_selected = rx->fec_locked_branch;
            } else {
                rx->debug_fec_selected = fec_branch_selected(1U) ? 1U : 0U;
            }
        }
        push_payload_bits_from_fec(rx, branch, branch_index, decoded);
        update_fec_phase_search(rx, branch);
    }
}

static void push_fec_encoded_bit(psk_rtty_rx_t *rx, uint8_t bit)
{
    const uint8_t stream_parity = (uint8_t)(rx->fec_stream_bit_count & 1U);
    rx->fec_stream_bit_count++;

    if (rx->fec_dual_branch) {
        if (!rx->fec_have_last_encoded_bit) {
            rx->fec_last_encoded_bit = bit;
            rx->fec_have_last_encoded_bit = 1U;
            rx->fec_rxbitstate = 1U;
            return;
        }
        const uint8_t branch_index = rx->fec_rxbitstate ? 0U : 1U;
        if (rx->fec_locked_branch >= 2U || branch_index == rx->fec_locked_branch) {
            decode_fec_pair_for_branch(rx, branch_index, bit, rx->fec_last_encoded_bit);
        }
        rx->fec_last_encoded_bit = bit;
        rx->fec_rxbitstate ^= 1U;
        rx->fec_pair_count++;
        return;
    }

    if (rx->fec_phase_align_pending) {
        if (stream_parity != rx->fec_phase_candidate) {
            return;
        }
        rx->fec_phase_align_pending = 0U;
    }

    rx->fec_pair_buf[rx->fec_pair_len++] = bit;
    if (rx->fec_pair_len < 2U) {
        return;
    }

    const uint8_t previous = rx->fec_pair_buf[0];
    const uint8_t current = rx->fec_pair_buf[1];
    rx->fec_pair_len = 0;
    decode_fec_pair_for_branch(rx, 0U, current, previous);
    rx->fec_pair_count++;
}

static void push_rtty_bit(psk_rtty_rx_t *rx, int bit)
{
    if (rx->rtty_bit_index == 0) {
        if (bit == 0) {
            rx->rtty_shift_reg = 0;
            rx->rtty_bit_index = 1;
        }
        return;
    }
    if (rx->rtty_bit_index <= 5) {
        rx->rtty_shift_reg >>= 1;
        if (bit) {
            rx->rtty_shift_reg |= 0x10;
        }
        rx->rtty_bit_index++;
        return;
    }

    if (bit) {
        const uint8_t code = rx->rtty_shift_reg & 0x1f;
        if (code == 0x1b) {
            rx->rtty_figures = 1;
        } else if (code == 0x1f) {
            rx->rtty_figures = 0;
        } else {
            emit(rx, (char)s_rtty_table[code][rx->rtty_figures]);
        }
    }
    rx->rtty_bit_index = 0;
}

static void finish_symbol(psk_rtty_rx_t *rx)
{
    if (rx->samples_in_symbol == 0) {
        return;
    }

    const float phase = atan2f(rx->q_acc, rx->i_acc);
    float diff = phase - rx->prev_symbol_phase;
    while (diff > (float)M_PI) {
        diff -= TWO_PI;
    }
    while (diff < -(float)M_PI) {
        diff += TWO_PI;
    }
    rx->prev_symbol_phase = phase;

    if (rx->config.mode == PSK_RTTY_MODE_RTTY) {
        push_rtty_bit(rx, rx->i_acc > 0.0f ? 1 : 0);
    } else {
        const int bit = fabsf(diff) > ((float)M_PI * 0.5f) ? 0 : 1;
        push_varicode_bit(rx, bit);
    }

    rx->i_acc = 0.0f;
    rx->q_acc = 0.0f;
    rx->samples_in_symbol = 0;
}

static PSK_FAST_TEXT psk_rtty_complex_t fir_complex(const psk_rtty_complex_t *queue, int newest, int length, const float *coef)
{
    psk_rtty_complex_t acc = {0.0f, 0.0f};
    int pos = newest;
    for (int i = 0; i < length; ++i) {
        acc.x += queue[pos].x * coef[i];
        acc.y += queue[pos].y * coef[i];
        if (++pos >= length) {
            pos = 0;
        }
    }
    return acc;
}

static PSK_FAST_TEXT void calc_bit_filter(psk_rtty_rx_t *rx, uint8_t car, psk_rtty_complex_t sample)
{
    rx->chan_bit_queue[car][rx->chan_bit_fir_state[car]] = sample;
    rx->chan_bit_signal[car] = fir_complex(rx->chan_bit_queue[car], rx->chan_bit_fir_state[car],
                                            PSKCORE_BITFIR_LENGTH, s_bit_fir);
    if (rx->psk_family == PSK_FAMILY_8PSK) {
        rx->chan_freq_signal[car] = rx->chan_bit_signal[car];
    } else {
        rx->chan_freq_signal[car] = fir_complex(rx->chan_bit_queue[car], rx->chan_bit_fir_state[car],
                                                 PSKCORE_BITFIR_LENGTH, s_freq_fir);
    }
    if (--rx->chan_bit_fir_state[car] < 0) {
        rx->chan_bit_fir_state[car] = PSKCORE_BITFIR_LENGTH - 1;
    }
}

static PSK_FAST_TEXT void calc_channel_agc(psk_rtty_rx_t *rx)
{
    uint8_t active = rx->carriers;
    if (active == 0U || active > MAX_PSK_CARRIERS) {
        active = 1U;
    }

    float mag_sum = 0.0f;
    for (uint8_t car = 0; car < active; ++car) {
        mag_sum += sqrtf(rx->chan_freq_signal[car].x * rx->chan_freq_signal[car].x +
                         rx->chan_freq_signal[car].y * rx->chan_freq_signal[car].y);
    }
    const float mag = mag_sum / (float)active;

    if (mag > rx->agc_ave) {
        rx->agc_ave = (1.0f - 1.0f / 200.0f) * rx->agc_ave + (1.0f / 200.0f) * mag;
    } else {
        rx->agc_ave = (1.0f - 1.0f / 500.0f) * rx->agc_ave + (1.0f / 500.0f) * mag;
    }

    if (rx->agc_ave >= 1.0f) {
        for (uint8_t car = 0; car < active; ++car) {
            rx->chan_bit_signal[car].x /= rx->agc_ave;
            rx->chan_bit_signal[car].y /= rx->agc_ave;
            rx->chan_freq_signal[car].x /= rx->agc_ave;
            rx->chan_freq_signal[car].y /= rx->agc_ave;
        }
    }
}

static PSK_FAST_TEXT void calc_freq_error(psk_rtty_rx_t *rx, uint8_t car)
{
    if (!rx->afc_enabled || rx->carriers != 1U || rx->psk_family == PSK_FAMILY_8PSK) {
        rx->freq_error = 0.0f;
        rx->fperr_ave = 0.0f;
        rx->fferr_ave = 0.0f;
        return;
    }

    const float p_gn = 0.001f;
    const float i_gn = 1.5e-6f;
    const float p_cgn = 0.0004f;
    const float i_cgn = 3.0e-6f;
    const float wide_gn = 1.0f / 0.02f;
    const float wlp_k = 200.0f;

    float ferr = (rx->chan_freq_signal[car].x - rx->z2.x) * rx->z1.y -
                 (rx->chan_freq_signal[car].y - rx->z2.y) * rx->z1.x;
    rx->z2 = rx->z1;
    rx->z1 = rx->chan_freq_signal[car];

    if (ferr > 0.30f) {
        ferr = 0.30f;
    } else if (ferr < -0.30f) {
        ferr = -0.30f;
    }

    rx->fferr_ave = (1.0f - 1.0f / wlp_k) * rx->fferr_ave + (wide_gn / wlp_k) * ferr;
    if (rx->afc_timer > 0) {
        ferr = rx->fferr_ave;
        if (ferr > 0.3f || ferr < -0.3f) {
            rx->nco_phase_inc += ferr * i_cgn;
        }
        rx->freq_error = ferr * p_cgn;
    } else {
        ferr = (rx->fferr_ave * rx->fperr_ave > 0.0f) ? rx->fperr_ave : 0.0f;
        if (ferr > 0.3f || ferr < -0.3f) {
            rx->nco_phase_inc += ferr * i_gn;
        }
        rx->freq_error = ferr * p_gn;
    }

    if (rx->nco_phase_inc + rx->freq_error > rx->afc_max) {
        rx->nco_phase_inc = rx->afc_max;
        rx->freq_error = 0.0f;
    } else if (rx->nco_phase_inc + rx->freq_error < rx->afc_min) {
        rx->nco_phase_inc = rx->afc_min;
        rx->freq_error = 0.0f;
    }
}

static PSK_FAST_TEXT int symbol_sync(psk_rtty_rx_t *rx, float energy)
{
    int trigger = 0;
    int bit_pos = rx->bit_pos;
    const int timing_bins = rx->timing_bins > 0 ? rx->timing_bins : PSK_TIMING_MAX_BINS;

    if (bit_pos < timing_bins) {
        if (energy > 4.0f) {
            energy = 1.0f;
        }
        rx->sync_ave[bit_pos] = (1.0f - 1.0f / 82.0f) * rx->sync_ave[bit_pos] + (1.0f / 82.0f) * energy;
        trigger = (bit_pos == rx->pk_pos);

        if (bit_pos == rx->timing_half_pos) {
            if (rx->pk_pos != rx->new_pk_pos) {
                rx->debug_timing_adjusts++;
            }
            rx->pk_pos = rx->new_pk_pos;
        }
        bit_pos++;
    }

    rx->bit_phase_pos += rx->bit_phase_inc;
    if (rx->bit_phase_pos >= PSKCORE_TS) {
        rx->bit_phase_pos = fmodf(rx->bit_phase_pos, PSKCORE_TS);
        if (bit_pos == timing_bins - 1 && rx->pk_pos == timing_bins - 1) {
            trigger = 1;
        }
        bit_pos = 0;

        float max_energy = -1.0e10f;
        int max_pos = rx->new_pk_pos;
        for (int i = 0; i < timing_bins; ++i) {
            if (rx->sync_ave[i] > max_energy) {
                max_pos = i;
                max_energy = rx->sync_ave[i];
            }
        }
        rx->raw_pk_pos = max_pos;

        if (rx->sq_open) {
            int delta = max_pos - rx->pk_pos;
            const int half = timing_bins / 2;
            if (delta > half) {
                delta -= timing_bins;
            } else if (delta < -half) {
                delta += timing_bins;
            }
            if (delta > 1) {
                rx->new_pk_pos = (rx->pk_pos + 1) % timing_bins;
            } else if (delta < -1) {
                rx->new_pk_pos = (rx->pk_pos + timing_bins - 1) % timing_bins;
            } else {
                rx->new_pk_pos = max_pos;
            }
        } else {
            rx->new_pk_pos = max_pos;
        }
        rx->timing_half_pos = (rx->new_pk_pos + timing_bins / 2) % timing_bins;
        rx->last_pk_pos = rx->pk_pos;
    }

    rx->bit_pos = bit_pos;
    return trigger;
}

static PSK_FAST_TEXT void psk_channel_rx_feed(psk_rtty_rx_t *rx, const int16_t *samples, size_t count)
{
    if (rx->afc_timer > 0 && --rx->afc_timer <= 0) {
        rx->afc_timer = 0;
        rx->afc_max = rx->nco_phase_inc + rx->afc_limit;
        rx->afc_min = rx->nco_phase_inc - rx->afc_limit;
        if (rx->afc_min < 0.0f) {
            rx->afc_min = 0.0f;
        }
    }

    uint8_t active = rx->carriers;
    if (active == 0U || active > MAX_PSK_CARRIERS) {
        active = 1U;
    }

    const int decim = rx->second_decim > 0 ? rx->second_decim : 1;
    for (size_t n = 0; n < count; ++n) {
        const float sample = (float)samples[n];

        for (uint8_t car = 0; car < active; ++car) {
            rx->chan_mix_i_acc[car] += sample * rx->carrier_cos[car];
            rx->chan_mix_q_acc[car] -= sample * rx->carrier_sin[car];

            float rot_cos = rx->carrier_rot_cos[car];
            float rot_sin = rx->carrier_rot_sin[car];
            if (rx->carriers == 1U && !rx->decision_directed_afc) {
                const float phase_inc = rx->nco_phase_inc + rx->freq_error;
                rot_cos = cosf(phase_inc);
                rot_sin = sinf(phase_inc);
            }
            const float next_cos = rx->carrier_cos[car] * rot_cos - rx->carrier_sin[car] * rot_sin;
            const float next_sin = rx->carrier_sin[car] * rot_cos + rx->carrier_cos[car] * rot_sin;
            rx->carrier_cos[car] = next_cos;
            rx->carrier_sin[car] = next_sin;
        }

        if (++rx->chan_mix_decim_count < decim) {
            ++rx->sample_count;
            continue;
        }
        rx->chan_mix_decim_count = 0;

        const float scale = 1.0f / (float)decim;
        for (uint8_t car = 0; car < active; ++car) {
            psk_rtty_complex_t mixed = {
                rx->chan_mix_i_acc[car] * scale,
                rx->chan_mix_q_acc[car] * scale,
            };
            rx->chan_mix_i_acc[car] = 0.0f;
            rx->chan_mix_q_acc[car] = 0.0f;
            calc_bit_filter(rx, car, mixed);
        }

        calc_channel_agc(rx);
        if (active == 1U && !rx->decision_directed_afc) {
            calc_freq_error(rx, 0U);
        }

        if (symbol_sync(rx, channel_sync_energy(rx))) {
            finish_channel_psk_symbol(rx);
        }

        if ((++rx->sample_count & 0xff) == 0) {
            for (uint8_t car = 0; car < active; ++car) {
                const float mag = sqrtf(rx->carrier_cos[car] * rx->carrier_cos[car] +
                                        rx->carrier_sin[car] * rx->carrier_sin[car]);
                if (mag > 0.0f) {
                    rx->carrier_cos[car] /= mag;
                    rx->carrier_sin[car] /= mag;
                }
            }
        }
    }
}

static uint8_t phase_index(float angle, uint8_t count)
{
    if (angle < 0.0f) {
        angle += TWO_PI;
    }
    const float step = TWO_PI / (float)count;
    return (uint8_t)(((int)(angle / step + 0.5f)) & (count - 1U));
}

static uint8_t inverse_psk_symbol(const psk_rtty_rx_t *rx, float angle, float dot)
{
    if (rx->psk_family == PSK_FAMILY_BPSK) {
        return dot >= 0.0f ? 2U : 0U;
    }
    return psk_codec_symbol_from_phase_index((psk_mode_family_t)rx->psk_family,
                                             rx->fec_enabled, phase_index(angle,
                                                                          rx->psk_family ==
                                                                          PSK_FAMILY_QPSK ? 4U :
                                                                          8U));
}

static uint8_t bpsk_soft_bit(psk_rtty_rx_t *rx, float angle, float signal_amp)
{
    const float power = signal_amp * signal_amp;
    if (rx->bpsk_average_amp <= 0.0f) {
        rx->bpsk_average_amp = power;
    } else {
        rx->bpsk_average_amp = (49.0f * rx->bpsk_average_amp + power) / 50.0f;
    }

    float amplitude_penalty = 2.0f;
    if (power > 0.0f && rx->bpsk_average_amp > 0.0f) {
        const float ratio = power > rx->bpsk_average_amp ?
                            power / rx->bpsk_average_amp : rx->bpsk_average_amp / power;
        amplitude_penalty = sqrtf(ratio);
        if (amplitude_penalty < 1.0f) {
            amplitude_penalty = 1.0f;
        } else if (amplitude_penalty > 1000000.0f) {
            amplitude_penalty = 1000000.0f;
        }
    }

    float alpha = angle / (float)M_PI;
    if (alpha > 1.0f) {
        alpha = 2.0f - alpha;
    }
    const float phase_soft = 127.0f - 255.0f * alpha;
    float soft = phase_soft / (1.0f + amplitude_penalty * 0.5f) + 128.0f;
    if (soft < 0.0f) {
        soft = 0.0f;
    } else if (soft > 255.0f) {
        soft = 255.0f;
    }
    return (uint8_t)(soft + 0.5f);
}

/* Max-log soft demapper.  The previous code assigned one common quality to
 * all three bits and converted a whole 8PSK symbol to erasures near a decision
 * boundary.  8PSK bit reliabilities are not equal: retain the distance to the
 * nearest constellation point for each bit value so the Viterbi decoder can
 * still use the reliable bits of an uncertain symbol. */
static uint8_t psk8_soft_bit(float angle, int bit)
{
    const float step = TWO_PI / 8.0f;
    float best_zero = -2.0f;
    float best_one = -2.0f;

    for (uint8_t phase = 0U; phase < 8U; ++phase) {
        const float score = cosf(angle - step * (float)phase);
        if (s_gray_8psk_softbits[phase][bit] >= 128U) {
            if (score > best_one) {
                best_one = score;
            }
        } else if (score > best_zero) {
            best_zero = score;
        }
    }

    float soft = 128.0f + PSK8_SOFT_SCALE * (best_one - best_zero);
    if (soft < 0.0f) {
        soft = 0.0f;
    } else if (soft > 255.0f) {
        soft = 255.0f;
    }
    return (uint8_t)(soft + 0.5f);
}

static void push_8psk_soft_bits(psk_rtty_rx_t *rx, float angle)
{
    for (int bit = 2; bit >= 0; --bit) {
        const uint8_t soft = psk8_soft_bit(angle, bit);
        const int distance = (int)soft - 128;
        if (distance > -8 && distance < 8) {
            rx->debug_soft_ambiguous++;
        }
        push_fec_encoded_bit(rx, soft);
    }
}

static void process_raw_symbol(psk_rtty_rx_t *rx, uint8_t symbol, uint8_t phase_idx,
                               float angle, float signal_amp)
{
    rx->debug_symbols++;
    if (rx->warmup_symbols > 0) {
        rx->warmup_symbols--;
        refresh_lock_state(rx);
        return;
    }

    if (rx->fec_enabled && signal_amp <= 0.05f) {
        return;
    }

    if (rx->psk_family == PSK_FAMILY_BPSK && !rx->fec_enabled) {
        const int bit = symbol == 2U;
        rx->debug_bits++;
        bit ? rx->debug_ones++ : rx->debug_zeros++;
        push_varicode_bit(rx, bit);
        return;
    }

    if (rx->psk_family == PSK_FAMILY_BPSK) {
        const uint8_t soft = bpsk_soft_bit(rx, angle, signal_amp);
        const uint8_t bit = soft >= 128U ? 1U : 0U;
        rx->debug_bits++;
        bit ? rx->debug_ones++ : rx->debug_zeros++;
        push_fec_encoded_bit(rx, soft);
        return;
    }

    if (rx->psk_family == PSK_FAMILY_QPSK) {
        const uint8_t b0 = symbol & 1U;
        const uint8_t b1 = (symbol >> 1U) & 1U;
        rx->debug_bits += 2U;
        rx->debug_ones += b0 + b1;
        rx->debug_zeros += 2U - b0 - b1;
        push_fec_encoded_bit(rx, b0 ? 255U : 0U);
        push_fec_encoded_bit(rx, b1 ? 255U : 0U);
        return;
    }

    const uint8_t b0 = symbol & 1U;
    const uint8_t b1 = (symbol >> 1U) & 1U;
    const uint8_t b2 = (symbol >> 2U) & 1U;
    rx->debug_bits += 3U;
    rx->debug_ones += b0 + b1 + b2;
    rx->debug_zeros += 3U - b0 - b1 - b2;
    if (!rx->fec_enabled) {
        push_mfsk_varicode_bit(rx, b0);
        push_mfsk_varicode_bit(rx, b1);
        push_mfsk_varicode_bit(rx, b2);
        return;
    }
    if (rx->fec_punctured) {
        push_8psk_soft_bits(rx, angle);
        push_fec_encoded_bit(rx, FEC_PUNCTURE);
        return;
    }
    push_8psk_soft_bits(rx, angle);
}

static float circular_distance(float a, float b)
{
    float d = fabsf(a - b);
    if (d > (float)M_PI) {
        d = TWO_PI - d;
    }
    return d;
}

static void update_decision_directed_afc(psk_rtty_rx_t *rx, float angle, uint8_t phase_idx,
                                         uint8_t phase_states, int valid)
{
    if (!rx->decision_directed_afc || !rx->afc_enabled || rx->carriers != 1U || !valid ||
        rx->symbol_period_samples <= 0.0f || (!rx->sq_open && rx->debug_chars != 0U)) {
        return;
    }
    if (rx->debug_chars != 0U &&
        rx->sq_level < rx->sq_thresh + DD_AFC_TRACK_QUALITY_MARGIN) {
        return;
    }

    const float step = TWO_PI / (float)phase_states;
    float error = angle - step * (float)phase_idx;
    if (error > (float)M_PI) {
        error -= TWO_PI;
    } else if (error < -(float)M_PI) {
        error += TWO_PI;
    }
    const float max_error = phase_states == 4U ? QPSK_AFC_MAX_DECISION_ERROR :
                                                PSK8_AFC_MAX_DECISION_ERROR;
    if (fabsf(error) > step * max_error) {
        return;
    }

    const float residual_per_sample = error / rx->symbol_period_samples;
    rx->q_freq_error = 0.9f * rx->q_freq_error + 0.1f * residual_per_sample;
    const float gain = rx->debug_chars == 0U ? DD_AFC_ACQUIRE_GAIN : DD_AFC_TRACK_GAIN;
    rx->freq_error += gain * residual_per_sample;
    if (rx->freq_error > rx->afc_limit) {
        rx->freq_error = rx->afc_limit;
    } else if (rx->freq_error < -rx->afc_limit) {
        rx->freq_error = -rx->afc_limit;
    }
    const float phase_inc = rx->nco_phase_inc + rx->freq_error;
    rx->carrier_rot_cos[0] = cosf(phase_inc);
    rx->carrier_rot_sin[0] = sinf(phase_inc);
}

/* Differential 8PSK removes the data rotation when the filtered complex
 * symbol is raised to the eighth power.  This gives a coarse carrier estimate
 * before hard constellation decisions are trustworthy, which is essential at
 * the 1200F capture boundary. */
static void update_8psk_nondat_afc(psk_rtty_rx_t *rx, float i, float q, int valid)
{
    if (!rx->afc_enabled || rx->carriers != 1U || rx->psk_family != PSK_FAMILY_8PSK || !valid ||
        !rx->sq_open || rx->sq_stable_symbols < SQ_EMIT_STABLE_SYMBOLS ||
        rx->symbol_period_samples <= 0.0f) {
        return;
    }

    const float power = i * i + q * q;
    if (power <= 0.0025f) {
        rx->psk8_eighth_valid = 0U;
        return;
    }
    const float inv_mag = 1.0f / sqrtf(power);
    const float ux = i * inv_mag;
    const float uy = q * inv_mag;
    const psk_rtty_complex_t z2 = {ux * ux - uy * uy, 2.0f * ux * uy};
    const psk_rtty_complex_t z4 = {z2.x * z2.x - z2.y * z2.y, 2.0f * z2.x * z2.y};
    const psk_rtty_complex_t z8 = {z4.x * z4.x - z4.y * z4.y, 2.0f * z4.x * z4.y};

    if (rx->psk8_eighth_valid) {
        const float dot = rx->psk8_eighth_prev.x * z8.x + rx->psk8_eighth_prev.y * z8.y;
        const float cross = rx->psk8_eighth_prev.x * z8.y - rx->psk8_eighth_prev.y * z8.x;
        const float estimate = atan2f(cross, dot) / (8.0f * rx->symbol_period_samples);
        if (fabsf(estimate) <= rx->afc_limit) {
            const float filter = rx->debug_chars == 0U ? 0.08f : 0.02f;
            const float gain = rx->debug_chars == 0U ? PSK8_NDA_ACQUIRE_GAIN :
                                                       PSK8_NDA_TRACK_GAIN;
            rx->psk8_coarse_freq += filter * (estimate - rx->psk8_coarse_freq);
            rx->freq_error += gain * (rx->psk8_coarse_freq - rx->freq_error);
            if (rx->freq_error > rx->afc_limit) {
                rx->freq_error = rx->afc_limit;
            } else if (rx->freq_error < -rx->afc_limit) {
                rx->freq_error = -rx->afc_limit;
            }
            const float phase_inc = rx->nco_phase_inc + rx->freq_error;
            rx->carrier_rot_cos[0] = cosf(phase_inc);
            rx->carrier_rot_sin[0] = sinf(phase_inc);
            rx->debug_coarse_updates++;
        }
    }
    rx->psk8_eighth_prev = z8;
    rx->psk8_eighth_valid = 1U;
}

static uint8_t qpsk_soft_bit(float angle, uint8_t bit_mask)
{
    const float step = (float)M_PI / 2.0f;
    float best0 = TWO_PI;
    float best1 = TWO_PI;

    for (uint8_t idx = 0; idx < 4U; ++idx) {
        const float ideal = step * (float)idx;
        const float dist = circular_distance(angle, ideal);
        if (psk_codec_symbol_from_phase_index(PSK_MODE_FAMILY_QPSK, 0U, idx) & bit_mask) {
            if (dist < best1) {
                best1 = dist;
            }
        } else if (dist < best0) {
            best0 = dist;
        }
    }

    float soft = 128.0f + 127.0f * ((best0 - best1) / step);
    if (soft < 0.0f) {
        soft = 0.0f;
    } else if (soft > 255.0f) {
        soft = 255.0f;
    }
    return (uint8_t)(soft + 0.5f);
}

static void process_qpsk_symbol(psk_rtty_rx_t *rx, float angle, int signal_valid)
{
    rx->debug_symbols++;
    if (rx->warmup_symbols > 0) {
        rx->warmup_symbols--;
        refresh_lock_state(rx);
        return;
    }

    if (!signal_valid) {
        return;
    }

    const uint8_t b0 = qpsk_soft_bit(angle, 1U);
    const uint8_t b1 = qpsk_soft_bit(angle, 2U);
    rx->debug_bits += 2U;
    rx->debug_ones += (b0 >= 128U) + (b1 >= 128U);
    rx->debug_zeros += (b0 < 128U) + (b1 < 128U);
    push_fec_encoded_bit(rx, b0);
    push_fec_encoded_bit(rx, b1);
}

static float channel_sync_energy(const psk_rtty_rx_t *rx)
{
    uint8_t active = rx->carriers;
    if (active == 0U || active > MAX_PSK_CARRIERS) {
        active = 1U;
    }

    float energy = 0.0f;
    for (uint8_t car = 0; car < active; ++car) {
        const float i = rx->chan_bit_signal[car].x;
        const float q = rx->chan_bit_signal[car].y;
        energy += i * i + q * q;
    }

    return energy / (float)active;
}

static float nearest_phase_distance(float angle, uint8_t states)
{
    const float step = TWO_PI / (float)states;
    float nearest = roundf(angle / step) * step;
    float dist = fabsf(angle - nearest);
    if (dist > (float)M_PI) {
        dist = TWO_PI - dist;
    }
    return dist;
}

static void update_quality_squelch(psk_rtty_rx_t *rx, float dist, float step, int valid)
{
    const int was_open = rx->sq_open;

    if (!valid || rx->agc_ave <= 1.0f) {
        rx->dev_ave = 100.0f;
        rx->sq_level = 0;
    } else {
        float dev = 100.0f * dist / (step * 0.5f);
        if (dev > 100.0f) {
            dev = 100.0f;
        }
        if (dev < rx->dev_ave) {
            rx->dev_ave = (1.0f - 1.0f / (float)rx->squelch_speed) * rx->dev_ave +
                          (1.0f / (float)rx->squelch_speed) * dev;
        } else {
            const float slow = (float)rx->squelch_speed * 2.0f;
            rx->dev_ave = (1.0f - 1.0f / slow) * rx->dev_ave + (1.0f / slow) * dev;
        }
        rx->sq_level = 100 - (int)(rx->dev_ave + 0.5f);
        if (rx->sq_level < 0) {
            rx->sq_level = 0;
        } else if (rx->sq_level > 100) {
            rx->sq_level = 100;
        }
    }

    if (!rx->squelch_enabled) {
        rx->sq_open = 1;
    } else if (was_open) {
        rx->sq_open = rx->sq_level >= (rx->sq_thresh - SQL_OPEN_HYSTERESIS);
    } else {
        rx->sq_open = rx->sq_level >= rx->sq_thresh;
    }

    if (rx->sq_open) {
        if (!was_open) {
            rx->bit_acc = 0;
            rx->last_bit_zero = 0;
            rx->sq_stable_symbols = 0;
        } else if (rx->sq_stable_symbols < SQ_EMIT_STABLE_SYMBOLS) {
            rx->sq_stable_symbols++;
        }
    } else {
        rx->bit_acc = 0;
        rx->last_bit_zero = 0;
        rx->sq_stable_symbols = 0;
        if (was_open) {
            reset_fec_acquisition(rx);
        }
    }
    rx->sq_was_open = rx->sq_open;
    refresh_lock_state(rx);
}

static PSK_FAST_TEXT void finish_channel_psk_symbol(psk_rtty_rx_t *rx)
{
    uint8_t active = rx->carriers;
    if (active == 0U || active > MAX_PSK_CARRIERS) {
        active = 1U;
    }

    float dist_sum = 0.0f;
    uint8_t dist_count = 0U;
    const uint8_t phase_states = rx->psk_family == PSK_FAMILY_QPSK ? 4U :
                                 (rx->psk_family == PSK_FAMILY_8PSK ? 8U : 2U);
    const float phase_step = TWO_PI / (float)phase_states;

    for (uint8_t car = 0; car < active; ++car) {
        const float i = rx->chan_bit_signal[car].x;
        const float q = rx->chan_bit_signal[car].y;
        const float mag = sqrtf(i * i + q * q);

        if (rx->carrier_has_prev[car]) {
            if (rx->psk_family == PSK_FAMILY_8PSK && active == 1U) {
                update_8psk_nondat_afc(rx, i, q, mag > 0.05f);
            }
            const float pi = rx->carrier_i_prev[car];
            const float pq = rx->carrier_q_prev[car];
            const float dot = pi * i + pq * q;
            const float cross = pi * q - pq * i;
            float angle = atan2f(cross, dot);
            if (angle < 0.0f) {
                angle += TWO_PI;
            }
            if (mag > 0.05f) {
                dist_sum += nearest_phase_distance(angle, phase_states);
                dist_count++;
            }
            const uint8_t phase_idx = phase_index(angle, phase_states);
            if (rx->decision_directed_afc && active == 1U &&
                rx->psk_family != PSK_FAMILY_8PSK) {
                update_decision_directed_afc(rx, angle, phase_idx, phase_states, mag > 0.05f);
            }
            if (rx->psk_family == PSK_FAMILY_QPSK) {
                process_qpsk_symbol(rx, angle, mag > 0.05f);
            } else {
                process_raw_symbol(rx, inverse_psk_symbol(rx, angle, dot), phase_idx, angle, mag);
            }
        }

        rx->carrier_i_prev[car] = i;
        rx->carrier_q_prev[car] = q;
        rx->carrier_has_prev[car] = 1U;
    }

    const float avg_dist = dist_count ? (dist_sum / (float)dist_count) : phase_step;
    update_quality_squelch(rx, avg_dist, phase_step, dist_count > 0U);
}


static void psk_pipeline_rx_feed(psk_rtty_rx_t *rx, const int16_t *samples, size_t count)
{
    psk_channel_rx_feed(rx, samples, count);
}

static void rtty_rx_feed(psk_rtty_rx_t *rx, const int16_t *samples, size_t count)
{
    const float phase_inc = TWO_PI * (float)rx->config.rx_tone_hz / (float)rx->config.sample_rate_hz;
    for (size_t n = 0; n < count; ++n) {
        const float s = (float)samples[n] / 32768.0f;
        rx->i_acc += s * cosf(rx->nco_phase);
        rx->q_acc += s * sinf(rx->nco_phase);
        rx->samples_in_symbol++;
        rx->nco_phase += phase_inc;
        if (rx->nco_phase >= TWO_PI) {
            rx->nco_phase -= TWO_PI;
        }

        rx->symbol_clock += 1.0f;
        if (rx->symbol_clock >= rx->symbol_period_samples) {
            rx->symbol_clock -= rx->symbol_period_samples;
            finish_symbol(rx);
        }
    }
}

void psk_rtty_rx_feed(psk_rtty_rx_t *rx, const int16_t *samples, size_t count)
{
    if (rx->config.mode == PSK_RTTY_MODE_RTTY) {
        rtty_rx_feed(rx, samples, count);
        return;
    }

    psk_pipeline_rx_feed(rx, samples, count);
}
