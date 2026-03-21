#pragma once

/**
 * SSD1306 — Minimal 128x64 OLED driver over I2C
 *
 * Uses a 1KB framebuffer (allocated from PSRAM) and bulk-transfers
 * the entire buffer on each refresh.  No partial updates — simple
 * and fast enough at 400 kHz I2C for a 2 Hz status display.
 */

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

/** Initialise the SSD1306 on an existing I2C master bus. */
esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, uint8_t addr);

/** Clear the framebuffer (all pixels off). */
void ssd1306_clear(void);

/** Flush the framebuffer to the display. */
esp_err_t ssd1306_refresh(void);

/** Draw a single pixel. */
void ssd1306_pixel(int x, int y, bool on);

/** Draw a character at (x, y) using the built-in 6x8 font.
 *  Returns the x advance (6 pixels). */
int ssd1306_char(int x, int y, char c);

/** Draw a NUL-terminated string.  Returns final x position. */
int ssd1306_str(int x, int y, const char *s);

/** Draw a string with 2x vertical scaling (6x16 glyphs). */
int ssd1306_str_2x(int x, int y, const char *s);

/** Fill a rectangular region (x, y, w, h) with on/off. */
void ssd1306_fill_rect(int x, int y, int w, int h, bool on);

/** Draw a horizontal line. */
void ssd1306_hline(int x, int y, int w, bool on);

/** Set display contrast (0-255). */
esp_err_t ssd1306_set_contrast(uint8_t val);
