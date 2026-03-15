#pragma once
/**
 * downsample.h — integer decimation for UAC microphone PCM streams.
 *
 * Reduces 16-bit mono PCM from an arbitrary rate to a target by averaging
 * `ratio` consecutive samples into one.  Only exact integer ratios are
 * supported (e.g. 48 kHz → 16 kHz, ratio = 3).
 *
 * The conversion is done in-place: `buf` is read as `in_bytes` bytes and
 * written as `(in_bytes / ratio)` rounded-down bytes.  Returns the number
 * of output bytes.
 *
 * If ratio <= 1 the buffer is unchanged and `in_bytes` is returned as-is.
 *
 * This header is intentionally free of ESP-IDF dependencies so it can be
 * included in host-side Unity unit tests without stubs.
 */

#include <stddef.h>
#include <stdint.h>

static inline size_t pcm16_downsample(void *buf, size_t in_bytes, int ratio)
{
    if (ratio <= 1 || in_bytes < 2) return in_bytes;

    int16_t *in  = (int16_t *)buf;
    int16_t *out = (int16_t *)buf;
    size_t   in_samples  = in_bytes / 2;
    size_t   out_samples = 0;

    for (size_t i = 0; i + (size_t)ratio <= in_samples; i += (size_t)ratio) {
        int32_t sum = 0;
        for (int d = 0; d < ratio; d++) sum += in[i + d];
        out[out_samples++] = (int16_t)(sum / ratio);
    }
    return out_samples * 2;
}

/**
 * pcm16_stereo_to_mono — convert interleaved stereo to mono in-place.
 *
 * Averages each (L, R) pair into a single mono sample.  Returns the number
 * of output bytes (= in_bytes / 2).  If in_bytes < 4 the buffer is unchanged.
 */
static inline size_t pcm16_stereo_to_mono(void *buf, size_t in_bytes)
{
    if (in_bytes < 4) return in_bytes;

    int16_t *s = (int16_t *)buf;
    size_t   frames     = in_bytes / 4;   /* 4 bytes per stereo frame */
    size_t   out_samples = 0;

    for (size_t i = 0; i < frames; i++) {
        int32_t l = s[i * 2];
        int32_t r = s[i * 2 + 1];
        s[out_samples++] = (int16_t)((l + r) / 2);
    }
    return out_samples * 2;
}
