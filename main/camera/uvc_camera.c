#include "uvc_camera.h"
#include "langoustine_config.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "libuvc/libuvc.h"

static const char *TAG = "uvc_cam";

static uvc_context_t *s_ctx = NULL;
static bool s_initialized = false;

/* ── Frame capture context (stack-allocated per capture call) ─── */

typedef struct {
    uint8_t          *buf;
    size_t            buf_size;
    size_t            frame_len;
    SemaphoreHandle_t sem;
    bool              captured;
} capture_ctx_t;

static void frame_callback(uvc_frame_t *frame, void *user_ptr)
{
    capture_ctx_t *ctx = (capture_ctx_t *)user_ptr;
    if (!ctx || ctx->captured) return;

    /* Only accept MJPEG — the data IS the JPEG bytes */
    if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG &&
        frame->frame_format != UVC_FRAME_FORMAT_COMPRESSED) {
        return;
    }

    size_t copy_len = frame->data_bytes;
    if (copy_len > ctx->buf_size) {
        ESP_LOGW(TAG, "Frame %u bytes > buf %u, truncating",
                 (unsigned)copy_len, (unsigned)ctx->buf_size);
        copy_len = ctx->buf_size;
    }

    memcpy(ctx->buf, frame->data, copy_len);
    ctx->frame_len = copy_len;
    ctx->captured  = true;

    /* Callback runs in the USB host event task — use non-ISR semaphore */
    xSemaphoreGive(ctx->sem);
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t uvc_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    uvc_error_t res = uvc_init(&s_ctx, NULL);
    if (res != UVC_SUCCESS) {
        ESP_LOGW(TAG, "uvc_init failed (%d) — no USB camera support", (int)res);
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UVC camera driver initialized (USB OTG on GPIO 19/20)");
    return ESP_OK;
}

void uvc_camera_deinit(void)
{
    if (!s_initialized) return;
    uvc_exit(s_ctx);
    s_ctx = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "UVC camera driver deinitialized");
}

bool uvc_camera_is_connected(void)
{
    if (!s_initialized) return false;

    uvc_device_t *dev = NULL;
    uvc_error_t res = uvc_find_device(s_ctx, &dev, 0, 0, NULL);
    if (res == UVC_SUCCESS && dev) {
        uvc_unref_device(dev);
        return true;
    }
    return false;
}

esp_err_t uvc_camera_capture(uint8_t *buf, size_t buf_size,
                              size_t *jpeg_len, int timeout_ms)
{
    if (!buf || !jpeg_len || buf_size == 0) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    *jpeg_len = 0;

    /* Find any UVC device */
    uvc_device_t *dev = NULL;
    uvc_error_t res = uvc_find_device(s_ctx, &dev, 0, 0, NULL);
    if (res != UVC_SUCCESS || !dev) {
        ESP_LOGE(TAG, "No UVC device found (%d)", (int)res);
        return ESP_ERR_NOT_FOUND;
    }

    uvc_device_handle_t *devh = NULL;
    res = uvc_open(dev, &devh);
    uvc_unref_device(dev);
    if (res != UVC_SUCCESS) {
        ESP_LOGE(TAG, "uvc_open failed (%d)", (int)res);
        return ESP_FAIL;
    }

    /* Try 640×480 MJPEG @ 15fps, fall back to 320×240 */
    uvc_stream_ctrl_t ctrl;
    res = uvc_get_stream_ctrl_format_size(devh, &ctrl,
                                          UVC_FRAME_FORMAT_MJPEG, 640, 480, 15);
    if (res != UVC_SUCCESS) {
        ESP_LOGW(TAG, "640x480 not supported, trying 320x240");
        res = uvc_get_stream_ctrl_format_size(devh, &ctrl,
                                              UVC_FRAME_FORMAT_MJPEG, 320, 240, 15);
    }
    if (res != UVC_SUCCESS) {
        ESP_LOGE(TAG, "No supported MJPEG format (%d)", (int)res);
        uvc_close(devh);
        return ESP_FAIL;
    }

    capture_ctx_t ctx = {
        .buf      = buf,
        .buf_size = buf_size,
        .frame_len = 0,
        .captured  = false,
    };
    ctx.sem = xSemaphoreCreateBinary();
    if (!ctx.sem) {
        uvc_close(devh);
        return ESP_ERR_NO_MEM;
    }

    res = uvc_start_streaming(devh, &ctrl, frame_callback, &ctx, 0);
    if (res != UVC_SUCCESS) {
        ESP_LOGE(TAG, "uvc_start_streaming failed (%d)", (int)res);
        vSemaphoreDelete(ctx.sem);
        uvc_close(devh);
        return ESP_FAIL;
    }

    bool got_frame = xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    uvc_stop_streaming(devh);
    uvc_close(devh);
    vSemaphoreDelete(ctx.sem);

    if (!got_frame) {
        ESP_LOGE(TAG, "Capture timeout after %d ms", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    *jpeg_len = ctx.frame_len;
    ESP_LOGI(TAG, "Captured %u-byte JPEG", (unsigned)*jpeg_len);
    return ESP_OK;
}
