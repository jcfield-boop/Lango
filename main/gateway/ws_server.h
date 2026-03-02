#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Initialize and start the WebSocket/HTTP server on LANG_WS_PORT.
 *
 * HTTP endpoints:
 *   GET  /           Voice UI (from LittleFS /lfs/console/index.html)
 *   GET  /console    Developer console (from LittleFS /lfs/console/dev.html)
 *   GET  /health     Health check: {"ok":true,"firmware":"langoustine"}
 *   GET  /tts/<id>   Serve cached TTS audio (8-char hex id) from PSRAM
 *   GET/POST /api/file?name=soul|user|memory|heartbeat|services
 *   POST /api/reboot
 *   GET  /api/crons
 *   DELETE /api/cron?id=<id>
 *   GET/POST /api/config   (includes stt_key, tts_key, tts_voice)
 *   GET/POST/DELETE /api/skill[s]
 *   GET  /api/sysinfo
 *   POST /api/ota
 *
 * WebSocket endpoint /ws:
 *   Client → Server (JSON text):
 *     {"type":"prompt","content":"Hello","chat_id":"ws_42"}
 *     {"type":"audio_start","mime":"audio/webm;codecs=opus","chat_id":"ws_42"}
 *     Binary frames: raw audio bytes during recording
 *     {"type":"audio_end","chat_id":"ws_42"}
 *     {"type":"audio_abort","chat_id":"ws_42"}
 *
 *   Server → Client (JSON text):
 *     {"type":"token","content":"<delta>","chat_id":"ws_42"}
 *     {"type":"message","content":"<full>","chat_id":"ws_42","tts_id":"a3f7b2c1"}
 *     {"type":"status","stage":"stt_processing|llm_thinking|tts_generating|idle","chat_id":"ws_42"}
 *     {"type":"error","code":"stt_failed|audio_overflow|rate_limited","message":"...","chat_id":"ws_42"}
 *     {"type":"monitor","event":"...","msg":"..."}
 */
esp_err_t ws_server_start(void);

/** Send a final message to a specific WebSocket client by chat_id. */
esp_err_t ws_server_send(const char *chat_id, const char *text);

/** Send a final message with optional TTS ID (triggers browser audio playback). */
esp_err_t ws_server_send_with_tts(const char *chat_id, const char *text, const char *tts_id);

/** Send a streaming token delta to a client. */
esp_err_t ws_server_send_token(const char *chat_id, const char *delta);

/** Send a status update to a client (stage: idle/stt_processing/llm_thinking/tts_generating). */
esp_err_t ws_server_send_status(const char *chat_id, const char *stage);

/** Send an error to a client. */
esp_err_t ws_server_send_error(const char *chat_id, const char *code, const char *message);

/** Broadcast a monitor event to all connected WebSocket clients. */
esp_err_t ws_server_broadcast_monitor(const char *event, const char *msg);

/** Broadcast verbose-only monitor event; no-op when verbose logs disabled. */
esp_err_t ws_server_broadcast_monitor_verbose(const char *event, const char *msg);

/** Returns true if verbose logging is enabled. */
bool ws_server_get_verbose_logs(void);

/** Stop the WebSocket/HTTP server. */
esp_err_t ws_server_stop(void);
