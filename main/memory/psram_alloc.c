#include "psram_alloc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "psram_alloc";

void *ps_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "PSRAM alloc %u failed, falling back to SRAM", (unsigned)size);
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

void *ps_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "PSRAM calloc %u failed, falling back to SRAM", (unsigned)total);
        p = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (p) memset(p, 0, total);
    return p;
}

void *ps_realloc(void *ptr, size_t size)
{
    /* Try PSRAM first; if the original was SRAM this will migrate it */
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "PSRAM realloc %u failed, falling back to SRAM", (unsigned)size);
        p = heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

void *int_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *int_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) memset(p, 0, total);
    return p;
}
