#include "uvc_camera.h"
#include "langoustine_config.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

static const char *TAG = "uvc_cam";

static bool          s_initialized      = false;
static bool          s_device_connected = false;
static TaskHandle_t  s_usb_lib_task     = NULL;

/* ── USB host library event task ──────────────────────────────── */

static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* ── Driver / stream event callbacks ───────────────────────────── */

static void driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        s_device_connected = true;
        ESP_LOGI(TAG, "UVC device connected (addr=%d)", event->device_connected.dev_addr);
    }
}

static void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        s_device_connected = false;
        ESP_LOGW(TAG, "UVC device disconnected");
    }
}

/* ── Frame capture context ─────────────────────────────────────── */

typedef struct {
    uint8_t          *buf;
    size_t            buf_size;
    size_t            frame_len;
    SemaphoreHandle_t sem;
    bool              captured;
} capture_ctx_t;

/*
 * Returns true  → frame processed, driver reclaims buffer immediately.
 * Returns false → frame NOT yet processed, caller must call uvc_host_frame_return().
 * We copy data inline and return true so we avoid the extra return call.
 */
static bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    capture_ctx_t *ctx = (capture_ctx_t *)user_ctx;
    if (!ctx || ctx->captured) return true;

    size_t copy_len = frame->data_len;
    if (copy_len > ctx->buf_size) {
        ESP_LOGW(TAG, "Frame %u bytes > buf %u, truncating",
                 (unsigned)copy_len, (unsigned)ctx->buf_size);
        copy_len = ctx->buf_size;
    }

    memcpy(ctx->buf, frame->data, copy_len);
    ctx->frame_len = copy_len;
    ctx->captured  = true;
    xSemaphoreGive(ctx->sem);
    return true;
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t uvc_camera_init(void)
{
    if (s_initialized) return ESP_OK;

    /* Install USB Host Library */
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb_host_install failed (%s) — no USB camera support",
                 esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* USB library event task (Core 0, priority 15) */
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL,
                                15, &s_usb_lib_task, 0) != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    /* Install UVC driver */
    const uvc_host_driver_config_t uvc_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = 16,
        .xCoreID                = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb               = driver_event_cb,
        .user_ctx               = NULL,
    };
    err = uvc_host_install(&uvc_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uvc_host_install failed (%s) — no USB camera support",
                 esp_err_to_name(err));
        vTaskDelete(s_usb_lib_task);
        s_usb_lib_task = NULL;
        usb_host_uninstall();
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UVC camera driver initialized (USB OTG on GPIO 19/20)");
    return ESP_OK;
}

void uvc_camera_deinit(void)
{
    if (!s_initialized) return;
    uvc_host_uninstall();
    if (s_usb_lib_task) {
        vTaskDelete(s_usb_lib_task);
        s_usb_lib_task = NULL;
    }
    usb_host_uninstall();
    s_initialized      = false;
    s_device_connected = false;
    ESP_LOGI(TAG, "UVC camera driver deinitialized");
}

bool uvc_camera_is_connected(void)
{
    return s_initialized && s_device_connected;
}

esp_err_t uvc_camera_capture(uint8_t *buf, size_t buf_size,
                              size_t *jpeg_len, int timeout_ms)
{
    if (!buf || !jpeg_len || buf_size == 0) return ESP_ERR_INVALID_ARG;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    *jpeg_len = 0;

    capture_ctx_t ctx = {
        .buf       = buf,
        .buf_size  = buf_size,
        .frame_len = 0,
        .captured  = false,
    };
    ctx.sem = xSemaphoreCreateBinary();
    if (!ctx.sem) return ESP_ERR_NO_MEM;

    /* Try 640×480 MJPEG @ 15fps, fall back to 320×240 */
    uvc_host_stream_config_t stream_cfg = {
        .event_cb = stream_event_cb,
        .frame_cb = frame_cb,
        .user_ctx = &ctx,
        .usb = {
            .dev_addr         = UVC_HOST_ANY_DEV_ADDR,
            .vid              = UVC_HOST_ANY_VID,
            .pid              = UVC_HOST_ANY_PID,
            .uvc_stream_index = 0,
        },
        .vs_format = {
            .h_res  = 640,
            .v_res  = 480,
            .fps    = 15.0f,
            .format = UVC_VS_FORMAT_MJPEG,
        },
        .advanced = {
            .number_of_frame_buffers = 2,
            .frame_size              = buf_size,
            .frame_heap_caps         = MALLOC_CAP_SPIRAM,
            .number_of_urbs          = 3,
            .urb_size                = 10 * 1024,
        },
    };

    uvc_host_stream_hdl_t stream_hdl = NULL;
    esp_err_t ret = uvc_host_stream_open(&stream_cfg, pdMS_TO_TICKS(timeout_ms / 2), &stream_hdl);
    if (ret != ESP_OK) {
        /* Try lower resolution */
        stream_cfg.vs_format.h_res = 320;
        stream_cfg.vs_format.v_res = 240;
        ret = uvc_host_stream_open(&stream_cfg, pdMS_TO_TICKS(timeout_ms / 2), &stream_hdl);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_open failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(ctx.sem);
        return ESP_ERR_NOT_FOUND;
    }

    ret = uvc_host_stream_start(stream_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_start failed: %s", esp_err_to_name(ret));
        uvc_host_stream_close(stream_hdl);
        vSemaphoreDelete(ctx.sem);
        return ESP_FAIL;
    }

    bool got_frame = xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    uvc_host_stream_stop(stream_hdl);
    uvc_host_stream_close(stream_hdl);
    vSemaphoreDelete(ctx.sem);

    if (!got_frame) {
        ESP_LOGE(TAG, "Capture timeout after %d ms", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    *jpeg_len = ctx.frame_len;
    ESP_LOGI(TAG, "Captured %u-byte JPEG", (unsigned)*jpeg_len);
    return ESP_OK;
}
