#pragma once

#include <stddef.h>

#define LOG_RING_SIZE  (8 * 1024)

/**
 * Install a vprintf hook that captures ESP_LOG output into an 8KB PSRAM ring
 * buffer while continuing to write to UART unchanged.
 */
void log_buffer_init(void);

/**
 * Copy the ring buffer contents into `out` (NUL-terminated).
 * At most `max_len - 1` bytes are written.
 */
void log_buffer_get(char *out, size_t max_len);
