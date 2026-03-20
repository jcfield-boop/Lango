#include "i2s_audio.h"
#include "langoustine_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_types.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "soc/i2s_struct.h"
#include "driver/i2s_common.h"

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

/* I2S channel handles — full-duplex on I2S_NUM_0 (TX=speaker, RX=mic).
 * BCLK/WS shared between MAX98357A and INMP441 on GPIO 3/4.
 * In full-duplex, RX is forced to slave and uses TX clocks via sig_loopback.
 * TX DMA runs continuously: auto_clear_after_cb fills played buffers with
 * silence, keeping BCLK alive for the mic even when no audio is playing.
 * Amp gated by GPIO 42 (SD pin) so silence writes produce no sound. */
static i2s_chan_handle_t s_tx_handle = NULL;  /* speaker TX */
static i2s_chan_handle_t s_rx_handle = NULL;  /* mic RX     */
static bool             s_rx_enabled = false;

/* Track current I2S configuration to avoid unnecessary reconfigures */
static uint32_t s_current_sample_rate = 0;
static uint32_t s_current_bits        = 0;

/* DMA stall recovery counters (reset by i2s_audio_rx_restart) */
static int s_rx_restart_count = 0;
static int s_rx_consec_fails  = 0;

/* ── Silence pump task ─────────────────────────────────────────── */
/* Keeps TX DMA active by writing silence when no playback is running.
 * This ensures BCLK/WS stay alive for the INMP441 mic (RX is slave).
 * The amp SD pin is LOW during silence so no sound is produced.
 * During playback, the pump yields — play_wav takes over TX writes. */
static volatile bool s_playback_active = false;

