/**
 * test_downsample.c — Unity tests for pcm16_downsample()
 *
 * Run on-device:  cd test_app && idf.py flash monitor
 * All tests use no hardware — pure C logic only.
 */

#include "unity.h"
#include "audio/downsample.h"
#include <string.h>
#include <stdint.h>

/* ── helpers ────────────────────────────────────────────────── */

static void fill_ramp(int16_t *buf, int n, int16_t start, int16_t step)
{
    for (int i = 0; i < n; i++) buf[i] = (int16_t)(start + i * step);
}

/* ── ratio=1: identity ──────────────────────────────────────── */

TEST_CASE("downsample ratio=1 is identity", "[downsample]")
{
    int16_t buf[8] = {100, 200, 300, 400, 500, 600, 700, 800};
    int16_t orig[8];
    memcpy(orig, buf, sizeof(buf));

    size_t out = pcm16_downsample(buf, sizeof(buf), 1);

    TEST_ASSERT_EQUAL(sizeof(buf), out);
    TEST_ASSERT_EQUAL_INT16_ARRAY(orig, buf, 8);
}

/* ── ratio=0 / negative: treated as identity ────────────────── */

TEST_CASE("downsample ratio<=0 is identity", "[downsample]")
{
    int16_t buf[4] = {1, 2, 3, 4};
    size_t out = pcm16_downsample(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL(sizeof(buf), out);

    out = pcm16_downsample(buf, sizeof(buf), -1);
    TEST_ASSERT_EQUAL(sizeof(buf), out);
}

/* ── ratio=3 (48 kHz → 16 kHz): averaging ──────────────────── */

TEST_CASE("downsample 48kHz->16kHz averages 3 samples", "[downsample]")
{
    /* 6 samples → 2 output samples */
    int16_t buf[6] = {100, 200, 300,   /* avg = 200 */
                      400, 500, 600 }; /* avg = 500 */
    size_t out = pcm16_downsample(buf, sizeof(buf), 3);

    TEST_ASSERT_EQUAL(4, out);           /* 2 samples × 2 bytes */
    TEST_ASSERT_EQUAL_INT16(200, buf[0]);
    TEST_ASSERT_EQUAL_INT16(500, buf[1]);
}

/* ── ratio=2 (32 kHz → 16 kHz) ─────────────────────────────── */

TEST_CASE("downsample ratio=2 averages pairs", "[downsample]")
{
    int16_t buf[8] = {0, 4, 10, 6, -4, 4, 100, 100};
    size_t out = pcm16_downsample(buf, sizeof(buf), 2);

    TEST_ASSERT_EQUAL(8, out);
    TEST_ASSERT_EQUAL_INT16(2,   buf[0]);   /* (0+4)/2   */
    TEST_ASSERT_EQUAL_INT16(8,   buf[1]);   /* (10+6)/2  */
    TEST_ASSERT_EQUAL_INT16(0,   buf[2]);   /* (-4+4)/2  */
    TEST_ASSERT_EQUAL_INT16(100, buf[3]);   /* (100+100)/2 */
}

/* ── truncation: leftover samples discarded ─────────────────── */

TEST_CASE("downsample discards trailing partial group", "[downsample]")
{
    /* 7 samples with ratio=3 → 2 complete groups, 1 leftover discarded */
    int16_t buf[7] = {1, 1, 1, 2, 2, 2, 99};
    size_t out = pcm16_downsample(buf, sizeof(buf), 3);

    TEST_ASSERT_EQUAL(4, out);          /* only 2 output samples */
    TEST_ASSERT_EQUAL_INT16(1, buf[0]);
    TEST_ASSERT_EQUAL_INT16(2, buf[1]);
}

/* ── output bytes = input / ratio (rounded down) ────────────── */

TEST_CASE("downsample output size is input/ratio rounded down", "[downsample]")
{
    /* 256 bytes = 128 samples; ratio=3 → 42 groups of 3 = 42 samples = 84 bytes
     * (remaining 128 - 126 = 2 samples discarded) */
    int16_t buf[128];
    fill_ramp(buf, 128, 0, 1);
    size_t out = pcm16_downsample(buf, 256, 3);
    TEST_ASSERT_EQUAL(42 * 2, out);
}

/* ── sign-correct averaging (negative values) ───────────────── */

TEST_CASE("downsample averages negative samples correctly", "[downsample]")
{
    int16_t buf[4] = {-100, -200, -60, -40};
    size_t out = pcm16_downsample(buf, sizeof(buf), 2);

    TEST_ASSERT_EQUAL(4, out);
    TEST_ASSERT_EQUAL_INT16(-150, buf[0]);  /* (-100 + -200)/2 */
    TEST_ASSERT_EQUAL_INT16(-50,  buf[1]);  /* (-60  +  -40)/2 */
}

/* ── in-place safety: output overlaps input at start ────────── */

TEST_CASE("downsample is safe in-place", "[downsample]")
{
    int16_t buf[9] = {3, 3, 3, 6, 6, 6, 9, 9, 9};
    size_t out = pcm16_downsample(buf, sizeof(buf), 3);

    TEST_ASSERT_EQUAL(6, out);
    TEST_ASSERT_EQUAL_INT16(3, buf[0]);
    TEST_ASSERT_EQUAL_INT16(6, buf[1]);
    TEST_ASSERT_EQUAL_INT16(9, buf[2]);
}
