/**
 * test_ws_messages.c — Unity tests for WebSocket JSON message routing.
 *
 * Tests the type/content parsing logic that ws_server uses to dispatch
 * inbound text frames.  No hardware or FreeRTOS dependencies.
 */

#include "unity.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h>

/* ── helpers that mirror ws_server.c parsing ────────────────── */

typedef struct {
    const char *type;
    const char *content;
    const char *chat_id;
    const char *mime;
    bool        valid;
} parsed_msg_t;

static parsed_msg_t parse_ws_text(const char *json_str)
{
    parsed_msg_t out = {0};
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return out;

    cJSON *t = cJSON_GetObjectItem(root, "type");
    cJSON *c = cJSON_GetObjectItem(root, "content");
    cJSON *id = cJSON_GetObjectItem(root, "chat_id");
    cJSON *m = cJSON_GetObjectItem(root, "mime");

    out.valid   = true;
    out.type    = (t  && cJSON_IsString(t))  ? t->valuestring  : "";
    out.content = (c  && cJSON_IsString(c))  ? c->valuestring  : "";
    out.chat_id = (id && cJSON_IsString(id)) ? id->valuestring : "";
    out.mime    = (m  && cJSON_IsString(m))  ? m->valuestring  : "";

    /* Note: caller must cJSON_Delete(root); omitted here for test brevity */
    cJSON_Delete(root);
    return out;
}

static bool is_valid_msg_type(const char *t)
{
    return strcmp(t, "prompt") == 0
        || strcmp(t, "message") == 0
        || strcmp(t, "audio_start") == 0
        || strcmp(t, "audio_end") == 0;
}

/* ── tests ──────────────────────────────────────────────────── */

TEST_CASE("prompt message parses correctly", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("{\"type\":\"prompt\",\"content\":\"Hello\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("prompt", m.type);
    TEST_ASSERT_EQUAL_STRING("Hello", m.content);
    TEST_ASSERT_TRUE(is_valid_msg_type(m.type));
}

TEST_CASE("legacy message type parses correctly", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("{\"type\":\"message\",\"content\":\"Hi\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("message", m.type);
    TEST_ASSERT_EQUAL_STRING("Hi", m.content);
    TEST_ASSERT_TRUE(is_valid_msg_type(m.type));
}

TEST_CASE("audio_start with mime parses correctly", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text(
        "{\"type\":\"audio_start\",\"content\":\"abc123\",\"mime\":\"audio/webm\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("audio_start", m.type);
    TEST_ASSERT_EQUAL_STRING("abc123", m.content);
    TEST_ASSERT_EQUAL_STRING("audio/webm", m.mime);
    TEST_ASSERT_TRUE(is_valid_msg_type(m.type));
}

TEST_CASE("audio_end parses correctly", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("{\"type\":\"audio_end\",\"content\":\"abc123\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("audio_end", m.type);
    TEST_ASSERT_TRUE(is_valid_msg_type(m.type));
}

TEST_CASE("invalid JSON returns not-valid", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("not json at all");
    TEST_ASSERT_FALSE(m.valid);
}

TEST_CASE("empty JSON object has empty type and content", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("{}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("", m.type);
    TEST_ASSERT_EQUAL_STRING("", m.content);
    TEST_ASSERT_FALSE(is_valid_msg_type(m.type));
}

TEST_CASE("unknown type is not a valid msg type", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("{\"type\":\"hack\",\"content\":\"x\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_FALSE(is_valid_msg_type(m.type));
}

TEST_CASE("chat_id field is extracted", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text(
        "{\"type\":\"prompt\",\"content\":\"Hi\",\"chat_id\":\"ws_42\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("ws_42", m.chat_id);
}

TEST_CASE("missing content field yields empty string", "[ws_msg]")
{
    parsed_msg_t m = parse_ws_text("{\"type\":\"prompt\"}");
    TEST_ASSERT_TRUE(m.valid);
    TEST_ASSERT_EQUAL_STRING("prompt", m.type);
    TEST_ASSERT_EQUAL_STRING("", m.content);
}

TEST_CASE("PONG payload 'ping' is invalid JSON", "[ws_msg]")
{
    /* This is what was causing spurious warnings before the PONG fix.
     * ping is not valid JSON — confirms the guard is needed. */
    parsed_msg_t m = parse_ws_text("ping");
    TEST_ASSERT_FALSE(m.valid);
}
