#include <stdio.h>
#include <string.h>
#include "ap580_playout.h"

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #expr); ++failures; \
} } while (0)

static void check_rate(uint32_t rate, uint32_t start)
{
    ap580_playout_t playout;
    uint8_t frame[16] = {0x59, 0x4f, 1, 7, 0x22};
    uint8_t out[16];
    ap580_playout_reset(&playout, rate, start);
    CHECK(ap580_playout_next(&playout, NULL, start, out) == AP580_PLAYOUT_NONE);
    CHECK(ap580_playout_next(&playout, frame, start, out) == AP580_PLAYOUT_VOICE);
    CHECK(memcmp(frame, out, sizeof(frame)) == 0);
    CHECK(!ap580_playout_due(&playout, playout.next_tick - 1U));
    const uint32_t period = ap580_frame_period_half_ms(rate);
    CHECK(playout.next_tick - start == period / 2U);
    CHECK(ap580_playout_next(&playout, NULL, playout.next_tick, out) == AP580_PLAYOUT_VOICE);
    CHECK(playout.next_tick - start == period);
    CHECK(ap580_playout_next(&playout, NULL, playout.next_tick, out) == AP580_PLAYOUT_VOICE);
    CHECK(ap580_playout_next(&playout, NULL, playout.next_tick, out) == AP580_PLAYOUT_STOP);
    CHECK(ap580_playout_next(&playout, NULL, playout.next_tick + 10000U, out) == AP580_PLAYOUT_NONE);

    frame[4] = 0x44;
    const uint32_t resumed = playout.next_tick + 20000U;
    CHECK(ap580_playout_next(&playout, frame, resumed, out) == AP580_PLAYOUT_VOICE);
    CHECK(memcmp(frame, out, sizeof(frame)) == 0);
    CHECK(!ap580_playout_due(&playout, resumed));

    /* Rate switches cannot conceal a new session with an old codec frame. */
    ap580_playout_reset(&playout, AP580_CODEC_RATE_1200, resumed);
    CHECK(ap580_playout_next(&playout, NULL, resumed, out) == AP580_PLAYOUT_NONE);
    CHECK(playout.active == 0U);
    CHECK(ap580_playout_next(&playout, frame, resumed, out) == AP580_PLAYOUT_VOICE);
    CHECK(playout.next_tick == resumed + 67U);
}

int main(void)
{
    const uint32_t rates[] = {600U, 1200U, 2400U};
    for (unsigned i = 0U; i < 3U; ++i) {
        check_rate(rates[i], 100U);
        check_rate(rates[i], UINT32_MAX - 10U);
    }
    printf("AP580 playout: rates=3, wraparound, bounded repeats, resume, rate reset; failures=%u\n",
           failures);
    return failures != 0U;
}
