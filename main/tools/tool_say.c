#include "tool_say.h"
#include "langoustine_config.h"
#include "audio/tts_client.h"
#include "audio/i2s_audio.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_say";

/* ── Core speak function ─────────────────────────────────────── */

esp_err_t say_speak(const char *text)
{
    if (!text || text[0] == '\0') {
        ESP_LOGW(TAG, "Empty text — nothing to speak");
        return ESP_ERR_INVALID_ARG;
    }

    char tts_id[9] = {0};
    esp_err_t err = tts_generate(text, tts_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS failed: %s", esp_err_to_name(err));
        return err;
    }

    const uint8_t *wav = NULL;
    size_t wav_len = 0;
    err = tts_cache_get(tts_id, &wav, &wav_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS cache miss for id=%s", tts_id);
        return err;
    }

#if LANG_I2S_AUDIO_ENABLED
    ESP_LOGI(TAG, "Speaking: \"%.*s%s\" (%u bytes WAV)",
             (int)(strlen(text) > 40 ? 40 : strlen(text)), text,
             strlen(text) > 40 ? "..." : "",
             (unsigned)wav_len);
    err = i2s_audio_play_wav_async(wav, wav_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S play failed: %s", esp_err_to_name(err));
        return err;
    }
#else
    ESP_LOGW(TAG, "I2S audio disabled — TTS generated but not played");
#endif

    return ESP_OK;
}

/* ── Tool execute (JSON interface for agent) ─────────────────── */

esp_err_t tool_say_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text_item) || !text_item->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'text' field is required");
        return ESP_ERR_INVALID_ARG;
    }

    /* Copy text before deleting cJSON tree */
    char text[512];
    strncpy(text, text_item->valuestring, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    cJSON_Delete(root);

    esp_err_t err = say_speak(text);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: spoken aloud");
    } else {
        snprintf(output, output_size, "Error: TTS/playback failed (%s)", esp_err_to_name(err));
    }

    return err;
}
