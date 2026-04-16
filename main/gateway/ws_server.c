#include "ws_server.h"
#include "langoustine_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "tools/tool_web_search.h"
#include "heartbeat/heartbeat.h"
#include "ota/ota_manager.h"
#include "cron/cron_service.h"
#include "audio/audio_pipeline.h"
#include "audio/stt_client.h"
#include "audio/tts_client.h"
#include "audio/uac_microphone.h"
#include "audio/i2s_audio.h"
#include "audio/wake_word.h"
#include "config/services_config.h"
#include "tools/tool_say.h"
#include "mcp/mcp_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "mbedtls/md.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "agent/agent_loop.h"
#include "tools/tool_device_temp.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "memory/psram_alloc.h"
#include "diag/log_buffer.h"

static const char *TAG = "ws";

#define WS_NVS_NAMESPACE "ws_config"
#define WS_NVS_KEY_VERBOSE "verbose_logs"
static bool s_verbose_logs = false;

/* CHANGE 5: auth token */
#define WS_NVS_KEY_AUTH_TOKEN "auth_token"
static char s_auth_token[128] = {0};

/* CHANGE 6: CORS origin */
#define WS_NVS_KEY_CORS_ORIGIN "cors_origin"
static char s_cors_origin[128] = "*";

static httpd_handle_t s_server = NULL;
static TaskHandle_t s_ws_ping_task = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
    int64_t last_req_us;   /* for rate limiting */
    uint8_t ping_fail_count; /* consecutive WS ping failures */
} ws_client_t;

static ws_client_t s_clients[LANG_WS_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_lock;  /* SRAM mutex — protects s_clients[] */
static SemaphoreHandle_t s_send_lock;     /* SRAM mutex — serializes httpd_ws_send_frame_async calls */
static esp_err_t send_frame_locked(int fd, httpd_ws_frame_t *frame); /* forward decl */

/* ── PSRAM monitor event ring (16KB) — persists across WS disconnects ── */
#define MONITOR_RING_SIZE  (16 * 1024)
static char   *s_monitor_ring = NULL;
static size_t  s_monitor_head = 0;
static size_t  s_monitor_fill = 0;

static void monitor_ring_append(const char *line, size_t len)
{
    if (!s_monitor_ring || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        s_monitor_ring[s_monitor_head] = line[i];
        s_monitor_head = (s_monitor_head + 1) % MONITOR_RING_SIZE;
    }
    /* Newline separator */
    s_monitor_ring[s_monitor_head] = '\n';
    s_monitor_head = (s_monitor_head + 1) % MONITOR_RING_SIZE;
    s_monitor_fill += len + 1;
    if (s_monitor_fill > MONITOR_RING_SIZE) s_monitor_fill = MONITOR_RING_SIZE;
}

static void monitor_ring_get(char *out, size_t max_len)
{
    if (!out || max_len == 0) return;
    out[0] = '\0';
    if (!s_monitor_ring || s_monitor_fill == 0) return;
    size_t start = (s_monitor_fill < MONITOR_RING_SIZE) ? 0 : s_monitor_head;
    size_t copy  = (s_monitor_fill < max_len - 1) ? s_monitor_fill : max_len - 1;
    for (size_t i = 0; i < copy; i++) {
        out[i] = s_monitor_ring[(start + i) % MONITOR_RING_SIZE];
    }
    out[copy] = '\0';
}

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGW(TAG, "Duplicate fd=%d, replacing client slot", fd);
            return &s_clients[i];
        }
    }
    for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            s_clients[i].last_req_us = 0;
            s_clients[i].ping_fail_count = 0;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static void remove_client(int fd)
{
    for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            return;
        }
    }
}

/* Periodic WS ping — keeps browser connections alive through the recv_wait_timeout */
#define WS_PING_INTERVAL_MS  20000

static void ws_ping_task(void *arg)
{
    static const uint8_t ping_payload[] = "ping";
    httpd_ws_frame_t ping_pkt = {
        .type    = HTTPD_WS_TYPE_PING,
        .payload = (uint8_t *)ping_payload,
        .len     = sizeof(ping_payload) - 1,
        .final   = true,
    };

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WS_PING_INTERVAL_MS));
        if (!s_server) continue;

        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        int fds[LANG_WS_MAX_CLIENTS];
        int count = 0;
        for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
            if (s_clients[i].active) fds[count++] = s_clients[i].fd;
        }
        xSemaphoreGive(s_clients_lock);

        for (int i = 0; i < count; i++) {
            esp_err_t err = send_frame_locked(fds[i], &ping_pkt);
            xSemaphoreTake(s_clients_lock, portMAX_DELAY);
            ws_client_t *c = find_client_by_fd(fds[i]);
            if (c) {
                if (err == ESP_OK) {
                    c->ping_fail_count = 0;
                } else {
                    c->ping_fail_count++;
                    if (c->ping_fail_count >= 3) {
                        ESP_LOGW(TAG, "ping fd=%d failed %d times, removing",
                                 fds[i], c->ping_fail_count);
                        remove_client(fds[i]);
                    } else {
                        ESP_LOGD(TAG, "ping fd=%d failed (%s), attempt %d/3",
                                 fds[i], esp_err_to_name(err), c->ping_fail_count);
                    }
                }
            }
            xSemaphoreGive(s_clients_lock);
        }
    }
}

/* Serialized WebSocket send — takes s_send_lock to prevent concurrent
 * SSL writes from ping task + outbound dispatch → double-free crash. */
static esp_err_t send_frame_locked(int fd, httpd_ws_frame_t *frame)
{
    if (!s_send_lock) return httpd_ws_send_frame_async(s_server, fd, frame);
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_err_t ret = httpd_ws_send_frame_async(s_server, fd, frame);
    xSemaphoreGive(s_send_lock);
    return ret;
}

/* Send a JSON status frame to a specific client */
static esp_err_t send_json_to_fd(int fd, const char *json_str)
{
    if (!s_server || !json_str) return ESP_ERR_INVALID_ARG;
    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };
    return send_frame_locked(fd, &pkt);
}

/* ── CHANGE 5: auth helper ──────────────────────────────────── */

static bool request_is_authed(httpd_req_t *req)
{
    if (s_auth_token[0] == '\0') return true; /* auth disabled when token not set */
    char hdr[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        if (strncmp(hdr, "Bearer ", 7) == 0 && strcmp(hdr + 7, s_auth_token) == 0) {
            return true;
        }
    }
    char query[256] = {0};
    char token_val[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "token", token_val, sizeof(token_val)) == ESP_OK) {
            if (strcmp(token_val, s_auth_token) == 0) return true;
        }
    }
    return false;
}

/* ── CHANGE 6: CORS helper ──────────────────────────────────── */

static void apply_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  s_cors_origin);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age",       "86400");
}

/* ── CHANGE 5: set_auth_token public function ───────────────── */

void ws_server_set_auth_token(const char *token)
{
    if (!token) return;
    strncpy(s_auth_token, token, sizeof(s_auth_token) - 1);
    s_auth_token[sizeof(s_auth_token) - 1] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(WS_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, WS_NVS_KEY_AUTH_TOKEN, token);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "HTTP auth token saved");
    }
}

/* ── CHANGE 6: set_cors_origin public function ──────────────── */

void ws_server_set_cors_origin(const char *origin)
{
    if (!origin) return;
    strncpy(s_cors_origin, origin, sizeof(s_cors_origin) - 1);
    s_cors_origin[sizeof(s_cors_origin) - 1] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(WS_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, WS_NVS_KEY_CORS_ORIGIN, origin);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "CORS origin set to: %s", origin);
    }
}

/* ── Public send helpers ────────────────────────────────────── */

esp_err_t ws_server_send_status(const char *chat_id, const char *stage)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    ws_client_t *c = find_client_by_chat_id(chat_id);
    int fd = c ? c->fd : -1;
    xSemaphoreGive(s_clients_lock);
    if (fd < 0) return ESP_ERR_NOT_FOUND;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "status");
    cJSON_AddStringToObject(j, "stage", stage);
    cJSON_AddStringToObject(j, "chat_id", chat_id);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t ret = send_json_to_fd(fd, s);
    free(s);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(fd);
        xSemaphoreGive(s_clients_lock);
    }
    return ret;
}

