#include "log_buffer.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_heap_caps.h"

static char           *s_ring    = NULL;
static size_t          s_head    = 0;
static size_t          s_fill    = 0;
static vprintf_like_t  s_orig    = NULL;

/* Static format buffer: zero stack impact in the hook.
 * Protected by a simple atomic flag — concurrent callers skip the ring write
 * rather than racing on the buffer (worst case: one log line is dropped). */
static char            s_tmp[256];
static _Atomic int     s_busy    = 0;

static int log_vprintf_hook(const char *fmt, va_list args)
{
    /* va_copy so the original args are still intact for s_orig below */
    va_list args_copy;
    va_copy(args_copy, args);

    int n = 0;
    /* Try to acquire the static buffer (non-blocking) */
    if (s_ring && __atomic_exchange_n(&s_busy, 1, __ATOMIC_ACQUIRE) == 0) {
        n = vsnprintf(s_tmp, sizeof(s_tmp), fmt, args_copy);
        if (n > 0) {
            size_t copy = (n < (int)sizeof(s_tmp)) ? (size_t)n : sizeof(s_tmp) - 1;
            for (size_t i = 0; i < copy; i++) {
                s_ring[s_head] = s_tmp[i];
                s_head = (s_head + 1) % LOG_RING_SIZE;
            }
            if (s_fill < LOG_RING_SIZE) {
                s_fill += copy;
                if (s_fill > LOG_RING_SIZE) s_fill = LOG_RING_SIZE;
            }
        }
        __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
    }
    va_end(args_copy);

    /* Always forward to the original output fn (libc vprintf → UART) */
    if (s_orig) return s_orig(fmt, args);
    return n;
}

void log_buffer_init(void)
{
    s_ring = heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring) return;
    memset(s_ring, 0, LOG_RING_SIZE);
    s_orig = esp_log_set_vprintf(log_vprintf_hook);
}

void log_buffer_get(char *out, size_t max_len)
{
    if (!out || max_len == 0) return;
    out[0] = '\0';
    if (!s_ring) return;

    /* Acquire the same busy flag used by the writer for a coherent snapshot */
    while (__atomic_exchange_n(&s_busy, 1, __ATOMIC_ACQUIRE) != 0) { /* spin */ }
    size_t head = s_head;
    size_t fill = s_fill;
    __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);

    if (fill == 0) return;
    size_t start = (fill < LOG_RING_SIZE) ? 0 : head;
    size_t copy  = (fill < max_len - 1) ? fill : max_len - 1;
    for (size_t i = 0; i < copy; i++) {
        out[i] = s_ring[(start + i) % LOG_RING_SIZE];
    }
    out[copy] = '\0';
}

void log_buffer_clear(void)
{
    if (!s_ring) return;
    while (__atomic_exchange_n(&s_busy, 1, __ATOMIC_ACQUIRE) != 0) { /* spin */ }
    memset(s_ring, 0, LOG_RING_SIZE);
    s_head = 0;
    s_fill = 0;
    __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
}
