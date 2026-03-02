#pragma once

#include "esp_http_client.h"
#include "esp_err.h"
#include <stdbool.h>

/**
 * Persistent HTTP session for TLS connection reuse.
 *
 * Wraps a single esp_http_client_handle_t that is kept alive across
 * requests so that mbedTLS can resume the TLS session and avoid the
 * ~300–500 ms handshake on every call.
 *
 * Usage:
 *   http_session_init(&s, url, my_event_cb, timeout_ms, buf_rx, buf_tx);
 *   // per call:
 *   http_session_set_ctx(&s, &my_response_buf);
 *   esp_http_client_set_method(s.handle, HTTP_METHOD_POST);
 *   esp_http_client_set_header(s.handle, "Authorization", auth);
 *   esp_http_client_set_post_field(s.handle, body, body_len);
 *   http_session_perform(&s);   // auto-resets + retries on connection drop
 *   int status = esp_http_client_get_status_code(s.handle);
 */
typedef struct {
    esp_http_client_handle_t       handle;
    http_event_handle_cb user_handler; /* real event callback */
    void                          *user_ctx;     /* forwarded as user_data */
    const char                    *url;          /* stable endpoint URL    */
    int                            timeout_ms;
    int                            buf_rx;
    int                            buf_tx;
    bool                           valid;
} http_session_t;

/**
 * Create the persistent HTTP client handle.
 * @param url     Endpoint URL — must remain valid for the session's lifetime.
 * @param handler Event handler called for HTTP_EVENT_ON_DATA etc.
 *                Receives user_ctx (set via http_session_set_ctx) as user_data.
 */
esp_err_t http_session_init(http_session_t *s,
                             const char *url,
                             http_event_handle_cb handler,
                             int timeout_ms, int buf_rx, int buf_tx);

/** Set the context pointer forwarded to the event handler for the next call. */
void http_session_set_ctx(http_session_t *s, void *ctx);

/**
 * Perform the HTTP request.
 * On ESP_ERR_HTTP_CONNECT or ESP_ERR_HTTP_CONNECTION_CLOSED the session is
 * reset and the request is retried once, so callers do not need retry logic
 * for transient connection drops.
 */
esp_err_t http_session_perform(http_session_t *s);

/** Destroy and recreate the underlying handle, preserving URL/config. */
void http_session_reset(http_session_t *s);

/** Release all resources. */
void http_session_deinit(http_session_t *s);
