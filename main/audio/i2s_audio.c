#include "i2s_audio.h"
#include "audio/wake_word.h"
#include "langoustine_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "i2s_audio";

/* Runtime volume: 0=mute, 128=50% (-6dB), 255=100%. Persisted to NVS. */
static volatile uint8_t s_volume = LANG_AUDIO_VOLUME;

/* Async playback cancel flag — checked in the write loop */
static volatile bool s_play_cancel = false;

/* Async playback task state */
static TaskHandle_t     s_play_task    = NULL;
static const uint8_t   *s_play_wav     = NULL;
static size_t           s_play_wav_len = 0;


void i2s_audio_set_volume(uint8_t vol)
{
    s_volume = vol;
    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_AUDIO, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, LANG_NVS_KEY_VOLUME, vol);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Volume set to %u/255 (%u%%)", vol, (unsigned)(vol * 100u / 255u));
}

uint8_t i2s_audio_get_volume(void)
{
    return s_volume;
}

/* I2S TX/RX channel handles (I2S_NUM_0, master, full-duplex) */
static i2s_chan_handle_t s_tx_handle  = NULL;
static i2s_chan_handle_t s_rx_handle  = NULL;
static bool             s_rx_enabled = false;  /* true after RX init+enable */

/* Track current I2S configuration to avoid unnecessary reconfigures */
static uint32_t s_current_sample_rate = 0;
static uint32_t s_current_bits        = 0;

/* ── WAV header parsing ─────────────────────────────────────────── */

/*
 * Minimal WAV/RIFF parser.  Handles non-standard chunk ordering
 * by scanning for the "fmt " and "data" sub-chunks rather than
 * assuming a fixed 44-byte layout.
 */
typedef struct {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t num_channels;
    size_t   pcm_offset;  /* byte offset into wav_data[] where PCM begins */
    size_t   pcm_len;     /* byte length of PCM payload */
} wav_info_t;

static esp_err_t parse_wav(const uint8_t *buf, size_t buf_len, wav_info_t *out)
{
    if (!buf || buf_len < 44 || !out) return ESP_ERR_INVALID_ARG;

    /* Check RIFF + WAVE magic */
    if (memcmp(buf,     "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a RIFF/WAVE file");
        return ESP_ERR_INVALID_ARG;
    }

    bool found_fmt  = false;
    bool found_data = false;
    size_t pos = 12;  /* skip "RIFF" + size + "WAVE" */

    while (pos + 8 <= buf_len) {
        /* Each chunk: 4-byte ID + 4-byte little-endian size */
        uint32_t chunk_size =
            (uint32_t)buf[pos+4]        |
            ((uint32_t)buf[pos+5] << 8) |
            ((uint32_t)buf[pos+6] << 16)|
            ((uint32_t)buf[pos+7] << 24);

        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            if (pos + 8 + 16 > buf_len) break;
            /* audio format (offset 8): 1 = PCM */
            uint16_t fmt = (uint16_t)buf[pos+8] | ((uint16_t)buf[pos+9] << 8);
            if (fmt != 1) {
                ESP_LOGW(TAG, "Unsupported WAV format: %u (only PCM=1 supported)", fmt);
                return ESP_ERR_INVALID_ARG;
            }
            out->num_channels    = (uint16_t)buf[pos+10] | ((uint16_t)buf[pos+11] << 8);
            out->sample_rate     = (uint32_t)buf[pos+12] |
                                   ((uint32_t)buf[pos+13] << 8) |
                                   ((uint32_t)buf[pos+14] << 16) |
                                   ((uint32_t)buf[pos+15] << 24);
            out->bits_per_sample = (uint16_t)buf[pos+22] | ((uint16_t)buf[pos+23] << 8);
            found_fmt = true;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            out->pcm_offset = pos + 8;
            out->pcm_len    = chunk_size;
            /* Clamp to actual buffer. Also handles 0xFFFFFFFF "unspecified"
             * size (streaming WAV) which overflows a 32-bit addition. */
            if (out->pcm_offset >= buf_len ||
                out->pcm_len > buf_len - out->pcm_offset) {
                out->pcm_len = buf_len - out->pcm_offset;
            }
            found_data = true;
        }

        if (found_fmt && found_data) break;

        /* Advance to next chunk (size is always even-padded in WAV) */
        pos += 8 + chunk_size + (chunk_size & 1);
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG, "WAV missing fmt or data chunk");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "WAV: %u Hz, %u-bit, %u ch, PCM offset=%u len=%u",
             (unsigned)out->sample_rate, (unsigned)out->bits_per_sample,
             (unsigned)out->num_channels,
             (unsigned)out->pcm_offset, (unsigned)out->pcm_len);
    return ESP_OK;
}

