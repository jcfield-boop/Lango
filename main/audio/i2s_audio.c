#include "i2s_audio.h"
#include "langoustine_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2s_audio";

/* I2S TX channel handle (I2S_NUM_0, master, TX only) */
static i2s_chan_handle_t s_tx_handle = NULL;

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
        i2s_channel_disable(s_tx_handle);
    }

    if (s_current_sample_rate == 0) {
        /* ── First-time initialisation ── */
        i2s_std_config_t std_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO),
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
        ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        /* ── Reconfiguration (channel already initialised) ── */
        i2s_std_clk_config_t  clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        i2s_std_slot_config_t slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO);

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

    /* Create TX-only channel on I2S_NUM_0 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Initial configuration: 16kHz, 16-bit mono (matches most TTS output) */
    ret = i2s_configure(16000, I2S_DATA_BIT_WIDTH_16BIT);
    if (ret != ESP_OK) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "I2S audio init OK (BCLK=%d LRCLK=%d DOUT=%d)",
             LANG_I2S_BCLK, LANG_I2S_LRCLK, LANG_I2S_DOUT);
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

    /* Reconfigure I2S only if parameters changed */
    if (info.sample_rate != s_current_sample_rate ||
        (uint32_t)bits    != s_current_bits) {
        ret = i2s_configure(info.sample_rate, bits);
        if (ret != ESP_OK) return ret;
    }

    /* Stream PCM data in 4KB chunks */
    const uint8_t *pcm    = wav_data + info.pcm_offset;
    size_t         remain = info.pcm_len;
    size_t         written;

    ESP_LOGI(TAG, "Playing WAV: %u bytes PCM", (unsigned)remain);

    while (remain > 0) {
        size_t chunk = remain < 4096 ? remain : 4096;
        ret = i2s_channel_write(s_tx_handle, pcm, chunk, &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write error: %s", esp_err_to_name(ret));
            return ret;
        }
        pcm    += written;
        remain -= written;
    }

    ESP_LOGI(TAG, "Playback complete");
    return ESP_OK;
}
