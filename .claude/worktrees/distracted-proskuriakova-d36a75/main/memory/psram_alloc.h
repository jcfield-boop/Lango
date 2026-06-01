#pragma once
#include <stddef.h>

/**
 * PSRAM allocation wrappers for Langoustine.
 *
 * Use ps_malloc/ps_calloc for buffers > 4KB (audio ring, LLM stream, TTS
 * cache, cJSON trees, context builder, multipart bodies).
 * Use int_malloc/int_calloc for FreeRTOS objects, task stacks, DMA buffers,
 * and any allocation that must stay in internal SRAM.
 * Use standard free() for both.
 */

/** Allocate from PSRAM; falls back to SRAM and logs a warning. */
void *ps_malloc(size_t size);

/** Allocate from PSRAM, zero-initialised; falls back to SRAM. */
void *ps_calloc(size_t n, size_t size);

/** Reallocate a PSRAM buffer; falls back to SRAM. */
void *ps_realloc(void *ptr, size_t size);

/** Allocate from internal SRAM only (DMA, FreeRTOS objects). */
void *int_malloc(size_t size);

/** Allocate from internal SRAM only, zero-initialised. */
void *int_calloc(size_t n, size_t size);
