#include "llm/http_session.h"
#include "wifi/wifi_recovery.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "http_session";

/* ── Proxy event handler ──────────────────────────────────────── */
/*
 * The session registers itself as user_data so it can forward calls to
 * the real handler with the per-call user_ctx.  This lets callers change
 * user_data between requests without recreating the client handle.
 */
static esp_err_t session_event_cb(esp_http_client_event_t *evt)
{
    http_session_t *s = (http_session_t *)evt->user_data;
    if (!s || !s->user_handler) return ESP_OK;

    esp_http_client_event_t proxy = *evt;
    proxy.user_data = s->user_ctx;
    return s->user_handler(&proxy);
}

/* ── Internal handle factory ─────────────────────────────────── */

static esp_http_client_handle_t create_handle(http_session_t *s)
{
    esp_http_client_config_t cfg = {
        .url               = s->url,
        .event_handler     = session_event_cb,
        .user_data         = s,           /* session as user_data for proxy */
        .timeout_ms        = s->timeout_ms,
        .buffer_size       = s->buf_rx,
        .buffer_size_tx    = s->buf_tx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,        /* TCP keep-alive probes */
    };
    return esp_http_client_init(&cfg);
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t http_session_init(http_session_t *s,
                             const char *url,
                             http_event_handle_cb handler,
                             int timeout_ms, int buf_rx, int buf_tx)
{
    if (!s || !url || !handler) return ESP_ERR_INVALID_ARG;

    s->url          = url;
    s->user_handler = handler;
    s->user_ctx     = NULL;
    s->timeout_ms   = timeout_ms;
    s->buf_rx       = buf_rx;
    s->buf_tx       = buf_tx;
    s->valid        = false;
    s->handle       = NULL;

    s->handle = create_handle(s);
    if (!s->handle) {
        ESP_LOGE(TAG, "Failed to create HTTP client for %s", url);
        return ESP_FAIL;
    }

    s->valid = true;
    ESP_LOGI(TAG, "Session ready: %s", url);
    return ESP_OK;
}

void http_session_set_ctx(http_session_t *s, void *ctx)
{
    if (s) s->user_ctx = ctx;
}

esp_err_t http_session_perform(http_session_t *s)
{
    if (!s || !s->valid || !s->handle) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = esp_http_client_perform(s->handle);
    if (ret != ESP_OK) {
        /* Reset session (destroy old handle, create fresh one) so the
         * next caller-initiated request starts with a clean TLS connection.
         *
         * NOTE: We do NOT retry here. The fresh handle has no headers,
         * method, or POST body — only the base URL. Retrying would send
         * a bare GET and get 404. The caller (agent_loop.c) has full
         * retry logic that rebuilds the complete request. */
        ESP_LOGW(TAG, "Request failed (%s), resetting session for next call",
                 esp_err_to_name(ret));
        http_session_reset(s);
        wifi_recovery_record_failure("http_session");
    } else {
        wifi_recovery_record_success("http_session");
    }
    return ret;
}

void http_session_reset(http_session_t *s)
{
    if (!s) return;
    if (s->handle) {
        esp_http_client_cleanup(s->handle);
        s->handle = NULL;
        s->valid  = false;
    }
    s->handle = create_handle(s);
    if (s->handle) {
        s->valid = true;
        ESP_LOGI(TAG, "Session reset: %s", s->url);
    } else {
        ESP_LOGE(TAG, "Session reset failed: %s", s->url);
    }
}

void http_session_deinit(http_session_t *s)
{
    if (!s) return;
    if (s->handle) {
        esp_http_client_cleanup(s->handle);
        s->handle = NULL;
    }
    s->valid = false;
}