esp_err_t ws_server_send_error(const char *chat_id, const char *code, const char *message)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    ws_client_t *c = find_client_by_chat_id(chat_id);
    int fd = c ? c->fd : -1;
    xSemaphoreGive(s_clients_lock);
    if (fd < 0) return ESP_ERR_NOT_FOUND;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "error");
    cJSON_AddStringToObject(j, "code", code ? code : "error");
    cJSON_AddStringToObject(j, "message", message ? message : "");
    cJSON_AddStringToObject(j, "chat_id", chat_id);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t ret = send_json_to_fd(fd, s);
    free(s);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(fd);
        xSemaphoreGive(s_clients_lock);
    }
    return ret;
}

esp_err_t ws_server_send_token(const char *chat_id, const char *delta)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    ws_client_t *c = find_client_by_chat_id(chat_id);
    int fd = c ? c->fd : -1;
    xSemaphoreGive(s_clients_lock);
    if (fd < 0) return ESP_ERR_NOT_FOUND;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type", "token");
    cJSON_AddStringToObject(j, "content", delta ? delta : "");
    cJSON_AddStringToObject(j, "chat_id", chat_id);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return ESP_ERR_NO_MEM;
    esp_err_t ret = send_json_to_fd(fd, s);
    free(s);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(fd);
        xSemaphoreGive(s_clients_lock);
    }
    return ret;
}

/* ── WebSocket handler ──────────────────────────────────────── */

