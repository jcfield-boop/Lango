#include "wifi_onboard.h"
#include "wifi_manager.h"
#include "langoustine_config.h"
#include "led/led_indicator.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

static const char *TAG = "onboard";

/* ── Credential check ──────────────────────────────────────────── */

bool wifi_onboard_has_credentials(void)
{
    /* Check NVS first */
    nvs_handle_t nvs;
    if (nvs_open(LANG_NVS_WIFI, NVS_READONLY, &nvs) == ESP_OK) {
        char ssid[33] = {0};
        size_t len = sizeof(ssid);
        if (nvs_get_str(nvs, LANG_NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0') {
            nvs_close(nvs);
            return true;
        }
        nvs_close(nvs);
    }

    /* Check build-time secrets */
    if (LANG_SECRET_WIFI_SSID[0] != '\0') {
        return true;
    }

    return false;
}

/* ── Captive portal HTML ───────────────────────────────────────── */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Langoustine Setup</title>"
    "<style>"
    "body{font-family:system-ui,-apple-system,sans-serif;max-width:420px;"
    "margin:40px auto;padding:0 20px;background:#1a1a2e;color:#e0e0e0}"
    "h1{color:#00d4aa;font-size:1.4em;text-align:center}"
    "h2{color:#888;font-size:0.85em;text-align:center;margin-top:-10px}"
    "label{display:block;margin:12px 0 4px;font-size:0.9em;color:#aaa}"
    "input{width:100%;padding:10px;border:1px solid #333;border-radius:6px;"
    "background:#0d0d1a;color:#fff;font-size:1em;box-sizing:border-box}"
    "input:focus{border-color:#00d4aa;outline:none}"
    "button{width:100%;padding:12px;margin-top:20px;border:none;border-radius:6px;"
    "background:#00d4aa;color:#000;font-size:1.1em;font-weight:bold;cursor:pointer}"
    "button:hover{background:#00b894}"
    ".opt{color:#666;font-size:0.8em}"
    ".footer{text-align:center;margin-top:24px;color:#444;font-size:0.75em}"
    "</style></head><body>"
    "<h1>&#129438; Langoustine</h1>"
    "<h2>ESP32-S3 AI Assistant Setup</h2>"
    "<form method='POST' action='/save'>"
    "<label>WiFi Network (SSID) *</label>"
    "<input name='ssid' required maxlength='32' placeholder='Your WiFi name'>"
    "<label>WiFi Password *</label>"
    "<input name='pass' type='password' maxlength='64' placeholder='WiFi password'>"
    "<label>LLM API Key <span class='opt'>(optional — set later via CLI)</span></label>"
    "<input name='api_key' maxlength='128' placeholder='sk-... or openrouter key'>"
    "<label>Telegram Bot Token <span class='opt'>(optional)</span></label>"
    "<input name='tg_token' maxlength='64' placeholder='123456:ABC-...'>"
    "<button type='submit'>Save &amp; Reboot</button>"
    "</form>"
    "<p class='footer'>Connect to this WiFi to configure. After saving,<br>"
    "the device reboots and joins your network.</p>"
    "</body></html>";

static const char SAVE_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved!</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:420px;margin:80px auto;"
    "padding:0 20px;background:#1a1a2e;color:#e0e0e0;text-align:center}"
    "h1{color:#00d4aa}"
    "</style></head><body>"
    "<h1>&#10004; Settings Saved!</h1>"
    "<p>Rebooting now...<br>Connect to your WiFi network and look for<br>"
    "<strong>https://lango.local</strong></p>"
    "</body></html>";

/* ── URL-decode helper ─────────────────────────────────────────── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int h = hex_val(src[1]);
            int l = hex_val(src[2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)((h << 4) | l);
                src += 3;
                continue;
            }
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
            continue;
        }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

/* ── Extract form field from POST body ─────────────────────────── */

static bool extract_field(const char *body, const char *key, char *out, size_t out_size)
{
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        /* Verify it's at start or after & */
        if (p != body && *(p - 1) != '&') { p += klen; continue; }
        if (p[klen] != '=') { p += klen; continue; }
        const char *val = p + klen + 1;
        const char *end = strchr(val, '&');
        size_t vlen = end ? (size_t)(end - val) : strlen(val);
        if (vlen >= out_size) vlen = out_size - 1;
        char encoded[256] = {0};
        if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
        memcpy(encoded, val, vlen);
        encoded[vlen] = '\0';
        url_decode(out, encoded, out_size);
        return true;
    }
    out[0] = '\0';
    return false;
}

