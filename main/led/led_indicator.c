#include "led_indicator.h"
#include "langoustine_config.h"

#include <math.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "hal/gpio_types.h"

static const char *TAG = "led";

/* RMT resolution: 10 MHz → 100 ns / tick */
#define RMT_RESOLUTION_HZ   10000000

/* WS2812 bit timings at 10 MHz (100 ns / tick):
 *   T0H = 400 ns = 4 ticks,  T0L = 800 ns = 8 ticks
 *   T1H = 800 ns = 8 ticks,  T1L = 400 ns = 4 ticks */
#define WS2812_T0H  4
#define WS2812_T0L  8
#define WS2812_T1H  8
#define WS2812_T1L  4

/* Max brightness (0-255): 60 = comfortable for desk use */
#define LED_MAX_BRIGHT  60
/* Flash fade ticks: 30 × 20 ms = 600 ms exponential decay white→off */
#define ANIM_FLASH_FADE_TICKS  30

/* Animation period in ticks (1 tick = 20 ms) */
#define ANIM_PERIOD_TICKS  200  /* 4 s breathing cycle */
#define ANIM_BLINK_ON      10  /* blink on-time: 200 ms */
#define ANIM_BLINK_OFF     40  /* blink off-time: 800 ms (total = 1 s) */
#define ANIM_FAST_ON        3  /* fast flash on: 60 ms */
#define ANIM_FAST_OFF       7  /* fast flash off: 140 ms (total = 200 ms) */
#define ANIM_ERROR_CYCLES  25  /* ~5 s of fast flashing then auto-revert */

static rmt_channel_handle_t s_rmt_chan = NULL;
static rmt_encoder_handle_t s_encoder  = NULL;
static _Atomic led_state_t  s_state    = LED_BOOTING;

/* --- WS2812 bytes encoder -------------------------------------------- */

/* Encode one GRB byte over RMT */
static rmt_bytes_encoder_config_t s_enc_cfg = {
    .bit0 = {
        .duration0 = WS2812_T0H, .level0 = 1,
        .duration1 = WS2812_T0L, .level1 = 0,
    },
    .bit1 = {
        .duration0 = WS2812_T1H, .level0 = 1,
        .duration1 = WS2812_T1L, .level1 = 0,
    },
    .flags.msb_first = 1,
};

static rmt_transmit_config_t s_tx_cfg = {
    .loop_count = 0,
};

/* Send one RGB pixel (internally GRB to the WS2812) */
static void send_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rmt_chan || !s_encoder) return;

    uint8_t grb[3] = { g, r, b };
    /* Fire-and-forget: 20ms task delay >> ~30µs WS2812 frame time.
     * rmt_tx_wait_all_done is intentionally omitted — the done interrupt
     * may be delayed on some GPIO configs and blocking here causes spam. */
    rmt_transmit(s_rmt_chan, s_encoder, grb, sizeof(grb), &s_tx_cfg);
}

/* Scale a 0-255 colour component by brightness 0-255 */
static inline uint8_t scale(uint8_t c, uint8_t bright)
{
    return (uint8_t)(((uint16_t)c * bright) >> 8);
}

/* --- Animation task --------------------------------------------------- */