/* ── I2S channel (re)configuration ─────────────────────────────── */

/*
 * i2s_configure() handles both the first-time init and all subsequent
 * sample-rate / bit-depth changes.
 *
 * ESP-IDF v5.x rule:
 *   - i2s_channel_init_std_mode()  — must be called exactly ONCE per channel
 *     (after i2s_new_channel, before first enable).
 *   - i2s_channel_reconfig_std_clock() / _slot() — used for every subsequent
 *     change; channel must be disabled first.
 */
static esp_err_t i2s_configure(uint32_t sample_rate, i2s_data_bit_width_t bits)
{
    esp_err_t ret;

    if (s_current_sample_rate != 0) {
        /* Full-duplex: RX shares the clock — disable before reconfiguring */
        if (s_rx_enabled) i2s_channel_disable(s_rx_handle);
        i2s_channel_disable(s_tx_handle);
    }

    if (s_current_sample_rate == 0) {
        /* ── First-time initialisation ── */
        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = LANG_I2S_BCLK,
                .ws   = LANG_I2S_LRCLK,
                .dout = LANG_I2S_DOUT,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv   = false,
                },
            },
        };
        /* INMP441 requires 64 BCLK per frame (32-bit slots).
         * I2S_SLOT_BIT_WIDTH_AUTO defaults to data_bit_width (16),
         * generating only 32 BCLK → INMP441 underclocked.
         * Force 32-bit slots so BCLK = 2*32*Fs (1.024 MHz @ 16kHz).
         * MAX98357A works fine with 32-bit slots (ignores padding). */
        std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        std_cfg.slot_cfg.ws_width      = 32;  /* WS high/low = 32 BCLK each */
        ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* ── Reconfiguration (channel already initialised) ── */
        i2s_std_clk_config_t  clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO);
        slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        slot_cfg.ws_width       = 32;

        ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_reconfig_std_clock failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ret = i2s_channel_reconfig_std_slot(s_tx_handle, &slot_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_reconfig_std_slot failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Re-enable RX after clock reconfiguration (skip on first-time TX init) */
    if (s_rx_enabled) {
        ret = i2s_channel_enable(s_rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_enable (RX) failed: %s", esp_err_to_name(ret));
        }
    }

    s_current_sample_rate = sample_rate;
    s_current_bits        = (uint32_t)bits;
    ESP_LOGI(TAG, "I2S configured: %u Hz, %u-bit", (unsigned)sample_rate, (unsigned)bits);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t i2s_audio_init(void)
{
    if (s_tx_handle) {
        ESP_LOGW(TAG, "i2s_audio_init called more than once — ignoring");
        return ESP_OK;
    }

    /* Create full-duplex TX+RX channel pair on I2S_NUM_0.
     * Both channels share BCLK and LRCLK (MAX98357A + INMP441 use the same lines). */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initial TX configuration: 16kHz, 16-bit mono (matches most TTS output) */
    ret = i2s_configure(16000, I2S_DATA_BIT_WIDTH_16BIT);
    if (ret != ESP_OK) {
        i2s_del_channel(s_tx_handle);
        i2s_del_channel(s_rx_handle);
        s_tx_handle = NULL;
        s_rx_handle = NULL;
        return ret;
    }

    /* RX channel: INMP441 microphone, 16kHz mono.
     * INMP441 uses I2S Philips standard (1-bit delay after WS edge).
     * INMP441 always outputs 24-bit data left-justified in 32-bit slots.
     * We MUST read 32-bit samples (data_bit_width=32) to get the full slot.
     * i2s_audio_read() post-processes: right-shift 16 → int16_t for callers.
     * BCLK/WS set to I2S_GPIO_UNUSED because they are already claimed by TX. */
    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(LANG_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,   /* shared with TX */
            .ws   = I2S_GPIO_UNUSED,   /* shared with TX */
            .dout = I2S_GPIO_UNUSED,
            .din  = LANG_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    rx_cfg.slot_cfg.ws_width       = 32;
    ret = i2s_channel_init_std_mode(s_rx_handle, &rx_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode (RX) failed: %s", esp_err_to_name(ret));
        /* Non-fatal: TX still works for speaker output */
    } else {
        ret = i2s_channel_enable(s_rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable (RX) failed: %s", esp_err_to_name(ret));
        } else {
            s_rx_enabled = true;
            ESP_LOGI(TAG, "I2S RX (mic) enabled on GPIO %d", LANG_I2S_DIN);
        }
    }

    /* Load persisted volume from NVS (falls back to LANG_AUDIO_VOLUME if not set) */
    {
        nvs_handle_t nvs;
        if (nvs_open(LANG_NVS_AUDIO, NVS_READONLY, &nvs) == ESP_OK) {
            uint8_t stored;
            if (nvs_get_u8(nvs, LANG_NVS_KEY_VOLUME, &stored) == ESP_OK) {
                s_volume = stored;
            }
            nvs_close(nvs);
        }
    }

    ESP_LOGI(TAG, "I2S audio init OK (BCLK=%d LRCLK=%d DOUT=%d DIN=%d vol=%u/255)",
             LANG_I2S_BCLK, LANG_I2S_LRCLK, LANG_I2S_DOUT, LANG_I2S_DIN, s_volume);

#if LANG_AMP_SD_GPIO >= 0
    /* Configure amp shutdown pin — output, start low (amp off) */
    gpio_reset_pin(LANG_AMP_SD_GPIO);
    gpio_set_direction(LANG_AMP_SD_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LANG_AMP_SD_GPIO, 0);
    ESP_LOGI(TAG, "Amp SD pin GPIO%d configured (amp off)", LANG_AMP_SD_GPIO);
#endif

    return ESP_OK;
}

esp_err_t i2s_audio_play_wav(const uint8_t *wav_data, size_t len)
{
    if (!s_tx_handle) {
        ESP_LOGE(TAG, "I2S not initialised — call i2s_audio_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wav_data || len == 0) return ESP_ERR_INVALID_ARG;

    /* Parse WAV header */
    wav_info_t info = {0};
    esp_err_t ret = parse_wav(wav_data, len, &info);
    if (ret != ESP_OK) return ret;

    if (info.pcm_len == 0) {
        ESP_LOGW(TAG, "WAV data chunk is empty — nothing to play");
        return ESP_OK;
    }

    /* Map bit depth to IDF enum */
    i2s_data_bit_width_t bits;
    switch (info.bits_per_sample) {
        case 8:  bits = I2S_DATA_BIT_WIDTH_8BIT;  break;
        case 16: bits = I2S_DATA_BIT_WIDTH_16BIT; break;
        case 24: bits = I2S_DATA_BIT_WIDTH_24BIT; break;
        case 32: bits = I2S_DATA_BIT_WIDTH_32BIT; break;
        default:
            ESP_LOGE(TAG, "Unsupported bit depth: %u", (unsigned)info.bits_per_sample);
            return ESP_ERR_INVALID_ARG;
    }

    /* Suspend wake word feed task before reconfiguring I2S clock.
     * The feed task calls i2s_channel_read() which races with
     * i2s_channel_disable/enable during reconfiguration → permanent RX failure.
     * Suspend BEFORE reconfig, resume AFTER playback + restore completes. */
    bool ww_was_running = wake_word_is_running();
    bool ww_suspended = false;
    if (ww_was_running &&
        (info.sample_rate != s_current_sample_rate ||
         (uint32_t)bits   != s_current_bits)) {
        wake_word_suspend();
        ww_suspended = true;
    }

    /* Reconfigure I2S only if sample rate or bit depth changed.
     * WiFi PS is disabled globally (WIFI_PS_NONE) so no per-playback transitions needed. */
    if (info.sample_rate != s_current_sample_rate ||
        (uint32_t)bits    != s_current_bits) {
        ret = i2s_configure(info.sample_rate, bits);
        if (ret != ESP_OK) {
            if (ww_suspended) wake_word_resume();
            return ret;
        }
    }

#if LANG_AMP_SD_GPIO >= 0
    /* Enable amp; 20 ms for output capacitor and supply to stabilise.
     * The SD pin gates the class-D output stage — pulling low eliminates
     * idle current draw and hiss between utterances. */
    gpio_set_level(LANG_AMP_SD_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

    /* Stream PCM data in 4KB chunks with software volume scaling.
     * LANG_AUDIO_VOLUME (0–256, 128 = 50% = -6 dB) reduces peak amp current
     * to prevent brownouts on USB/limited PSU rails. */
    const uint8_t *pcm    = wav_data + info.pcm_offset;
    size_t         remain = info.pcm_len;
    size_t         written;
    int16_t        vol_buf[2048];   /* 4096 bytes = 2048 samples, stack is fine */

    uint8_t vol = s_volume;
    ESP_LOGI(TAG, "Playing WAV: %u bytes PCM (vol=%u/255)", (unsigned)remain, vol);

    while (remain > 0) {
        if (s_play_cancel) {
            ESP_LOGI(TAG, "Playback cancelled (%u bytes remaining)", (unsigned)remain);
            break;
        }
        size_t chunk = remain < 4096 ? remain : 4096;
        if (vol < 255) {
            /* Scale 16-bit samples; safe for both mono and stereo */
            size_t samples = chunk / 2;
            const int16_t *src = (const int16_t *)pcm;
            for (size_t i = 0; i < samples; i++) {
                vol_buf[i] = (int16_t)(((int32_t)src[i] * vol) >> 8);
            }
            ret = i2s_channel_write(s_tx_handle, vol_buf, chunk, &written, pdMS_TO_TICKS(1000));
        } else {
            ret = i2s_channel_write(s_tx_handle, pcm, chunk, &written, pdMS_TO_TICKS(1000));
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write error: %s", esp_err_to_name(ret));
            break;
        }
        pcm    += written;
        remain -= written;
    }

#if LANG_AMP_SD_GPIO >= 0
    /* Drain: flush the I2S DMA FIFO so the last samples reach the DAC,
     * then disable amp. Small delay avoids a click/pop from abrupt shutdown. */
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(LANG_AMP_SD_GPIO, 0);
    ESP_LOGD(TAG, "Amp shutdown (GPIO%d low)", LANG_AMP_SD_GPIO);
#endif

    /* Restore I2S to 16kHz (mic native rate) after playback.
     * TX and RX share I2S_NUM_0 (full-duplex), so any TX sample-rate change
     * also changes RX BCLK.  The AFE/wake-word expects 16kHz; leaving the
     * bus at e.g. 24kHz after TTS breaks wake-word detection. */
    if (s_current_sample_rate != LANG_MIC_SAMPLE_RATE) {
        ESP_LOGI(TAG, "Restoring I2S to %u Hz for mic", (unsigned)LANG_MIC_SAMPLE_RATE);
        i2s_configure(LANG_MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT);
    }

    /* Explicitly restart RX channel after any clock change.
     * The TX-focused reconfiguration in i2s_configure() disables/re-enables RX,
     * but the RX DMA can get stuck if the BCLK changed during playback.
     * A full disable→enable cycle resets the DMA pointers. */
    if (ww_suspended) {
        i2s_audio_rx_restart();
        ESP_LOGI(TAG, "I2S RX restarted after playback");
    }

    /* Resume wake word feed task now that I2S is back to mic rate */
    if (ww_suspended) {
        wake_word_resume();
    }

    ESP_LOGI(TAG, "Playback complete");
    return ESP_OK;
}

/* ── Async playback ────────────────────────────────────────────── */

static void i2s_play_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const uint8_t *wav = s_play_wav;
        size_t len = s_play_wav_len;
        s_play_cancel = false;

        if (wav && len > 0) {
            i2s_audio_play_wav(wav, len);
        }
    }
}

esp_err_t i2s_audio_play_wav_async(const uint8_t *wav_data, size_t len)
{
    if (!s_tx_handle) return ESP_ERR_INVALID_STATE;

    /* Create playback task on first call (Core 0, keeps Core 1 for agent) */
    if (!s_play_task) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            i2s_play_task, "i2s_play", 8192, NULL, 4, &s_play_task, 0);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create I2S play task");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Cancel current playback if any.
     * 100ms allows: cancel flag seen (1 write cycle ~50ms) + amp shutdown (30ms) */
    s_play_cancel = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    s_play_wav     = wav_data;
    s_play_wav_len = len;
    xTaskNotifyGive(s_play_task);

    return ESP_OK;
}

void i2s_audio_stop(void)
{
    s_play_cancel = true;
}

/* ── Speaker test tone (440 Hz, ~2 seconds) ───────────────────── */

#include <math.h>

esp_err_t i2s_audio_test_tone(void)
{
    if (!s_tx_handle) {
        ESP_LOGE(TAG, "I2S not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t sample_rate = 16000;
    const float freq = 440.0f;
    const float duration = 2.0f;
    const uint32_t total_samples = (uint32_t)(sample_rate * duration);

    /* Reconfigure to 16kHz if needed */
    if (s_current_sample_rate != sample_rate ||
        s_current_bits != (uint32_t)I2S_DATA_BIT_WIDTH_16BIT) {
        esp_err_t ret = i2s_configure(sample_rate, I2S_DATA_BIT_WIDTH_16BIT);
        if (ret != ESP_OK) return ret;
    }

#if LANG_AMP_SD_GPIO >= 0
    gpio_set_level(LANG_AMP_SD_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  /* longer settle for cold-start test */
#endif

    uint8_t vol = s_volume;
    ESP_LOGI(TAG, "Test tone: 440 Hz, %g s, %u Hz, vol=%u/255", duration, sample_rate, vol);

    int16_t buf[512];
    uint32_t sample = 0;
    while (sample < total_samples) {
        uint32_t chunk = total_samples - sample;
        if (chunk > 512) chunk = 512;
        for (uint32_t i = 0; i < chunk; i++) {
            float t = (float)(sample + i) / (float)sample_rate;
            int16_t s = (int16_t)(sinf(2.0f * M_PI * freq * t) * 16000.0f);
            buf[i] = (int16_t)(((int32_t)s * vol) >> 8);
        }
        size_t written;
        esp_err_t ret = i2s_channel_write(s_tx_handle, buf, chunk * 2, &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write error: %s", esp_err_to_name(ret));
            break;
        }
        sample += chunk;
    }

#if LANG_AMP_SD_GPIO >= 0
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(LANG_AMP_SD_GPIO, 0);
#endif

    ESP_LOGI(TAG, "Test tone complete");
    return ESP_OK;
}

esp_err_t i2s_audio_rx_restart(void)
{
    if (!s_rx_handle) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Restarting I2S RX channel");
    i2s_channel_disable(s_rx_handle);
    esp_err_t ret = i2s_channel_enable(s_rx_handle);
    s_rx_enabled = (ret == ESP_OK);
    return ret;
}

esp_err_t i2s_audio_read(uint8_t *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_rx_handle || !s_rx_enabled) {
        ESP_LOGE(TAG, "I2S RX not ready — call i2s_audio_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!buf || buf_size == 0 || !bytes_read) return ESP_ERR_INVALID_ARG;

    /* RX is configured for 32-bit data (INMP441 outputs 24-bit left-justified
     * in 32-bit slots).  Callers expect 16-bit PCM.
     *
     * ESP32-S3 DMA delivers interleaved L+R 32-bit samples even in "mono" mode
     * when data_bit_width=32.  We extract every other sample (the real mic
     * channel), then right-shift by 16.
     *
     * Caller asks for N bytes of 16-bit data = N/2 samples.
     * Each output sample needs 2 raw 32-bit values (L+R pair) = 8 bytes.
     * So raw_size = N/2 * 8 = N*4. */
    size_t raw_size = buf_size * 4;

    /* Heap-allocate the raw buffer (too large for task stacks) */
    uint8_t *raw_buf = malloc(raw_size);  /* falls to PSRAM if >4KB */
    if (!raw_buf) return ESP_ERR_NO_MEM;

    size_t raw_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_handle, raw_buf, raw_size,
                                      &raw_read, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        /* Log the first few errors and periodically to avoid spamming */
        static int s_read_err_count = 0;
        s_read_err_count++;
        if (s_read_err_count <= 3 || (s_read_err_count % 50000 == 0)) {
            ESP_LOGW(TAG, "i2s_channel_read error: %s (#%d, rx_en=%d)",
                     esp_err_to_name(ret), s_read_err_count, s_rx_enabled);
        }
        free(raw_buf);
        *bytes_read = 0;
        return ret;
    }

    /* DMA delivers interleaved L+R 32-bit samples even in "mono" mode on
     * ESP32-S3 when data_bit_width=32 and slot_bit_width=32.
     * INMP441 with L/R=GND outputs real data in one channel; the other
     * channel is tri-stated garbage (floating bus → 0, -256, -32768, etc).
     *
     * PROBLEM: DMA channel alignment can shift between reads on ESP32-S3.
     * A one-time auto-detect is NOT reliable — the L/R order can change.
     *
     * SOLUTION: DC-tracking per-sample channel selection.
     * The mic signal stays near a slowly-moving DC offset (~80 in silence).
     * The garbage channel produces values far from the mic DC (0, ±256, ±32768).
     * For each L+R pair, pick the value closer to the running DC estimate.
     * Bootstrap: auto-detect on first read to seed the DC estimate. */
    size_t n_raw = raw_read / 4;
    int32_t *src = (int32_t *)raw_buf;
    int16_t *dst = (int16_t *)buf;

    /* One-time diagnostic: dump first 8 raw samples */
    static bool s_dump_done = false;
    if (!s_dump_done && n_raw >= 8) {
        ESP_LOGI(TAG, "RX raw 32-bit [0..7]: 0x%08lx 0x%08lx 0x%08lx 0x%08lx "
                 "0x%08lx 0x%08lx 0x%08lx 0x%08lx",
                 (unsigned long)(uint32_t)src[0], (unsigned long)(uint32_t)src[1],
                 (unsigned long)(uint32_t)src[2], (unsigned long)(uint32_t)src[3],
                 (unsigned long)(uint32_t)src[4], (unsigned long)(uint32_t)src[5],
                 (unsigned long)(uint32_t)src[6], (unsigned long)(uint32_t)src[7]);
        s_dump_done = true;
    }

    /* DC-tracking state (persists across calls).
     * s_dc_q16: Q16.16 fixed-point DC estimate of the mic signal.
     * Initialised from the first read's auto-detected mic channel. */
    static int32_t s_dc_q16 = 0;
    static bool    s_dc_init = false;

    if (!s_dc_init && n_raw >= 64) {
        /* Bootstrap: auto-detect mic channel on first read to seed the DC.
         * Use cross-zero + range heuristic (reliable for one read). */
        int16_t min_e = INT16_MAX, max_e = INT16_MIN;
        int16_t min_o = INT16_MAX, max_o = INT16_MIN;
        int64_t sum_e = 0, sum_o = 0;
        int pairs = (n_raw < 128) ? (int)(n_raw / 2) : 64;
        for (int k = 0; k < pairs; k++) {
            int16_t a = (int16_t)(src[k * 2]     >> 16);
            int16_t b = (int16_t)(src[k * 2 + 1] >> 16);
            if (a < min_e) min_e = a;
            if (a > max_e) max_e = a;
            if (b < min_o) min_o = b;
            if (b > max_o) max_o = b;
            sum_e += a;  sum_o += b;
        }
        bool xzero_e = (min_e < 0 && max_e > 0);
        bool xzero_o = (min_o < 0 && max_o > 0);
        int range_e  = (int)max_e - (int)min_e;
        int range_o  = (int)max_o - (int)min_o;

        int boot_ch;
        if (xzero_e && !xzero_o)       boot_ch = 0;
        else if (!xzero_e && xzero_o)   boot_ch = 1;
        else                             boot_ch = (range_e <= range_o) ? 0 : 1;

        int32_t dc_int = (int32_t)((boot_ch == 0 ? sum_e : sum_o) / pairs);
        s_dc_q16 = dc_int << 16;
        s_dc_init = true;

        ESP_LOGW(TAG, "Mic DC-track boot: ch=%s dc=%d (xzero e=%d o=%d, "
                 "range e=%d [%d..%d] o=%d [%d..%d], n=%d)",
                 boot_ch ? "ODD" : "EVEN", (int)dc_int,
                 xzero_e, xzero_o,
                 range_e, (int)min_e, (int)max_e,
                 range_o, (int)min_o, (int)max_o, pairs);
    }

    /* Per-sample DC-tracking channel selection.
     * For each L+R pair, pick the value closer to the DC estimate.
     * Update DC with IIR: alpha=1/1024 → time constant ~64ms @ 16kHz. */
    size_t out_idx = 0;
    for (size_t i = 0; i + 1 < n_raw; i += 2) {
        int16_t a = (int16_t)(src[i]     >> 16);
        int16_t b = (int16_t)(src[i + 1] >> 16);
        int32_t dc16 = s_dc_q16 >> 16;         /* integer part of DC */
        int32_t da = (int32_t)a - dc16;
        int32_t db = (int32_t)b - dc16;
        if (da < 0) da = -da;
        if (db < 0) db = -db;
        int16_t pick = (da <= db) ? a : b;

        /* IIR DC update: dc += (pick - dc) >> 10  (alpha ≈ 1/1024) */
        s_dc_q16 += (((int32_t)pick << 16) - s_dc_q16) >> 10;

        dst[out_idx++] = pick;
    }
    *bytes_read = out_idx * 2;

    free(raw_buf);
    return ESP_OK;
}