/* Send a small JSON error frame synchronously from within the WS receive handler. */
static void ws_send_busy(httpd_req_t *req)
{
    const char *err = "{\"type\":\"error\",\"message\":\"Device busy — please retry\"}";
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT,
                            .payload = (uint8_t *)err, .len = strlen(err) };
    httpd_ws_send_frame(req, &f);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        if (!request_is_authed(req)) {
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
            return ESP_FAIL;
        }
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        add_client(fd);
        xSemaphoreGive(s_clients_lock);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(httpd_req_to_sockfd(req));
        xSemaphoreGive(s_clients_lock);
        return ret;
    }
    /* Silently drop PONG frames (client responses to our 20 s ping) and any
     * incoming PING frames.  handle_ws_control_frames=true routes them here.
     * CRITICAL: httpd_ws_recv_frame(max_len=0) only reads the WS frame header —
     * the payload bytes remain in the TCP socket buffer.  We MUST drain them
     * before returning or they corrupt the next frame's header parse, causing
     * a recv error and connection drop after 1-2 ping cycles (~40-60 s).
     * Use heap allocation (not stack) to avoid any stack-overflow risk in the
     * HTTPS server's httpd task. */
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG || ws_pkt.type == HTTPD_WS_TYPE_PING) {
        if (ws_pkt.len > 0 && ws_pkt.len <= 125) {  /* RFC 6455 §5.5 guard */
            uint8_t *drain = malloc(ws_pkt.len);
            if (drain) {
                ws_pkt.payload = drain;
                httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
                ws_pkt.payload = NULL;
                free(drain);
            }
        }
        return ESP_OK;
    }

    /* CLOSE frame: browsers always include a 2-byte status code (RFC 6455 §5.5.1),
     * so ws_pkt.len is typically 2 (or more with a reason string), NOT 0.
     * The previous check only handled the len==0 case, causing CLOSE frames with
     * a status code to fall through to the text handler, get logged as "Invalid
     * JSON", and leave the client slot active for up to 20 s (until next ping).
     * That made rapid refresh fill all 4 slots with stale entries → new connections
     * rejected. Fix: check type first, drain payload, always call remove_client. */
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (ws_pkt.len > 0 && ws_pkt.len <= 125) {
            uint8_t *drain = malloc(ws_pkt.len);
            if (drain) {
                ws_pkt.payload = drain;
                httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
                ws_pkt.payload = NULL;
                free(drain);
            }
        }
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(httpd_req_to_sockfd(req));
        xSemaphoreGive(s_clients_lock);
        return ESP_OK;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK;
    }

    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    ws_client_t *client = find_client_by_fd(fd);
    xSemaphoreGive(s_clients_lock);

    /* Handle binary audio frames */
    if (ws_pkt.type == HTTPD_WS_TYPE_BINARY) {
        if (ws_pkt.len > LANG_WS_MAX_AUDIO_FRAME) {
            ESP_LOGW(TAG, "Audio frame too large (%u bytes), rejecting fd=%d",
                     (unsigned)ws_pkt.len, fd);
            if (client) ws_server_send_error(client->chat_id, "audio_overflow", "Frame too large");
            return ESP_ERR_INVALID_ARG;
        }
        /* Allocate payload from PSRAM */
        ws_pkt.payload = heap_caps_malloc(ws_pkt.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ws_pkt.payload) {
            ws_pkt.payload = malloc(ws_pkt.len + 1);
        }
        if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            audio_ring_append(ws_pkt.payload, ws_pkt.len);
        }
        free(ws_pkt.payload);
        return ret;
    }

    /* Text frames: enforce size limit */
    if (ws_pkt.len > 8 * 1024) {
        ESP_LOGW(TAG, "WS text frame too large (%u bytes), rejecting fd=%d",
                 (unsigned)ws_pkt.len, fd);
        return ESP_ERR_INVALID_ARG;
    }

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    free(ws_pkt.payload);

    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        return ESP_OK;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *content_item = cJSON_GetObjectItem(root, "content");
    cJSON *chat_id_item = cJSON_GetObjectItem(root, "chat_id");
    cJSON *mime_item = cJSON_GetObjectItem(root, "mime");

    /* Validate field lengths */
    if (type_item && cJSON_IsString(type_item) &&
        strnlen(type_item->valuestring, 33) > 32) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    const char *msg_type = (type_item && cJSON_IsString(type_item))
                           ? type_item->valuestring : "";
    const char *chat_id = (client) ? client->chat_id : "ws_unknown";

    /* Update chat_id from message if provided */
    if (chat_id_item && cJSON_IsString(chat_id_item)) {
        size_t cid_len = strnlen(chat_id_item->valuestring, 32);
        if (cid_len > 0 && cid_len <= 31) {
            chat_id = chat_id_item->valuestring;
            if (client) {
                xSemaphoreTake(s_clients_lock, portMAX_DELAY);
                ws_client_t *c2 = find_client_by_fd(fd);
                if (c2) {
                    strncpy(c2->chat_id, chat_id, sizeof(c2->chat_id) - 1);
                    client = c2;
                }
                xSemaphoreGive(s_clients_lock);
            }
        }
    }

    /* Rate limiting: LANG_RATE_LIMIT_RPM requests per minute */
    if (client && (strcmp(msg_type, "prompt") == 0 || strcmp(msg_type, "audio_end") == 0)) {
        int64_t now_us = esp_timer_get_time();
        int64_t min_gap_us = (int64_t)(60000000LL / LANG_RATE_LIMIT_RPM);
        if (client->last_req_us > 0 && (now_us - client->last_req_us) < min_gap_us) {
            ESP_LOGW(TAG, "Rate limit hit for %s", chat_id);
            ws_server_send_error(chat_id, "rate_limited", "Too many requests");
            cJSON_Delete(root);
            return ESP_OK;
        }
        client->last_req_us = now_us;
    }

    if (strcmp(msg_type, "prompt") == 0 &&
        content_item && cJSON_IsString(content_item)) {

        size_t content_len = strnlen(content_item->valuestring, 4097);
        if (content_len > 4096) {
            cJSON_Delete(root);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "WS prompt from %s: %.40s...", chat_id, content_item->valuestring);

        lang_msg_t msg = {0};
        strncpy(msg.channel, LANG_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
        msg.content = strdup(content_item->valuestring);
        if (msg.content) {
            if (message_bus_push_inbound(&msg) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound bus full, drop WS prompt from %s", chat_id);
                free(msg.content);
                ws_send_busy(req);
            }
        }

    } else if (strcmp(msg_type, "message") == 0 &&
               content_item && cJSON_IsString(content_item)) {
        /* Legacy "message" type — map to "prompt" for compatibility */
        size_t content_len = strnlen(content_item->valuestring, 4097);
        if (content_len <= 4096) {
            lang_msg_t msg = {0};
            strncpy(msg.channel, LANG_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
            msg.content = strdup(content_item->valuestring);
            if (msg.content) {
                if (message_bus_push_inbound(&msg) != ESP_OK) {
                    ESP_LOGW(TAG, "Inbound bus full, drop WS prompt from %s", chat_id);
                    free(msg.content);
                    ws_send_busy(req);
                }
            }
        }

    } else if (strcmp(msg_type, "audio_start") == 0) {
        const char *mime = (mime_item && cJSON_IsString(mime_item) &&
                            strnlen(mime_item->valuestring, 64) <= 63)
                           ? mime_item->valuestring
                           : "audio/webm;codecs=opus";
        esp_err_t err = audio_ring_open(chat_id, mime, LANG_CHAN_WEBSOCKET);
        if (err != ESP_OK) {
            ws_server_send_error(chat_id, "audio_overflow", "Audio ring busy");
        }

    } else if (strcmp(msg_type, "audio_end") == 0) {
        audio_ring_commit(chat_id);

    } else if (strcmp(msg_type, "audio_abort") == 0) {
        audio_ring_reset_for_client(chat_id);

    } else if (strcmp(msg_type, "ping") == 0) {
        /* Application-layer pong — keeps client reconnect logic happy */
        const char *pong = "{\"type\":\"pong\"}";
        send_json_to_fd(fd, pong);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── GET / → voice UI ─────────────────────────────────────────── */

/* / now serves dev.html — the web UI is developer-only.
 * Primary user interface is Telegram. */
static esp_err_t voice_ui_handler(httpd_req_t *req)
{
    const char *path = "/lfs/console/dev.html";
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h1>Langoustine</h1>"
                                "<p>dev.html not found. Upload to /lfs/console/.</p></body></html>");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /console → dev console ──────────────────────────────── */

static esp_err_t dev_console_handler(httpd_req_t *req)
{
    const char *path = "/lfs/console/dev.html";
    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, "<html><body><h1>Developer Console</h1>"
                                "<p>dev.html not found. Upload to /lfs/console/.</p></body></html>");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /health ──────────────────────────────────────────────── */

static esp_err_t health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true,\"firmware\":\"langoustine\"}");
    return ESP_OK;
}

/* ── GET /tts/<id> — serve cached audio from PSRAM ────────────── */

static esp_err_t tts_serve_handler(httpd_req_t *req)
{
    /* Extract the ID from the URI: /tts/<8-hex-char-id> */
    const char *uri = req->uri;
    const char *id_start = uri + 5;  /* skip "/tts/" */

    /* Validate: exactly 8 hex characters */
    size_t id_len = strlen(id_start);
    if (id_len != 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid TTS ID");
        return ESP_OK;
    }
    for (size_t i = 0; i < 8; i++) {
        char c = id_start[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid TTS ID");
            return ESP_OK;
        }
    }

    char id[9];
    memcpy(id, id_start, 8);
    id[8] = '\0';

    const uint8_t *buf = NULL;
    size_t len = 0;
    if (tts_cache_get(id, &buf, &len) != ESP_OK || !buf || len == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "TTS not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    apply_cors(req);

    /* Send in 4KB chunks */
    const size_t CHUNK = 4096;
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = (len - sent > CHUNK) ? CHUNK : (len - sent);
        if (httpd_resp_send_chunk(req, (const char *)(buf + sent), (ssize_t)chunk) != ESP_OK) break;
        sent += chunk;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /camera/latest.jpg — serve last captured webcam image ── */

static esp_err_t camera_latest_handler(httpd_req_t *req)
{
    FILE *f = fopen(LANG_CAMERA_CAPTURE_PATH, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No image captured yet");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    apply_cors(req);

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── File name → LittleFS path mapping ──────────────────────── */

static const char *name_to_path(const char *name)
{
    if (name && strcmp(name, "soul")          == 0) return LANG_SOUL_FILE;
    if (name && strcmp(name, "user")          == 0) return LANG_USER_FILE;
    if (name && strcmp(name, "memory")        == 0) return LANG_MEMORY_FILE;
    if (name && strcmp(name, "heartbeat")     == 0) return LANG_HEARTBEAT_FILE;
    if (name && strcmp(name, "services")      == 0) return "/lfs/config/SERVICES.md";
    if (name && strcmp(name, "cron")          == 0) return "/lfs/cron.json";
    if (name && strcmp(name, "console-index") == 0) return "/lfs/console/index.html";
    if (name && strcmp(name, "console-dev")   == 0) return "/lfs/console/dev.html";
    if (name && strcmp(name, "quickactions")  == 0) return "/lfs/config/quickactions.json";
    if (name && strcmp(name, "crashlog")      == 0) return "/lfs/memory/crashlog.md";
    return NULL;
}

/* ── GET /api/file ───────────────────────────────────────────── */

static esp_err_t file_get_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char query[128] = {0};
    char name[16]   = {0};
    char raw_path[96] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
        httpd_query_key_value(query, "path", raw_path, sizeof(raw_path));
    }

    const char *path = name_to_path(name);

    /* Fallback: if name lookup failed, accept an explicit /lfs/ path */
    if (!path && raw_path[0]) {
        if (strncmp(raw_path, "/lfs/", 5) != 0 || strstr(raw_path, "..")) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Path not allowed");
            return ESP_OK;
        }
        path = raw_path;
    }

    if (!path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown file name or path");
        return ESP_OK;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ── GET / DELETE /api/crashlog ───────────────────────────────── */
/* Convenience endpoints for the crashlog file written by log_crash_if_needed()
 * in main/langoustine.c on abnormal reset (panic/wdt/brownout). GET returns
 * raw markdown (same as /api/file?name=crashlog) but with no-store caching
 * and a friendly empty response. DELETE truncates the file to empty. */

static esp_err_t crashlog_get_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    apply_cors(req);

    FILE *f = fopen("/lfs/memory/crashlog.md", "r");
    if (!f) {
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t crashlog_delete_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    /* Truncate by opening in write mode with no content. If the file doesn't
     * exist this still succeeds (returns "cleared" semantics). */
    FILE *f = fopen("/lfs/memory/crashlog.md", "w");
    if (f) fclose(f);

    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true,\"cleared\":true}");
    return ESP_OK;
}

/* ── POST /api/file ──────────────────────────────────────────── */

static esp_err_t file_post_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char query[128] = {0};
    char name[16]   = {0};
    char raw_path[96] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
        httpd_query_key_value(query, "path", raw_path, sizeof(raw_path));
    }

    const char *path = name_to_path(name);

    /* Fallback: if name lookup failed, accept an explicit /lfs/ path */
    if (!path && raw_path[0]) {
        /* Security: must start with /lfs/, no ".." traversal, block /lfs/sessions/ */
        if (strncmp(raw_path, "/lfs/", 5) != 0 || strstr(raw_path, "..") ||
            strncmp(raw_path, "/lfs/sessions/", 14) == 0) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Path not allowed");
            return ESP_OK;
        }
        /* Auto-create parent directory (LittleFS single-level) */
        char dir[96];
        strncpy(dir, raw_path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
            mkdir(dir, 0755);
        }
        path = raw_path;
    }

    if (!path) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown file name or path");
        return ESP_OK;
    }

    size_t max_body = 48 * 1024;
    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < max_body)
                      ? (size_t)req->content_len : max_body;

    char *body = (body_len > 4096) ? ps_malloc(body_len + 1) : malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    /* Loop: mbedTLS returns at most one TLS record (~16KB) per call */
    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, body + received, body_len - received);
        if (r <= 0) break;
        received += (size_t)r;
    }
    if (received == 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body received");
        return ESP_OK;
    }
    body[received] = '\0';

    /* Check LittleFS free space before writing */
    {
        size_t lfs_total = 0, lfs_used = 0;
        esp_littlefs_info("littlefs", &lfs_total, &lfs_used);
        if (lfs_total > 0 && lfs_used + (size_t)received > lfs_total) {
            free(body);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Insufficient LittleFS space");
            return ESP_OK;
        }
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    if (fputs(body, f) == EOF) {
        fclose(f);
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    fclose(f);
    free(body);

    ESP_LOGI(TAG, "Saved %s (%d bytes)", path, received);

    /* If SERVICES.md was saved, hot-reload all API keys immediately */
    if (strcmp(name, "services") == 0) {
        services_config_reload();
    }

    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /api/reboot ────────────────────────────────────────── */

static void reboot_timer_cb(void *arg) { esp_restart(); }

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    esp_timer_handle_t t;
    esp_timer_create_args_t args = { .callback = reboot_timer_cb, .name = "reboot" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, 500 * 1000);
    }
    return ESP_OK;
}