static void silence_pump_task(void *arg)
{
    const size_t buf_sz = LANG_I2S_DMA_FRAME_NUM * 4;  /* 32-bit slot × mono */
    uint8_t *zeros = calloc(1, buf_sz);
    if (!zeros) {
        ESP_LOGE(TAG, "silence_pump: calloc failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Silence pump started (%u bytes/write)", (unsigned)buf_sz);
    int err_count = 0;
    while (1) {
        /* Yield while playback owns the TX channel */
        if (s_playback_active) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t written;
        esp_err_t ret = i2s_channel_write(s_tx_handle, zeros, buf_sz,
                                           &written, pdMS_TO_TICKS(500));
        if (ret != ESP_OK) {
            err_count++;
            if (err_count <= 3 || (err_count % 100) == 0) {
                ESP_LOGW(TAG, "silence_pump err: %s (#%d)",
                         esp_err_to_name(ret), err_count);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            err_count = 0;
        }
    }
}

/* ── WAV header parsing ─────────────────────────────────────────── */

typedef struct {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t num_channels;
    size_t   pcm_offset;
    size_t   pcm_len;
} wav_info_t;

static esp_err_t parse_wav(const uint8_t *buf, size_t buf_len, wav_info_t *out)
{
    if (!buf || buf_len < 44 || !out) return ESP_ERR_INVALID_ARG;

    if (memcmp(buf,     "RIFF", 4) != 0 ||
        memcmp(buf + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a RIFF/WAVE file");
        return ESP_ERR_INVALID_ARG;
    }

    bool found_fmt  = false;
    bool found_data = false;
    size_t pos = 12;

    while (pos + 8 <= buf_len) {
        uint32_t chunk_size =
            (uint32_t)buf[pos+4]        |
            ((uint32_t)buf[pos+5] << 8) |
            ((uint32_t)buf[pos+6] << 16)|
            ((uint32_t)buf[pos+7] << 24);

        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            if (pos + 8 + 16 > buf_len) break;
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
            if (out->pcm_offset >= buf_len ||
                out->pcm_len > buf_len - out->pcm_offset) {
                out->pcm_len = buf_len - out->pcm_offset;
            }
            found_data = true;
        }

        if (found_fmt && found_data) break;
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
 * i2s_configure() handles sample-rate / bit-depth changes for playback.
 * MUST disable RX before TX — RX is a slave that derives clocks from TX
 * via sig_loopback.  Changing TX clocks with RX DMA running causes the
 * RX DMA to stall (descriptor cycle completes but next fetch sees wrong
 * clock → timeout on the next i2s_channel_read).
 */
static esp_err_t i2s_configure(uint32_t sample_rate, i2s_data_bit_width_t bits)
{
    esp_err_t ret;

    if (s_current_sample_rate == 0) {
        ESP_LOGE(TAG, "i2s_configure called before init — should not happen");
        return ESP_ERR_INVALID_STATE;
    }

    /* Disable RX first — BCLK/WS are physically shared, so RX (slave on
     * I2S_NUM_1) would see garbled clocks during TX reconfiguration. */
    bool rx_was_enabled = s_rx_enabled;
    if (rx_was_enabled && s_rx_handle) {
        i2s_channel_disable(s_rx_handle);
        s_rx_enabled = false;
    }

    /* Disable TX, reconfigure, re-enable */
    i2s_channel_disable(s_tx_handle);

    i2s_std_clk_config_t  clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO);
    slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    slot_cfg.ws_width       = 32;

    ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_reconfig_std_clock failed: %s", esp_err_to_name(ret));
        goto reenable;
    }
    ret = i2s_channel_reconfig_std_slot(s_tx_handle, &slot_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_reconfig_std_slot failed: %s", esp_err_to_name(ret));
        goto reenable;
    }

reenable:
    /* Re-enable TX first (generates clocks), then RX (slave) */
    {
        esp_err_t en_ret = i2s_channel_enable(s_tx_handle);
        if (en_ret != ESP_OK) {
            ESP_LOGE(TAG, "TX re-enable failed: %s", esp_err_to_name(en_ret));
            if (ret == ESP_OK) ret = en_ret;
        }
    }

    if (rx_was_enabled && s_rx_handle) {
        esp_err_t rx_ret = i2s_channel_enable(s_rx_handle);
        if (rx_ret == ESP_OK) {
            s_rx_enabled = true;
        } else {
            ESP_LOGE(TAG, "RX re-enable failed: %s", esp_err_to_name(rx_ret));
        }
    }

    if (ret == ESP_OK) {
        s_current_sample_rate = sample_rate;
        s_current_bits        = (uint32_t)bits;
        ESP_LOGI(TAG, "I2S configured: %u Hz, %u-bit", (unsigned)sample_rate, (unsigned)bits);
    }
    return ret;
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t i2s_audio_init(void)
{
    if (s_tx_handle) {
        ESP_LOGW(TAG, "i2s_audio_init called more than once — ignoring");
        return ESP_OK;
    }

    /* SIMPLEX architecture: TX on I2S_NUM_0 (master), RX on I2S_NUM_1 (slave).
     *
     * BCLK/WS physically shared on GPIO 3/4 between MAX98357A and INMP441.
     * TX (I2S_NUM_0) drives BCLK/WS as outputs.
     * RX (I2S_NUM_1) reads the same BCLK/WS pins as inputs (slave mode).
     *
     * This avoids the full-duplex sig_loopback DMA coupling that caused
     * permanent RX timeouts on ESP32-S3.  Matches Espressif's own ESP-SR
     * example (esp32s3-eye) which uses simplex RX-only on I2S_NUM_1. */
    esp_err_t ret;

    /* ── Step 1: Create + init TX channel (I2S_NUM_0, master, TX-only) ── */
    {
        i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        tx_chan_cfg.dma_desc_num       = LANG_I2S_DMA_DESC_NUM;
        tx_chan_cfg.dma_frame_num      = LANG_I2S_DMA_FRAME_NUM;
        tx_chan_cfg.auto_clear_after_cb = true;
        ret = i2s_new_channel(&tx_chan_cfg, &s_tx_handle, NULL);  /* TX-only */
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TX i2s_new_channel failed: %s", esp_err_to_name(ret));
            return ret;
        }

        /* INMP441 requires 64 BCLK per frame (32-bit slots).
         * MAX98357A works fine with 32-bit slots (ignores padding). */
        i2s_std_config_t tx_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(LANG_MIC_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                             I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = LANG_I2S_BCLK,
                .ws   = LANG_I2S_LRCLK,
                .dout = LANG_I2S_DOUT,
                .din  = I2S_GPIO_UNUSED,
                .invert_flags = { false, false, false },
            },
        };
        tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        tx_cfg.slot_cfg.ws_width       = 32;
        ret = i2s_channel_init_std_mode(s_tx_handle, &tx_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TX init FAILED: %s", esp_err_to_name(ret));
            i2s_del_channel(s_tx_handle);
            s_tx_handle = NULL;
            return ret;
        }
        ESP_LOGI(TAG, "TX init OK on I2S_NUM_0 (BCLK=%d WS=%d DOUT=%d)",
                 LANG_I2S_BCLK, LANG_I2S_LRCLK, LANG_I2S_DOUT);
    }

    /* ── Step 2: Enable TX + preload silence so BCLK/WS are running ── */
    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TX enable FAILED: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
        return ret;
    }
    s_current_sample_rate = LANG_MIC_SAMPLE_RATE;
    s_current_bits        = (uint32_t)I2S_DATA_BIT_WIDTH_16BIT;

    {
        const size_t fill_sz = LANG_I2S_DMA_FRAME_NUM * 4;
        uint8_t *silence = calloc(1, fill_sz);
        if (silence) {
            size_t wr;
            for (int i = 0; i < LANG_I2S_DMA_DESC_NUM; i++) {
                i2s_channel_write(s_tx_handle, silence, fill_sz, &wr, pdMS_TO_TICKS(500));
            }
            free(silence);
            ESP_LOGI(TAG, "TX preloaded %d silence buffers — BCLK running", LANG_I2S_DMA_DESC_NUM);
        }
    }

    /* ── Step 3: Create + init RX channel (I2S_NUM_1, slave, RX-only) ──
     * Slave mode: reads BCLK/WS generated by TX on the shared bus.
     * Same GPIO pins — the I2S peripheral configures them as inputs. */
    {
        i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
        rx_chan_cfg.dma_desc_num  = LANG_I2S_DMA_DESC_NUM;
        rx_chan_cfg.dma_frame_num = LANG_I2S_DMA_FRAME_NUM;
        ret = i2s_new_channel(&rx_chan_cfg, NULL, &s_rx_handle);  /* RX-only */
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RX i2s_new_channel failed: %s", esp_err_to_name(ret));
            /* TX still works for speaker; mic just won't work */
        } else {
            i2s_std_config_t rx_cfg = {
                .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(LANG_MIC_SAMPLE_RATE),
                .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                                 I2S_SLOT_MODE_MONO),
                .gpio_cfg = {
                    .mclk = I2S_GPIO_UNUSED,
                    .bclk = LANG_I2S_BCLK,
                    .ws   = LANG_I2S_LRCLK,
                    .dout = I2S_GPIO_UNUSED,
                    .din  = LANG_I2S_DIN,
                    .invert_flags = { false, false, false },
                },
            };
            /* INMP441 outputs 24-bit data left-justified in 32-bit slots.
             * data_bit_width MUST be 32 to match — using 16 causes DMA
             * descriptor byte-count mismatch (first read OK, then stall).
             * We extract the upper 16 bits in i2s_audio_read(). */
            rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
            rx_cfg.slot_cfg.ws_width       = 32;
            rx_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_LEFT;
            ret = i2s_channel_init_std_mode(s_rx_handle, &rx_cfg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "RX init FAILED: %s", esp_err_to_name(ret));
                i2s_del_channel(s_rx_handle);
                s_rx_handle = NULL;
            } else {
                ESP_LOGI(TAG, "RX init OK on I2S_NUM_1 slave (BCLK=%d WS=%d DIN=%d)",
                         LANG_I2S_BCLK, LANG_I2S_LRCLK, LANG_I2S_DIN);
            }
        }
    }

    /* ── Step 4: Enable RX ── */
    if (s_rx_handle) {
        ret = i2s_channel_enable(s_rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RX enable FAILED: %s", esp_err_to_name(ret));
        } else {
            s_rx_enabled = true;
            ESP_LOGI(TAG, "RX enabled on DIN=%d (slave, clocked by TX)", LANG_I2S_DIN);
        }
    }

    /* Diagnostic: immediate RX read to confirm DMA is alive */
    if (s_rx_enabled) {
        uint8_t *test_buf = malloc(256);
        if (test_buf) {
            size_t test_read = 0;
            esp_err_t rx_test = i2s_channel_read(s_rx_handle, test_buf, 256,
                                                  &test_read, pdMS_TO_TICKS(2000));
            if (rx_test == ESP_OK && test_read > 0) {
                int32_t *raw32 = (int32_t *)test_buf;
                ESP_LOGI(TAG, "RX self-test OK: %u bytes, raw32[0]=0x%08lx raw32[1]=0x%08lx",
                         (unsigned)test_read,
                         (unsigned long)(uint32_t)raw32[0],
                         (unsigned long)(uint32_t)raw32[1]);
            } else {
                ESP_LOGE(TAG, "RX self-test FAILED: %s (%u bytes)",
                         esp_err_to_name(rx_test), (unsigned)test_read);
            }
            free(test_buf);
        }
    }

    /* Start silence pump — keeps TX DMA active so BCLK runs for the mic.
     * Low priority, small SRAM stack (no flash/NVS access). */
    xTaskCreatePinnedToCore(silence_pump_task, "i2s_sil",
                            3072, NULL, 2, NULL, 0);

    /* Load persisted volume from NVS */
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

    ESP_LOGI(TAG, "I2S audio init OK (simplex: TX=I2S0 master, RX=I2S1 slave | "
             "BCLK=%d LRCLK=%d DOUT=%d DIN=%d | vol=%u/255 dma=%dx%d)",
             LANG_I2S_BCLK, LANG_I2S_LRCLK, LANG_I2S_DOUT, LANG_I2S_DIN,
             s_volume, LANG_I2S_DMA_DESC_NUM, LANG_I2S_DMA_FRAME_NUM);

