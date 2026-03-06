#pragma once

/**
 * LED Indicator — WS2812 NeoPixel status LED via RMT
 *
 * States:
 *   LED_BOOTING    — solid red         (power-on, init)
 *   LED_WIFI       — slow yellow blink (connecting to WiFi)
 *   LED_READY      — breathing green   (idle, waiting for input)
 *   LED_THINKING   — pulsing blue      (LLM call in progress)
 *   LED_SPEAKING   — pulsing cyan      (TTS/I2S playback)
 *   LED_LISTENING  — pulsing white     (mic active: PTT held or wake word)
 *   LED_ERROR      — fast red flash    (auto-reverts to READY after ~5s)
 *   LED_OTA        — rapid magenta flash (OTA download in progress)
 *
 * GPIO configured via LANG_LED_GPIO in langoustine_config.h (default: 48).
 */

typedef enum {
    LED_BOOTING    = 0,
    LED_WIFI       = 1,
    LED_READY      = 2,
    LED_THINKING   = 3,
    LED_SPEAKING   = 4,
    LED_ERROR      = 5,
    LED_LISTENING  = 6,
    LED_OTA        = 7,   /* rapid magenta flash: OTA download in progress */
} led_state_t;

/**
 * Initialize the WS2812 RMT driver and start the animation task.
 * Initial state is LED_BOOTING.
 */
void led_indicator_init(void);

/**
 * Set the current LED state (thread-safe, callable from any task/ISR).
 */
void led_indicator_set(led_state_t state);