/* ── POST /api/heartbeat ─────────────────────────────────────── */

static esp_err_t heartbeat_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    httpd_resp_set_type(req, "application/json");
    bool triggered = heartbeat_trigger();
    httpd_resp_sendstr(req, triggered ? "{\"ok\":true,\"triggered\":true}"
                                      : "{\"ok\":true,\"triggered\":false}");
    return ESP_OK;
}

/* ── Skill helpers ───────────────────────────────────────────── */

static bool skill_name_valid(const char *name)
{
    if (!name || name[0] == '\0') return false;
    size_t len = strlen(name);
    if (len > 48) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

static void skill_name_to_path(const char *name, char *path, size_t path_size)
{
    snprintf(path, path_size, "%s%s.md", LANG_SKILLS_PREFIX, name);
}

/* GET /api/skills */
static esp_err_t skills_list_handler(httpd_req_t *req)
{
    DIR *dir = opendir(LANG_LFS_SKILLS_DIR);
    cJSON *arr = cJSON_CreateArray();

    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            const char *fname = ent->d_name;
            size_t len = strlen(fname);
            if (len < 4) continue;
            if (strcmp(fname + len - 3, ".md") != 0) continue;
            size_t nlen = len - 3;
            char skill_name[64];
            if (nlen >= sizeof(skill_name)) nlen = sizeof(skill_name) - 1;
            memcpy(skill_name, fname, nlen);
            skill_name[nlen] = '\0';
            cJSON_AddItemToArray(arr, cJSON_CreateString(skill_name));
        }
        closedir(dir);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, json ? json : "[]");
    free(json);
    return ESP_OK;
}

/* GET /api/skill?name=<name> */
static esp_err_t skill_get_handler(httpd_req_t *req)
{
    char query[64] = {0}, name[52] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!skill_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid skill name");
        return ESP_OK;
    }
    char path[128];
    skill_name_to_path(name, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Skill not found");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/skill?name=<name> */
static esp_err_t skill_post_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char query[64] = {0}, name[52] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!skill_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid skill name");
        return ESP_OK;
    }
    size_t max_body = 8 * 1024;
    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < max_body)
                      ? (size_t)req->content_len : max_body;
    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < body_len) { int r = httpd_req_recv(req, body + received, body_len - received); if (r <= 0) break; received += (size_t)r; }
    if (received == 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body"); return ESP_OK; }
    body[received] = '\0';

    char path[128];
    skill_name_to_path(name, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed"); return ESP_OK; }
    if (fputs(body, f) == EOF) {
        fclose(f);
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_OK;
    }
    fclose(f);
    free(body);

    ESP_LOGI(TAG, "Saved skill: %s (%d bytes)", path, received);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* DELETE /api/skill?name=<name> */
static esp_err_t skill_delete_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char query[64] = {0}, name[52] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "name", name, sizeof(name));
    }
    if (!skill_name_valid(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid skill name");
        return ESP_OK;
    }
    char path[128];
    skill_name_to_path(name, path, sizeof(path));
    int ret = unlink(path);
    ESP_LOGI(TAG, "Delete skill %s: %s", path, ret == 0 ? "ok" : "failed");
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, ret == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not found\"}");
    return ESP_OK;
}

/* ── GET /api/sysinfo ────────────────────────────────────────── */

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "ext_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

static esp_err_t sysinfo_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    size_t heap_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t lfs_total = 0, lfs_used = 0;
    esp_littlefs_info("littlefs", &lfs_total, &lfs_used);

    uint32_t tok_in = 0, tok_out = 0, llm_cost_mc = 0;
    llm_get_session_stats(&tok_in, &tok_out, &llm_cost_mc);

    uint32_t search_calls = 0, search_cost_mc = 0;
    tool_web_search_get_stats(&search_calls, &search_cost_mc);

    size_t psram_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const char *reset_str = reset_reason_str(esp_reset_reason());
    bool uac_connected = uac_microphone_available();

    /* WiFi RSSI */
    int8_t rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    /* FreeRTOS task count */
    UBaseType_t task_count = uxTaskGetNumberOfTasks();

    /* Chip temperature (shared handle via tool_device_temp) */
    float chip_temp_c = 0.0f;
    device_temp_get_celsius(&chip_temp_c);

    /* Agent daily message count */
    int msg_count = agent_get_rate_count();

    /* Running OTA partition */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();

    char buf[900];
    snprintf(buf, sizeof(buf),
             "{\"heap_free\":%u,\"heap_min\":%u,\"heap_block\":%u,\"psram_free\":%u,\"psram_min\":%u"
             ",\"lfs_total\":%u,\"lfs_used\":%u"
             ",\"tokens_in\":%u,\"tokens_out\":%u,\"cost_millicents\":%u"
             ",\"search_calls\":%u,\"search_cost_millicents\":%u"
             ",\"uptime_s\":%lu,\"reset_reason\":\"%s\""
             ",\"wifi_rssi\":%d,\"task_count\":%u"
             ",\"chip_temp_c\":%.1f,\"msg_count\":%d"
             ",\"ota_partition\":\"%s\",\"firmware_version\":\"%s\""
             ",\"uac_mic_connected\":%s}",
             (unsigned)heap_free, (unsigned)heap_min, (unsigned)heap_block,
             (unsigned)psram_free, (unsigned)psram_min,
             (unsigned)lfs_total, (unsigned)lfs_used,
             (unsigned)tok_in, (unsigned)tok_out, (unsigned)llm_cost_mc,
             (unsigned)search_calls, (unsigned)search_cost_mc,
             uptime_s, reset_str,
             (int)rssi, (unsigned)task_count,
             chip_temp_c, msg_count,
             running ? running->label : "unknown",
             app ? app->version : "unknown",
             uac_connected ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── GET /api/config ─────────────────────────────────────────── */

static void mask_key(const char *key, char *out, size_t out_size)
{
    size_t len = strlen(key);
    if (len == 0) {
        out[0] = '\0';
    } else if (len <= 4) {
        snprintf(out, out_size, "****");
    } else {
        snprintf(out, out_size, "****%s", key + len - 4);
    }
}

static void nvs_get_masked(const char *ns, const char *key, char *out, size_t out_size)
{
    char val[320] = {0};
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(val);
        nvs_get_str(nvs, key, val, &len);
        nvs_close(nvs);
    }
    mask_key(val, out, out_size);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char masked_api[16], masked_search[16], masked_stt[16], masked_tts[16];
    const char *ak = llm_get_api_key();
    mask_key(ak ? ak : "", masked_api, sizeof(masked_api));

    const char *sk = tool_web_search_get_key();
    mask_key(sk ? sk : "", masked_search, sizeof(masked_search));

    nvs_get_masked(LANG_NVS_STT, LANG_NVS_KEY_API_KEY, masked_stt, sizeof(masked_stt));
    nvs_get_masked(LANG_NVS_TTS, LANG_NVS_KEY_API_KEY, masked_tts, sizeof(masked_tts));

    /* Read TTS voice from NVS */
    char tts_voice[64] = LANG_DEFAULT_TTS_VOICE;
    {
        nvs_handle_t nvs;
        if (nvs_open(LANG_NVS_TTS, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(tts_voice);
            nvs_get_str(nvs, LANG_NVS_KEY_VOICE, tts_voice, &len);
            nvs_close(nvs);
        }
    }

    /* Read notify topic from NVS (not sensitive — show plaintext) */
    char notify_topic[128] = {0};
    {
        nvs_handle_t nvs;
        if (nvs_open("notify_config", NVS_READONLY, &nvs) == ESP_OK) {
            size_t len = sizeof(notify_topic);
            nvs_get_str(nvs, "topic", notify_topic, &len);
            nvs_close(nvs);
        }
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "provider",     llm_get_provider());
    cJSON_AddStringToObject(j, "model",        llm_get_model());
    cJSON_AddStringToObject(j, "api_key",      masked_api);
    cJSON_AddStringToObject(j, "search_key",   masked_search);
    cJSON_AddStringToObject(j, "stt_key",      masked_stt);
    cJSON_AddStringToObject(j, "tts_key",      masked_tts);
    cJSON_AddStringToObject(j, "tts_voice",    tts_voice);
    cJSON_AddStringToObject(j, "notify_topic", notify_topic);
    cJSON_AddBoolToObject(j, "verbose_logs", s_verbose_logs);
    cJSON_AddBoolToObject(j, "wake_word", wake_word_is_running());
    cJSON_AddNumberToObject(j, "wake_threshold", (double)wake_word_get_threshold());
    cJSON_AddNumberToObject(j, "wake_gain", (double)wake_word_get_gain());
    cJSON_AddNumberToObject(j, "volume", (double)i2s_audio_get_volume());

    char *json_str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, json_str ? json_str : "{}");
    free(json_str);
    return ESP_OK;
}

