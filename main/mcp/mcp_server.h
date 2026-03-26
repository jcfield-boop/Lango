#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * MCP (Model Context Protocol) Streamable HTTP server.
 *
 * Exposes the device's tool registry to external MCP clients (Claude Desktop,
 * Cursor, etc.) via JSON-RPC 2.0 over a single HTTP POST endpoint.
 */

/**
 * Handle an incoming MCP JSON-RPC request.
 *
 * @param body       Raw POST body (JSON-RPC 2.0 request)
 * @param body_len   Length of body
 * @param output     Pre-allocated PSRAM buffer for the JSON-RPC response
 * @param output_size Size of output buffer
 * @return ESP_OK on success, ESP_FAIL on parse error
 */
esp_err_t mcp_handle_request(const char *body, size_t body_len,
                             char *output, size_t output_size);