#if LANG_AMP_SD_GPIO >= 0
    gpio_reset_pin(LANG_AMP_SD_GPIO);
    gpio_set_direction(LANG_AMP_SD_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LANG_AMP_SD_GPIO, 0);
    ESP_LOGI(TAG, "Amp SD pin GPIO%d configured (amp off)", LANG_AMP_SD_GPIO);
#endif

    /* Increase drive strength on I2S clock GPIOs for cleaner edges */
    gpio_set_drive_capability(LANG_I2S_BCLK,  GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(LANG_I2S_LRCLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(LANG_I2S_DOUT,  GPIO_DRIVE_CAP_3);

    return ESP_OK;
}

esp_err_t i2s_audio_play_wav(const uint8_t *wav_data, size_t len)
{
    if (!s_tx_handle) {
        ESP_LOGE(TAG, "I2S not initialised — call i2s_audio_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wav_data || len == 0) return ESP_ERR_INVALID_ARG;

    wav_info_t info = {0};
    esp_err_t ret = parse_wav(wav_data, len, &info);
    if (ret != ESP_OK) return ret;

    if (info.pcm_len == 0) {
        ESP_LOGW(TAG, "WAV data chunk is empty — nothing to play");
        return ESP_OK;
    }

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

    /* Take over TX from silence pump */
    s_playback_active = true;

    /* Reconfigure TX if sample rate or bit depth changed.
     * In full-duplex, this also changes the RX clock — mic data will be
     * at the wrong rate during playback, but feed_task handles that
     * gracefully (reads succeed, data is just at wrong rate). */
    if (info.sample_rate != s_current_sample_rate ||
        (uint32_t)bits    != s_current_bits) {
        ret = i2s_configure(info.sample_rate, bits);
        if (ret != ESP_OK) {
            s_playback_active = false;
            return ret;
        }
    }

#if LANG_AMP_SD_GPIO >= 0
    gpio_set_level(LANG_AMP_SD_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

    const uint8_t *pcm    = wav_data + info.pcm_offset;
    size_t         remain = info.pcm_len;
    size_t         written;
    int16_t        vol_buf[2048];

    uint8_t  vol = s_volume;
    uint32_t total_samples  = (uint32_t)(info.pcm_len / 2);
    uint32_t fade_samples   = (info.sample_rate * LANG_I2S_FADE_MS) / 1000;
    if (fade_samples > total_samples / 2) fade_samples = total_samples / 2;
    uint32_t fade_out_start = total_samples - fade_samples;
    uint32_t sample_offset  = 0;

    ESP_LOGI(TAG, "Playing WAV: %u bytes PCM (vol=%u/255, fade=%u samples)",
             (unsigned)remain, vol, (unsigned)fade_samples);

    while (remain > 0) {
        if (s_play_cancel) {
            ESP_LOGI(TAG, "Playback cancelled (%u bytes remaining)", (unsigned)remain);
            break;
        }
        size_t chunk = remain < 4096 ? remain : 4096;
        size_t chunk_samples = chunk / 2;
        const int16_t *src = (const int16_t *)pcm;

        for (size_t i = 0; i < chunk_samples; i++) {
            int32_t s = (int32_t)src[i];

            if (vol < 255) {
                s = (s * vol) >> 8;
            }

            uint32_t abs_idx = sample_offset + (uint32_t)i;
            if (abs_idx < fade_samples) {
                s = (s * (int32_t)abs_idx) / (int32_t)fade_samples;
            }
            else if (abs_idx >= fade_out_start) {
                uint32_t pos = abs_idx - fade_out_start;
                s = (s * (int32_t)(fade_samples - pos)) / (int32_t)fade_samples;
            }

            vol_buf[i] = (int16_t)s;
        }
        sample_offset += (uint32_t)chunk_samples;

        ret = i2s_channel_write(s_tx_handle, vol_buf, chunk, &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write error: %s", esp_err_to_name(ret));
            break;
        }
        pcm    += written;
        remain -= written;
    }

#if LANG_AMP_SD_GPIO >= 0
    {
        uint32_t one_buf_ms = (LANG_I2S_DMA_FRAME_NUM * 1000) / info.sample_rate;
        vTaskDelay(pdMS_TO_TICKS(one_buf_ms + 20));
    }
    gpio_set_level(LANG_AMP_SD_GPIO, 0);
    ESP_LOGD(TAG, "Amp shutdown (GPIO%d low)", LANG_AMP_SD_GPIO);
#endif

    /* Restore mic sample rate if playback changed it.
     * This ensures BCLK returns to 16kHz for the INMP441. */
    if (s_current_sample_rate != LANG_MIC_SAMPLE_RATE) {
        ESP_LOGI(TAG, "Restoring I2S to mic rate (%u Hz)", LANG_MIC_SAMPLE_RATE);
        i2s_configure(LANG_MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT);
    }

    /* Release TX back to silence pump */
    s_playback_active = false;

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

    if (!s_play_task) {
        BaseType_t ret = xTaskCreatePinnedToCore(
            i2s_play_task, "i2s_play", 8192, NULL, 4, &s_play_task, 0);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create I2S play task");
            return ESP_ERR_NO_MEM;
        }
    }

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

    s_playback_active = true;

    if (s_current_sample_rate != sample_rate ||
        s_current_bits != (uint32_t)I2S_DATA_BIT_WIDTH_16BIT) {
        esp_err_t ret = i2s_configure(sample_rate, I2S_DATA_BIT_WIDTH_16BIT);
        if (ret != ESP_OK) { s_playback_active = false; return ret; }
    }

#if LANG_AMP_SD_GPIO >= 0
    gpio_set_level(LANG_AMP_SD_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    uint8_t vol = s_volume;
    uint32_t fade_samples = (sample_rate * LANG_I2S_FADE_MS) / 1000;
    if (fade_samples > total_samples / 2) fade_samples = total_samples / 2;
    uint32_t fade_out_start = total_samples - fade_samples;

    ESP_LOGI(TAG, "Test tone: 440 Hz, %g s, %u Hz, vol=%u/255, fade=%u",
             duration, sample_rate, vol, (unsigned)fade_samples);

    int16_t buf[512];
    uint32_t sample = 0;
    while (sample < total_samples) {
        uint32_t chunk = total_samples - sample;
        if (chunk > 512) chunk = 512;
        for (uint32_t i = 0; i < chunk; i++) {
            float t = (float)(sample + i) / (float)sample_rate;
            int32_t s = (int32_t)(sinf(2.0f * M_PI * freq * t) * 16000.0f);
            if (vol < 255) s = (s * vol) >> 8;
            uint32_t abs_idx = sample + i;
            if (abs_idx < fade_samples) {
                s = (s * (int32_t)abs_idx) / (int32_t)fade_samples;
            } else if (abs_idx >= fade_out_start) {
                uint32_t pos = abs_idx - fade_out_start;
                s = (s * (int32_t)(fade_samples - pos)) / (int32_t)fade_samples;
            }
            buf[i] = (int16_t)s;
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
    {
        uint32_t one_buf_ms = (LANG_I2S_DMA_FRAME_NUM * 1000) / sample_rate;
        vTaskDelay(pdMS_TO_TICKS(one_buf_ms + 20));
    }
    gpio_set_level(LANG_AMP_SD_GPIO, 0);
#endif

    s_playback_active = false;
    ESP_LOGI(TAG, "Test tone complete");
    return ESP_OK;
}

esp_err_t i2s_audio_rx_restart(void)
{
    if (!s_rx_handle) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Restarting I2S RX channel (I2S_NUM_1)");
    i2s_channel_disable(s_rx_handle);
    esp_err_t ret = i2s_channel_enable(s_rx_handle);
    s_rx_enabled = (ret == ESP_OK);
    s_rx_restart_count = 0;
    s_rx_consec_fails  = 0;
    return ret;
}

void i2s_audio_diag(void)
{
    printf("\n=== I2S DIAGNOSTIC (simplex: TX=I2S0 master, RX=I2S1 slave) ===\n");
    printf("Handles: TX=%p RX=%p  rx_enabled=%d\n",
           (void *)s_tx_handle, (void *)s_rx_handle, s_rx_enabled);
    printf("Config:  rate=%lu bits=%lu  playback_active=%d\n",
           (unsigned long)s_current_sample_rate, (unsigned long)s_current_bits,
           s_playback_active);

    if (!s_rx_handle) {
        printf("ERROR: RX handle is NULL — i2s_audio_init failed\n");
        return;
    }

    /* Raw DMA read — 1024 bytes, 3-second timeout, bypasses i2s_audio_read() */
    uint8_t raw[1024];
    size_t got = 0;
    printf("Raw i2s_channel_read(1024 bytes, 3s timeout)...\n");
    esp_err_t ret = i2s_channel_read(s_rx_handle, raw, sizeof(raw), &got, pdMS_TO_TICKS(3000));
    printf("Result: %s, got=%u bytes\n", esp_err_to_name(ret), (unsigned)got);

    if (ret == ESP_OK && got > 0) {
        printf("Hex dump [0..64]:\n");
        for (size_t i = 0; i < got && i < 64; i++) {
            printf("%02x ", raw[i]);
            if ((i & 15) == 15) printf("\n");
        }
        printf("\n");

        int16_t *s16 = (int16_t *)raw;
        size_t n16 = got / 2;
        printf("As int16[0..%u]: ", (unsigned)(n16 < 16 ? n16 : 16));
        for (size_t i = 0; i < n16 && i < 16; i++) {
            printf("%d ", s16[i]);
        }
        printf("\n");

        bool all_zero = true;
        for (size_t i = 0; i < got; i++) {
            if (raw[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) {
            printf("WARNING: All bytes are zero — INMP441 SD line may not be connected\n");
        }
    }

    printf("\n=== WIRING CHECKLIST ===\n");
    printf("  1. INMP441 VDD → 3.3V:  expect 3.2-3.4V\n");
    printf("  2. INMP441 GND → GND:   expect continuity\n");
    printf("  3. INMP441 L/R → GND:   left channel select\n");
    printf("  4. INMP441 SCK → GPIO%d: expect ~1.6V avg\n", LANG_I2S_BCLK);
    printf("  5. INMP441 WS  → GPIO%d: expect ~1.6V avg\n", LANG_I2S_LRCLK);
    printf("  6. INMP441 SD  → GPIO%d: expect ~1.0-1.8V avg\n", LANG_I2S_DIN);
    printf("======================\n\n");
}

esp_err_t i2s_audio_read(uint8_t *buf, size_t buf_size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!s_rx_handle || !s_rx_enabled) {
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }
    if (!buf || buf_size == 0 || !bytes_read) return ESP_ERR_INVALID_ARG;

    /* During playback at a different sample rate, RX clocks are wrong
     * (BCLK/WS shared physically).  Return immediately so callers
     * (wake word feed) don't get garbage. */
    if (s_playback_active && s_current_sample_rate != LANG_MIC_SAMPLE_RATE) {
        *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }

    /* RX data_bit_width=32, slot_bit_width=32 (matches INMP441 wire format).
     * DMA delivers 32-bit words.  Each word: upper 16 = sample, lower 16 = 0.
     *
     * ESP32-S3 I2S slave RX DMA only fills the ring buffer once after
     * i2s_channel_enable.  Subsequent reads timeout because DMA doesn't
     * cycle.  Workaround: disable+enable RX before each read to restart
     * DMA.  This matches what mic_diag does (which always succeeds).
     * After enable, use a generous read timeout so i2s_channel_read
     * blocks in the DMA semaphore (yielding CPU) until data arrives.
     * The 30ms DMA fill time is handled by the read timeout, not a
     * separate vTaskDelay — this avoids starving IDLE/WDT. */
    i2s_channel_disable(s_rx_handle);
    i2s_channel_enable(s_rx_handle);

    size_t raw_read = 0;
    esp_err_t ret = i2s_channel_read(s_rx_handle, buf, buf_size,
                                      &raw_read, pdMS_TO_TICKS(timeout_ms));

    if (ret == ESP_OK) {
        s_rx_consec_fails = 0;
    } else {
        if (ret == ESP_ERR_TIMEOUT) {
            s_rx_consec_fails++;
            if (s_rx_consec_fails <= 3 || (s_rx_consec_fails % 500 == 0)) {
                ESP_LOGW(TAG, "RX read timeout (consec=%d)", s_rx_consec_fails);
            }
        }
        *bytes_read = 0;
        return ret;
    }

    if (raw_read == 0) {
        *bytes_read = 0;
        return ESP_ERR_TIMEOUT;
    }

    /* Extract upper 16 bits from each 32-bit DMA word (in-place).
     * Hex dump: 00 00 xx xx → int16[0]=pad, int16[1]=sample */
    size_t n_words = raw_read / 4;
    int16_t *s16 = (int16_t *)buf;

    static bool s_dump_done = false;
    if (!s_dump_done && n_words >= 4) {
        ESP_LOGI(TAG, "RX raw int16 pairs: [%d,%d] [%d,%d] [%d,%d] [%d,%d]",
                 s16[0], s16[1], s16[2], s16[3], s16[4], s16[5], s16[6], s16[7]);
        s_dump_done = true;
    }

    for (size_t i = 0; i < n_words; i++) {
        s16[i] = s16[i * 2 + 1];
    }
    *bytes_read = n_words * 2;

    return ESP_OK;
}