/* ── POST /api/config ────────────────────────────────────────── */

static void nvs_set_str_safe(const char *ns, const char *key, const char *val)
{
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, key, val);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    size_t max_body = 2048;
    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < max_body)
                      ? (size_t)req->content_len : max_body;

    char *body = malloc(body_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    size_t received = 0;
    while (received < body_len) { int r = httpd_req_recv(req, body + received, body_len - received); if (r <= 0) break; received += (size_t)r; }
    if (received == 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body received");
        return ESP_OK;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

#define CONFIG_FIELD_MAX 320
    cJSON *provider   = cJSON_GetObjectItem(root, "provider");
    cJSON *model      = cJSON_GetObjectItem(root, "model");
    cJSON *api_key    = cJSON_GetObjectItem(root, "api_key");
    cJSON *search_key = cJSON_GetObjectItem(root, "search_key");
    cJSON *stt_key    = cJSON_GetObjectItem(root, "stt_key");
    cJSON *tts_key    = cJSON_GetObjectItem(root, "tts_key");
    cJSON *tts_voice  = cJSON_GetObjectItem(root, "tts_voice");

    if (provider && cJSON_IsString(provider) && provider->valuestring[0]
        && strnlen(provider->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        llm_set_provider(provider->valuestring);
    }
    if (model && cJSON_IsString(model) && model->valuestring[0]
        && strnlen(model->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        llm_set_model(model->valuestring);
    }
    if (api_key && cJSON_IsString(api_key) && api_key->valuestring[0]
        && strncmp(api_key->valuestring, "****", 4) != 0
        && strnlen(api_key->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        llm_set_api_key(api_key->valuestring);
    }
    if (search_key && cJSON_IsString(search_key) && search_key->valuestring[0]
        && strncmp(search_key->valuestring, "****", 4) != 0
        && strnlen(search_key->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        tool_web_search_set_key(search_key->valuestring);
    }
    if (stt_key && cJSON_IsString(stt_key) && stt_key->valuestring[0]
        && strncmp(stt_key->valuestring, "****", 4) != 0
        && strnlen(stt_key->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        stt_set_api_key(stt_key->valuestring);  /* updates in-memory + NVS */
        ESP_LOGI(TAG, "STT API key updated (hot-reload)");
    }
    if (tts_key && cJSON_IsString(tts_key) && tts_key->valuestring[0]
        && strncmp(tts_key->valuestring, "****", 4) != 0
        && strnlen(tts_key->valuestring, CONFIG_FIELD_MAX + 1) <= CONFIG_FIELD_MAX) {
        tts_set_api_key(tts_key->valuestring);  /* updates in-memory + NVS */
        ESP_LOGI(TAG, "TTS API key updated (hot-reload)");
    }
    if (tts_voice && cJSON_IsString(tts_voice) && tts_voice->valuestring[0]
        && strnlen(tts_voice->valuestring, 64) <= 63) {
        tts_set_voice(tts_voice->valuestring);  /* updates in-memory + NVS */
        ESP_LOGI(TAG, "TTS voice set to: %s (hot-reload)", tts_voice->valuestring);
    }

    cJSON *notify_topic  = cJSON_GetObjectItem(root, "notify_topic");
    cJSON *notify_server = cJSON_GetObjectItem(root, "notify_server");
    if (notify_topic && cJSON_IsString(notify_topic) && notify_topic->valuestring[0]
        && strnlen(notify_topic->valuestring, 128) <= 127) {
        nvs_set_str_safe("notify_config", "topic", notify_topic->valuestring);
        ESP_LOGI(TAG, "Notify topic set to: %s", notify_topic->valuestring);
    }
    if (notify_server && cJSON_IsString(notify_server) && notify_server->valuestring[0]
        && strnlen(notify_server->valuestring, 128) <= 127) {
        nvs_set_str_safe("notify_config", "server", notify_server->valuestring);
        ESP_LOGI(TAG, "Notify server set to: %s", notify_server->valuestring);
    }
#undef CONFIG_FIELD_MAX

    cJSON *volume = cJSON_GetObjectItem(root, "volume");
    if (volume && cJSON_IsNumber(volume)) {
        int v = (int)volume->valuedouble;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        i2s_audio_set_volume((uint8_t)v);
    }

    cJSON *ww = cJSON_GetObjectItem(root, "wake_word");
    if (ww != NULL && wake_word_is_running()) {
        if (cJSON_IsTrue(ww)) {
            wake_word_resume();
        } else {
            wake_word_suspend();
        }
    }

    cJSON *verbose = cJSON_GetObjectItem(root, "verbose_logs");
    if (verbose != NULL) {
        s_verbose_logs = cJSON_IsTrue(verbose);
        nvs_set_str_safe(WS_NVS_NAMESPACE, WS_NVS_KEY_VERBOSE, s_verbose_logs ? "1" : "0");
        nvs_handle_t nvs;
        if (nvs_open(WS_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, WS_NVS_KEY_VERBOSE, s_verbose_logs ? 1 : 0);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }

    cJSON *ww_thresh = cJSON_GetObjectItem(root, "wake_threshold");
    if (ww_thresh && cJSON_IsString(ww_thresh)) {
        float t = (float)atof(ww_thresh->valuestring);
        if (t > 0.0f && t <= 1.0f) {
            wake_word_set_threshold(t);  /* applies live + saves to NVS */
            ESP_LOGI(TAG, "Wake threshold set to %.3f via HTTP", (double)t);
        }
    }

    cJSON *ww_gain = cJSON_GetObjectItem(root, "wake_gain");
    if (ww_gain && cJSON_IsString(ww_gain)) {
        float g = (float)atof(ww_gain->valuestring);
        if (g >= 0.1f && g <= 50.0f) {
            wake_word_set_gain(g);  /* applies live + saves to NVS */
            ESP_LOGI(TAG, "Wake gain set to %.2f via HTTP", (double)g);
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config updated via web");
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── GET /api/crons ──────────────────────────────────────────── */

static esp_err_t crons_get_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    const cron_job_t *jobs;
    int count;
    cron_list_jobs(&jobs, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "now_epoch", (double)time(NULL));
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        const cron_job_t *j = &jobs[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id",      j->id);
        cJSON_AddStringToObject(item, "name",    j->name);
        cJSON_AddBoolToObject(item,   "enabled", j->enabled);
        cJSON_AddStringToObject(item, "kind",    j->kind == CRON_KIND_EVERY ? "every" : "at");
        if (j->kind == CRON_KIND_EVERY) {
            cJSON_AddNumberToObject(item, "interval_s", j->interval_s);
        } else {
            cJSON_AddNumberToObject(item, "at_epoch",   (double)j->at_epoch);
        }
        cJSON_AddStringToObject(item, "message", j->message);
        cJSON_AddStringToObject(item, "channel", j->channel);
        cJSON_AddNumberToObject(item, "next_run", (double)j->next_run);
        cJSON_AddNumberToObject(item, "last_run", (double)j->last_run);
        cJSON_AddBoolToObject(item,   "delete_after_run", j->delete_after_run);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, json_str ? json_str : "{\"jobs\":[]}");
    free(json_str);
    return ESP_OK;
}

/* ── DELETE /api/cron ────────────────────────────────────────── */

static esp_err_t cron_delete_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char query[32] = {0}, id[12] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
    }
    if (id[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    esp_err_t err = cron_remove_job(id);
    httpd_resp_sendstr(req, err == ESP_OK ? "{\"ok\":true}"
                                          : "{\"ok\":false,\"error\":\"not_found\"}");
    return ESP_OK;
}

/* ── POST /api/ota ───────────────────────────────────────────── */

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    char body[300] = {0};
    int  rcv = httpd_req_recv(req, body, sizeof(body) - 1);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);

    if (rcv <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no body\"}");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *jurl = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(jurl) || !jurl->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing url\"}");
        return ESP_OK;
    }

    esp_err_t err = ota_start_async(jurl->valuestring);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"updating\"}");
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ota_already_running\"}");
    } else if (err == ESP_ERR_NO_MEM) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"low_heap\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"task_failed\"}");
    }
    return ESP_OK;
}