static void led_task(void *arg)
{
    uint32_t step = 0;
    int      error_count = 0;
    led_state_t prev_state = LED_BOOTING;

    while (1) {
        led_state_t state = atomic_load(&s_state);

        /* Reset step counter on state change */
        if (state != prev_state) {
            step = 0;
            error_count = 0;
            prev_state = state;
        }

        uint8_t r = 0, g = 0, b = 0;

        switch (state) {
        case LED_BOOTING:
        case LED_WIFI: {
            /* Breathing amber: calm "connecting" animation, no alarming red */
            float t = (float)step / ANIM_PERIOD_TICKS;
            float bright = (1.0f - cosf(2.0f * (float)M_PI * t)) * 0.5f;
            uint8_t v = scale(LED_MAX_BRIGHT, (uint8_t)(bright * 255.0f));
            r = v;
            g = v / 3;  /* amber = full red, 1/3 green, no blue */
            break;
        }

        case LED_READY: {
            /* Breathing green: brightness = (1 - cos(2π·step/period)) / 2 */
            float t = (float)step / ANIM_PERIOD_TICKS;
            float bright = (1.0f - cosf(2.0f * (float)M_PI * t)) * 0.5f;
            g = scale(LED_MAX_BRIGHT, (uint8_t)(bright * 255.0f));
            break;
        }

        case LED_THINKING: {
            /* Pulsing blue */
            float t = (float)step / ANIM_PERIOD_TICKS;
            float bright = (1.0f - cosf(2.0f * (float)M_PI * t)) * 0.5f;
            b = scale(LED_MAX_BRIGHT, (uint8_t)(bright * 255.0f));
            break;
        }

        case LED_SPEAKING: {
            /* Pulsing cyan */
            float t = (float)step / ANIM_PERIOD_TICKS;
            float bright = (1.0f - cosf(2.0f * (float)M_PI * t)) * 0.5f;
            g = scale(LED_MAX_BRIGHT, (uint8_t)(bright * 255.0f));
            b = scale(LED_MAX_BRIGHT, (uint8_t)(bright * 255.0f));
            break;
        }

        case LED_LISTENING: {
            /* Violet flash at 15% brightness — mic active, recording.
             * Violet = R:B ≈ 1:2 (similar to 148:255). 15% of LED_MAX_BRIGHT=60 → ~9 peak.
             * 200 ms on / 800 ms off (1 s cycle) — clearly visible, not anxious. */
            uint32_t phase = step % (ANIM_BLINK_ON + ANIM_BLINK_OFF);
            if (phase < ANIM_BLINK_ON) {
                r = 5;   /* ~55% of blue — gives violet hue */
                g = 0;
                b = 9;   /* 15% of LED_MAX_BRIGHT */
            }
            break;
        }

        case LED_ERROR: {
            /* Fast red flash, auto-revert to READY after ANIM_ERROR_CYCLES */
            uint32_t phase = step % (ANIM_FAST_ON + ANIM_FAST_OFF);
            if (phase < ANIM_FAST_ON) {
                r = LED_MAX_BRIGHT;
            }
            /* Count flash completions */
            if (step > 0 && (step % (ANIM_FAST_ON + ANIM_FAST_OFF) == 0)) {
                error_count++;
            }
            if (error_count >= ANIM_ERROR_CYCLES) {
                atomic_store(&s_state, LED_READY);
            }
            break;
        }

        case LED_OTA: {
            /* Rapid magenta flash (R+B) — OTA download in progress */
            uint32_t phase = step % (ANIM_FAST_ON + ANIM_FAST_OFF);
            if (phase < ANIM_FAST_ON) {
                r = LED_MAX_BRIGHT;
                b = LED_MAX_BRIGHT;
            }
            break;
        }

        case LED_CAPTURING: {
            /* Maximum white during capture — agent transitions to LED_FLASH_FADE when done.
             * No auto-revert: the vision API call can take up to 30 s. */
            r = 255; g = 255; b = 255;
            break;
        }

        case LED_FLASH_FADE: {
            /* Post-capture afterglow: exponential decay from 255→0 over ~600 ms.
             * Mimics a traditional camera flash fading after the shutter closes. */
            if (step < ANIM_FLASH_FADE_TICKS) {
                /* e^(-5t) decay mapped to 0..1 over the fade window */
                float t = (float)step / ANIM_FLASH_FADE_TICKS;
                float v = expf(-5.0f * t);
                uint8_t bv = (uint8_t)(v * 255.0f);
                r = bv; g = bv; b = bv;
            } else {
                r = 0; g = 0; b = 0;
                atomic_store(&s_state, LED_THINKING);
            }
            break;
        }
        }

        send_pixel(r, g, b);
        step++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* --- Public API ------------------------------------------------------- */

void led_indicator_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num        = LANG_LED_GPIO,
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return;
    }

    err = rmt_new_bytes_encoder(&s_enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(err));
        return;
    }

    err = rmt_enable(s_rmt_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        return;
    }

    /* Start with booting state */
    atomic_store(&s_state, LED_BOOTING);

    /* Suppress verbose RMT driver logs (interrupt timing noise on some GPIOs) */
    esp_log_level_set("rmt", ESP_LOG_WARN);

    xTaskCreatePinnedToCore(led_task, "led", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "LED indicator init on GPIO %d", LANG_LED_GPIO);
}

void led_indicator_set(led_state_t state)
{
    atomic_store(&s_state, state);
}