/* ── HTTP handlers ─────────────────────────────────────────────── */

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_OK;
    }
    body[len] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    char api_key[129] = {0};
    char tg_token[65] = {0};

    extract_field(body, "ssid", ssid, sizeof(ssid));
    extract_field(body, "pass", pass, sizeof(pass));
    extract_field(body, "api_key", api_key, sizeof(api_key));
    extract_field(body, "tg_token", tg_token, sizeof(tg_token));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Saving credentials: SSID=%s", ssid);

    /* Save WiFi credentials */
    wifi_manager_set_credentials(ssid, pass);

    /* Save API key if provided */
    if (api_key[0]) {
        nvs_handle_t nvs;
        if (nvs_open(LANG_NVS_LLM, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, LANG_NVS_KEY_API_KEY, api_key);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "API key saved (len=%d)", (int)strlen(api_key));
        }
    }

    /* Save Telegram token if provided */
    if (tg_token[0]) {
        nvs_handle_t nvs;
        if (nvs_open(LANG_NVS_TG, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, LANG_NVS_KEY_TG_TOKEN, tg_token);
            nvs_commit(nvs);
            nvs_close(nvs);
            ESP_LOGI(TAG, "Telegram token saved");
        }
    }

    /* Send success page */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVE_HTML, HTTPD_RESP_USE_STRLEN);

    /* Reboot after 2 seconds (let the response reach the browser) */
    ESP_LOGI(TAG, "Credentials saved — rebooting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

/* Redirect all unknown URLs to the portal (captive portal behavior) */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── DNS server for captive portal ─────────────────────────────── */
/* Simple DNS responder: all queries → 192.168.4.1 (AP gateway).
 * This makes phones/laptops auto-open the captive portal page. */

#include "lwip/sockets.h"
#include "lwip/netdb.h"

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS: socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS: bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS captive portal responder started");

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (len < 12) continue;

        /* Build minimal DNS response: copy query, set response flags, append answer */
        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;
        memcpy(resp, buf, len);

        /* Set QR=1 (response), AA=1 (authoritative) */
        resp[2] = 0x85;  /* QR=1, Opcode=0, AA=1, TC=0, RD=1 */
        resp[3] = 0x80;  /* RA=1, Z=0, RCODE=0 (no error) */
        /* Set answer count = 1 */
        resp[6] = 0x00; resp[7] = 0x01;

        /* Append answer: pointer to query name + type A + class IN + TTL + IP */
        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;  /* name pointer to offset 12 */
        resp[pos++] = 0x00; resp[pos++] = 0x01;   /* type A */
        resp[pos++] = 0x00; resp[pos++] = 0x01;   /* class IN */
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x0A;   /* TTL = 10s */
        resp[pos++] = 0x00; resp[pos++] = 0x04;   /* data length = 4 */
        resp[pos++] = 192; resp[pos++] = 168;
        resp[pos++] = 4;   resp[pos++] = 1;       /* 192.168.4.1 */

        sendto(sock, resp, pos, 0, (struct sockaddr *)&src, src_len);
    }
}

/* ── Main onboarding entry ─────────────────────────────────────── */

void wifi_onboard_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi onboarding captive portal...");
    led_indicator_set(LED_WIFI);

    /* Get MAC for unique AP name */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Langoustine-%02X%02X", mac[4], mac[5]);

    /* Create AP netif */
    esp_netif_create_default_wifi_ap();

    /* Configure AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: %s (open, no password)", ap_ssid);
    ESP_LOGI(TAG, "Connect to this network and open http://192.168.4.1");

    /* Start DNS server for captive portal redirection */
    xTaskCreatePinnedToCore(dns_task, "dns", 4096, NULL, 3, NULL, 0);

    /* Start HTTP server */
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 4096;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* Register handlers */
    httpd_uri_t portal_uri = {
        .uri = "/", .method = HTTP_GET, .handler = portal_get_handler
    };
    httpd_register_uri_handler(server, &portal_uri);

    httpd_uri_t save_uri = {
        .uri = "/save", .method = HTTP_POST, .handler = portal_save_handler
    };
    httpd_register_uri_handler(server, &save_uri);

    /* Catch-all redirect for captive portal detection (Apple, Android, Windows) */
    httpd_uri_t redirect_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = portal_redirect_handler
    };
    httpd_register_uri_handler(server, &redirect_uri);

    ESP_LOGI(TAG, "Captive portal ready — waiting for user to configure...");

    /* Block forever — portal_save_handler will reboot */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