/* ── GET /api/ota/status ─────────────────────────────────────── */

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_OK;
    }
    ota_status_t st = ota_get_status();
    static const char *state_names[] = {
        "idle", "pending", "downloading", "verifying", "rebooting", "error"
    };
    const char *state_str = (st.state <= OTA_STATE_ERROR)
                            ? state_names[st.state] : "unknown";
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"progress_pct\":%u,"
             "\"new_version\":\"%s\",\"error_msg\":\"%s\"}",
             state_str, (unsigned)st.progress_pct,
             st.new_version, st.error_msg);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── GET /api/logs ───────────────────────────────────────────── */

static esp_err_t logs_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    /* Check for ?type=monitor to return the PSRAM monitor ring */
    char query[32] = {0};
    char type[16]  = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "type", type, sizeof(type));
    }

    httpd_resp_set_type(req, "text/plain");
    apply_cors(req);

    if (strcmp(type, "monitor") == 0) {
        char *buf = ps_malloc(MONITOR_RING_SIZE + 64);
        if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
        monitor_ring_get(buf, MONITOR_RING_SIZE + 64);
        httpd_resp_sendstr(req, buf);
        free(buf);
    } else {
        char *buf = ps_malloc(LOG_RING_SIZE + 64);
        if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
        log_buffer_get(buf, LOG_RING_SIZE + 64);
        httpd_resp_sendstr(req, buf);
        free(buf);
    }
    return ESP_OK;
}

/* ── POST /api/logs/clear ────────────────────────────────────── */

static esp_err_t logs_clear_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }
    log_buffer_clear();
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── POST /api/message ───────────────────────────────────────── */

/* ── /api/say — direct TTS playback, no LLM round-trip ────────── */

/* Async say task: TTS needs ~16KB stack for TLS — httpd worker only has 10KB */
static void say_async_task(void *arg)
{
    char *text = (char *)arg;
    esp_err_t err = say_speak(text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "/api/say async failed: %s", esp_err_to_name(err));
    }
    free(text);
    vTaskDelete(NULL);
}

static esp_err_t say_post_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    httpd_resp_set_type(req, "application/json");
    apply_cors(req);

    char body[1024] = {0};
    int rcv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (rcv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *jtext = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(jtext) || !jtext->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'text'");
        return ESP_OK;
    }

    /* Heap-copy text for the async task */
    char *text = strdup(jtext->valuestring);
    cJSON_Delete(root);

    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "/api/say: \"%.*s%s\"",
             (int)(strlen(text) > 60 ? 60 : strlen(text)), text,
             strlen(text) > 60 ? "..." : "");

    /* Spawn task: 8KB for local HTTP TTS, 16KB if cloud TLS needed.
     * Try 8KB first (covers plain-HTTP mlx-audio), fall back to 16KB. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        say_async_task, "say", 8 * 1024, text, 5, NULL, 0);
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCore(
            say_async_task, "say", 16 * 1024, text, 5, NULL, 0);
    }
    if (ok != pdPASS) {
        free(text);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── /api/speaker-test — 440 Hz tone, no TTS needed ───────────── */

static void speaker_test_task(void *arg)
{
    (void)arg;
    i2s_audio_test_tone();
    vTaskDelete(NULL);
}

static esp_err_t speaker_test_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) { httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); return ESP_OK; }

    httpd_resp_set_type(req, "application/json");
    apply_cors(req);

    BaseType_t ok = xTaskCreatePinnedToCore(
        speaker_test_task, "spk_test", 8192, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Task create failed");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "{\"ok\":true,\"tone\":\"440Hz 2s\"}");
    return ESP_OK;
}

/* ── HMAC-SHA256 verification for webhooks ─────────────────── */

static bool verify_webhook_hmac(const char *body, size_t body_len, const char *sig_header)
{
    const char *secret = services_get_webhook_secret();
    if (!secret) return false;

    /* Header format: "sha256=<hex>" */
    if (strncmp(sig_header, "sha256=", 7) != 0) return false;
    const char *expected_hex = sig_header + 7;
    if (strlen(expected_hex) != 64) return false;

    /* Compute HMAC-SHA256 */
    unsigned char hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, md_info, 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret, strlen(secret));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)body, body_len);
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    /* Compare as hex */
    char computed_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(computed_hex + i * 2, "%02x", hmac[i]);
    }
    computed_hex[64] = '\0';

    return (memcmp(computed_hex, expected_hex, 64) == 0);
}

static esp_err_t message_post_handler(httpd_req_t *req)
{
    /* Read body first — needed for both HMAC verification and JSON parsing */
    #define MSG_BODY_MAX 8192
    char *body = ps_malloc(MSG_BODY_MAX);
    if (!body) { httpd_resp_send_500(req); return ESP_OK; }
    memset(body, 0, MSG_BODY_MAX);
    int rcv = httpd_req_recv(req, body, MSG_BODY_MAX - 1);
    httpd_resp_set_type(req, "application/json");
    apply_cors(req);

    if (rcv <= 0) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_OK;
    }

    /* Auth: Bearer token OR webhook HMAC signature */
    bool authed = request_is_authed(req);
    if (!authed) {
        /* Check for webhook HMAC signature */
        char sig_hdr[128] = {0};
        if (httpd_req_get_hdr_value_str(req, "X-Hub-Signature-256", sig_hdr, sizeof(sig_hdr)) == ESP_OK) {
            authed = verify_webhook_hmac(body, (size_t)rcv, sig_hdr);
            if (authed) {
                ESP_LOGI(TAG, "/api/message: webhook HMAC verified");
            } else {
                ESP_LOGW(TAG, "/api/message: webhook HMAC invalid");
            }
        }
    }

    if (!authed) {
        free(body);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *jmsg  = cJSON_GetObjectItem(root, "message");
    if (!cJSON_IsString(jmsg) || !jmsg->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing message");
        return ESP_OK;
    }

    cJSON *jchan   = cJSON_GetObjectItem(root, "channel");
    cJSON *jchat   = cJSON_GetObjectItem(root, "chat_id");
    cJSON *jaction = cJSON_GetObjectItem(root, "action");
    const char *channel = (cJSON_IsString(jchan) && jchan->valuestring[0])
                          ? jchan->valuestring : LANG_CHAN_SYSTEM;
    const char *chat_id = (cJSON_IsString(jchat) && jchat->valuestring[0])
                          ? jchat->valuestring : "api";

    /* Build message content — optionally prefixed with action/skill directive */
    char *content = NULL;
    if (cJSON_IsString(jaction) && jaction->valuestring[0]) {
        size_t len = strlen(jaction->valuestring) + strlen(jmsg->valuestring) + 64;
        content = malloc(len);
        if (content) {
            snprintf(content, len, "Run the %s skill: %s",
                     jaction->valuestring, jmsg->valuestring);
        }
    }
    if (!content) {
        content = strdup(jmsg->valuestring);
    }
    cJSON_Delete(root);

    if (!content) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    lang_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = content;

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        free(msg.content);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bus full");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "/api/message queued (channel=%s chat_id=%s)", channel, chat_id);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "{\"status\":\"queued\"}");
    return ESP_OK;
}

/* ── POST /mcp — MCP Streamable HTTP transport ───────────────── */

#define MCP_MAX_BODY   32768
#define MCP_OUTPUT_SIZE 32768

static esp_err_t mcp_post_handler(httpd_req_t *req)
{
    if (!request_is_authed(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_OK;
    }

    size_t body_len = (req->content_len > 0 && (size_t)req->content_len < MCP_MAX_BODY)
                      ? (size_t)req->content_len : MCP_MAX_BODY;

    char *body = ps_calloc(1, body_len + 1);
    char *output = ps_calloc(1, MCP_OUTPUT_SIZE);
    if (!body || !output) {
        free(body);
        free(output);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    /* Read full POST body (mbedTLS may fragment across multiple recv calls) */
    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, body + received, body_len - received);
        if (r <= 0) break;
        received += (size_t)r;
    }
    if (received == 0) {
        free(body);
        free(output);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body received");
        return ESP_OK;
    }
    body[received] = '\0';

    mcp_handle_request(body, received, output, MCP_OUTPUT_SIZE);
    free(body);

    apply_cors(req);
    httpd_resp_set_type(req, "application/json");

    if (output[0] == '\0') {
        /* Notification — no response body, send 204 */
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_sendstr(req, output);
    }

    free(output);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────── */

esp_err_t ws_server_start(void)
{
    s_clients_lock = xSemaphoreCreateMutex();
    if (!s_clients_lock) {
        ESP_LOGE(TAG, "Failed to create clients mutex");
        return ESP_ERR_NO_MEM;
    }
    s_send_lock = xSemaphoreCreateMutex();
    if (!s_send_lock) {
        ESP_LOGE(TAG, "Failed to create send mutex");
        return ESP_ERR_NO_MEM;
    }
    memset(s_clients, 0, sizeof(s_clients));

    /* Allocate PSRAM monitor ring for persistent diagnostics */
    if (!s_monitor_ring) {
        s_monitor_ring = ps_calloc(1, MONITOR_RING_SIZE);
        if (s_monitor_ring) {
            ESP_LOGI(TAG, "Monitor ring: %dKB PSRAM allocated", MONITOR_RING_SIZE / 1024);
        }
    }

    {
        nvs_handle_t nvs;
        if (nvs_open(WS_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(nvs, WS_NVS_KEY_VERBOSE, &v) == ESP_OK) s_verbose_logs = (v != 0);
            nvs_close(nvs);
        }
        ESP_LOGI(TAG, "Verbose logs: %s", s_verbose_logs ? "on" : "off");
    }

    /* Load auth token */
    {
        char atmp[128] = {0};
        size_t alen = sizeof(atmp);
        nvs_handle_t anvs;
        if (nvs_open(WS_NVS_NAMESPACE, NVS_READONLY, &anvs) == ESP_OK) {
            nvs_get_str(anvs, WS_NVS_KEY_AUTH_TOKEN, atmp, &alen);
            nvs_close(anvs);
        }
        if (atmp[0]) {
            strncpy(s_auth_token, atmp, sizeof(s_auth_token) - 1);
        } else if (LANG_SECRET_HTTP_TOKEN[0]) {
            strncpy(s_auth_token, LANG_SECRET_HTTP_TOKEN, sizeof(s_auth_token) - 1);
        }
        if (s_auth_token[0]) {
            ESP_LOGI(TAG, "HTTP auth enabled");
        } else {
            ESP_LOGW(TAG, "HTTP auth disabled (no token set)");
        }
    }

    /* Load CORS origin */
    {
        char ctmp[128] = {0};
        size_t clen = sizeof(ctmp);
        nvs_handle_t cnvs;
        if (nvs_open(WS_NVS_NAMESPACE, NVS_READONLY, &cnvs) == ESP_OK) {
            nvs_get_str(cnvs, WS_NVS_KEY_CORS_ORIGIN, ctmp, &clen);
            nvs_close(cnvs);
        }
        if (ctmp[0]) {
            strncpy(s_cors_origin, ctmp, sizeof(s_cors_origin) - 1);
        }
        ESP_LOGI(TAG, "CORS origin: %s", s_cors_origin);
    }

    httpd_config_t cfg             = HTTPD_DEFAULT_CONFIG();
    cfg.server_port                = LANG_WS_PORT;
    cfg.ctrl_port                  = LANG_WS_PORT + 1;
    cfg.max_open_sockets           = 8;
    cfg.stack_size                 = 8192;
    cfg.max_uri_handlers           = 28;  /* 27 handlers registered + 1 spare */
    cfg.send_wait_timeout          = 30;
    cfg.recv_wait_timeout          = 120;  /* extended: WS ping keeps connection alive */
    cfg.uri_match_fn               = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* WebSocket */
    httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler,
        .is_websocket = true, .handle_ws_control_frames = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* Voice UI (root) */
    httpd_uri_t voice_uri = { .uri = "/", .method = HTTP_GET, .handler = voice_ui_handler };
    httpd_register_uri_handler(s_server, &voice_uri);

    /* Speaker test (440 Hz tone — no TTS needed) */
    httpd_uri_t spktest_uri = { .uri = "/api/speaker-test", .method = HTTP_POST, .handler = speaker_test_handler };
    httpd_register_uri_handler(s_server, &spktest_uri);

    /* Developer console */
    httpd_uri_t console_uri = { .uri = "/console", .method = HTTP_GET, .handler = dev_console_handler };
    httpd_register_uri_handler(s_server, &console_uri);

    /* Health check */
    httpd_uri_t health_uri = { .uri = "/health", .method = HTTP_GET, .handler = health_handler };
    httpd_register_uri_handler(s_server, &health_uri);

    /* TTS audio serve (wildcard: /tts/ID) */
    httpd_uri_t tts_uri = { .uri = "/tts/*", .method = HTTP_GET, .handler = tts_serve_handler };
    httpd_register_uri_handler(s_server, &tts_uri);

    /* Latest webcam image */
    httpd_uri_t camera_uri = { .uri = "/camera/latest.jpg", .method = HTTP_GET, .handler = camera_latest_handler };
    httpd_register_uri_handler(s_server, &camera_uri);

    /* File read/write */
    httpd_uri_t file_get_uri = { .uri = "/api/file", .method = HTTP_GET, .handler = file_get_handler };
    httpd_register_uri_handler(s_server, &file_get_uri);
    httpd_uri_t file_post_uri = { .uri = "/api/file", .method = HTTP_POST, .handler = file_post_handler };
    httpd_register_uri_handler(s_server, &file_post_uri);

    /* Crashlog (dedicated endpoint; also reachable via /api/file?name=crashlog) */
    httpd_uri_t crashlog_get_uri  = { .uri = "/api/crashlog", .method = HTTP_GET,    .handler = crashlog_get_handler };
    httpd_register_uri_handler(s_server, &crashlog_get_uri);
    httpd_uri_t crashlog_del_uri  = { .uri = "/api/crashlog", .method = HTTP_DELETE, .handler = crashlog_delete_handler };
    httpd_register_uri_handler(s_server, &crashlog_del_uri);

    /* Reboot */
    httpd_uri_t reboot_uri = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler };
    httpd_register_uri_handler(s_server, &reboot_uri);

    /* Heartbeat */
    httpd_uri_t heartbeat_uri = { .uri = "/api/heartbeat", .method = HTTP_POST, .handler = heartbeat_handler };
    httpd_register_uri_handler(s_server, &heartbeat_uri);

    /* Skills */
    httpd_uri_t skills_list_uri = { .uri = "/api/skills", .method = HTTP_GET, .handler = skills_list_handler };
    httpd_register_uri_handler(s_server, &skills_list_uri);
    httpd_uri_t skill_get_uri = { .uri = "/api/skill", .method = HTTP_GET, .handler = skill_get_handler };
    httpd_register_uri_handler(s_server, &skill_get_uri);
    httpd_uri_t skill_post_uri = { .uri = "/api/skill", .method = HTTP_POST, .handler = skill_post_handler };
    httpd_register_uri_handler(s_server, &skill_post_uri);
    httpd_uri_t skill_del_uri = { .uri = "/api/skill", .method = HTTP_DELETE, .handler = skill_delete_handler };
    httpd_register_uri_handler(s_server, &skill_del_uri);

    /* Sysinfo */
    httpd_uri_t sysinfo_uri = { .uri = "/api/sysinfo", .method = HTTP_GET, .handler = sysinfo_handler };
    httpd_register_uri_handler(s_server, &sysinfo_uri);

    /* Log ring buffer */
    httpd_uri_t logs_uri = { .uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler };
    httpd_register_uri_handler(s_server, &logs_uri);
    httpd_uri_t logs_clear_uri = { .uri = "/api/logs/clear", .method = HTTP_POST, .handler = logs_clear_handler };
    httpd_register_uri_handler(s_server, &logs_clear_uri);

    /* Config */
    httpd_uri_t config_get_uri = { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler };
    httpd_register_uri_handler(s_server, &config_get_uri);
    httpd_uri_t config_post_uri = { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler };
    httpd_register_uri_handler(s_server, &config_post_uri);

    /* Cron */
    httpd_uri_t crons_get_uri = { .uri = "/api/crons", .method = HTTP_GET, .handler = crons_get_handler };
    httpd_register_uri_handler(s_server, &crons_get_uri);
    httpd_uri_t cron_del_uri = { .uri = "/api/cron", .method = HTTP_DELETE, .handler = cron_delete_handler };
    httpd_register_uri_handler(s_server, &cron_del_uri);

    /* OTA */
    httpd_uri_t ota_uri = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler };
    httpd_register_uri_handler(s_server, &ota_uri);
    httpd_uri_t ota_status_uri = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler };
    httpd_register_uri_handler(s_server, &ota_status_uri);

    /* Direct TTS playback — no LLM */
    httpd_uri_t say_uri = { .uri = "/api/say", .method = HTTP_POST, .handler = say_post_handler };
    httpd_register_uri_handler(s_server, &say_uri);

    /* Inbound message injection */
    httpd_uri_t message_uri = { .uri = "/api/message", .method = HTTP_POST, .handler = message_post_handler };
    httpd_register_uri_handler(s_server, &message_uri);

    /* MCP (Model Context Protocol) */
    httpd_uri_t mcp_uri = { .uri = "/mcp", .method = HTTP_POST, .handler = mcp_post_handler };
    httpd_register_uri_handler(s_server, &mcp_uri);

    /* WebSocket keepalive ping task (Core 0, low priority) */
    if (!s_ws_ping_task) {
        xTaskCreatePinnedToCore(ws_ping_task, "ws_ping", 4096, NULL, 2, &s_ws_ping_task, 0);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d (WS: /ws, Voice: /, Dev: /console)", LANG_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    ws_client_t *client = find_client_by_chat_id(chat_id);
    int fd = client ? client->fd : -1;
    /* Broadcast fallback: if no client matches the specific chat_id (e.g. PTT
     * uses chat_id="ptt"), send to the first active WS client instead. */
    if (fd < 0) {
        for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
            if (s_clients[i].active) {
                fd = s_clients[i].fd;
                ESP_LOGI(TAG, "No WS client '%s', broadcasting to %s (fd=%d)",
                         chat_id, s_clients[i].chat_id, fd);
                break;
            }
        }
    }
    xSemaphoreGive(s_clients_lock);

    if (fd < 0) {
        ESP_LOGW(TAG, "No WS clients connected (chat_id=%s)", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "message");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };

    esp_err_t ret = send_frame_locked(fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Send to %s failed: %s", chat_id, esp_err_to_name(ret));
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(fd);
        xSemaphoreGive(s_clients_lock);
    }
    return ret;
}

/* Send {"type":"message","content":"...","tts_id":"<id>"} or
 * {"type":"tts_ready","tts_id":"<id>"} when text is NULL (follow-up audio). */
esp_err_t ws_server_send_with_tts(const char *chat_id, const char *text,
                                  const char *tts_id, const char *image_url)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    ws_client_t *client = find_client_by_chat_id(chat_id);
    int fd = client ? client->fd : -1;
    /* Broadcast fallback for PTT (chat_id="ptt") or other non-matching IDs */
    if (fd < 0) {
        for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
            if (s_clients[i].active) {
                fd = s_clients[i].fd;
                break;
            }
        }
    }
    xSemaphoreGive(s_clients_lock);

    if (fd < 0) {
        ESP_LOGD(TAG, "No WS client for chat_id=%s", chat_id ? chat_id : "null");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *resp = cJSON_CreateObject();
    if (text) {
        /* Full message with text (and optionally tts_id) */
        cJSON_AddStringToObject(resp, "type", "message");
        cJSON_AddStringToObject(resp, "content", text);
    } else {
        /* TTS-only follow-up — text was already sent */
        cJSON_AddStringToObject(resp, "type", "tts_ready");
    }
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    if (tts_id && tts_id[0]) {
        cJSON_AddStringToObject(resp, "tts_id", tts_id);
    }
    if (image_url && image_url[0]) {
        cJSON_AddStringToObject(resp, "image_url", image_url);
    }

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };

    esp_err_t ret = send_frame_locked(fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS send failed for %s: %s", chat_id, esp_err_to_name(ret));
        xSemaphoreTake(s_clients_lock, portMAX_DELAY);
        remove_client(fd);
        xSemaphoreGive(s_clients_lock);
    }
    return ret;
}

esp_err_t ws_server_broadcast_monitor(const char *event, const char *msg_text)
{
    /* Append to PSRAM monitor ring (persists even without WS clients) */
    {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char line[320];
        int n = snprintf(line, sizeof(line), "%02d:%02d:%02d[%s] %s",
                         tm.tm_hour, tm.tm_min, tm.tm_sec,
                         event ? event : "", msg_text ? msg_text : "");
        if (n > 0) monitor_ring_append(line, (size_t)n);
    }

    if (!s_server) return ESP_ERR_INVALID_STATE;

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "type",  "monitor");
    cJSON_AddStringToObject(j, "event", event    ? event    : "");
    cJSON_AddStringToObject(j, "msg",   msg_text ? msg_text : "");

    char *json_str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len     = strlen(json_str),
    };

    /* Snapshot active fds under lock */
    int snap_fds[LANG_WS_MAX_CLIENTS];
    int snap_count = 0;
    xSemaphoreTake(s_clients_lock, portMAX_DELAY);
    for (int i = 0; i < LANG_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active) {
            snap_fds[snap_count++] = s_clients[i].fd;
        }
    }
    xSemaphoreGive(s_clients_lock);

    for (int i = 0; i < snap_count; i++) {
        /* Guard against FD reuse: if a WS client disconnected and the kernel
         * recycled the FD for a new HTTP connection, sending a WS frame to it
         * would corrupt the HTTP response.  httpd_ws_get_fd_info() consults
         * the httpd's internal FD table — HTTPD_WS_CLIENT_WEBSOCKET confirms
         * the FD is still an active, upgraded WebSocket connection. */
        httpd_ws_client_info_t fd_type = httpd_ws_get_fd_info(s_server, snap_fds[i]);
        if (fd_type != HTTPD_WS_CLIENT_WEBSOCKET) {
            ESP_LOGD(TAG, "Monitor: fd=%d is not WS (type=%d), removing stale slot",
                     snap_fds[i], (int)fd_type);
            xSemaphoreTake(s_clients_lock, portMAX_DELAY);
            remove_client(snap_fds[i]);
            xSemaphoreGive(s_clients_lock);
            continue;
        }
        esp_err_t ret = send_frame_locked(snap_fds[i], &pkt);
        if (ret != ESP_OK) {
            ESP_LOGD(TAG, "Monitor send to fd=%d failed, removing", snap_fds[i]);
            xSemaphoreTake(s_clients_lock, portMAX_DELAY);
            remove_client(snap_fds[i]);
            xSemaphoreGive(s_clients_lock);
        }
    }

    free(json_str);
    return ESP_OK;
}

bool ws_server_get_verbose_logs(void) { return s_verbose_logs; }

esp_err_t ws_server_broadcast_monitor_verbose(const char *event, const char *msg)
{
    if (!s_verbose_logs) return ESP_OK;
    return ws_server_broadcast_monitor(event, msg);
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Server stopped");
    }
    return ESP_OK;
}
