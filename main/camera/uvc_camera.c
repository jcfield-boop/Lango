#include "uvc_camera.h"
#include "langoustine_config.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
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
    ESP_LOGI(TAG, "usb_lib_task running (waiting for USB events)");
    while (1) {
        uint32_t event_flags = 0;
        /* Use a 10 s timeout to emit a heartbeat if nothing arrives */
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(10000), &event_flags);
        if (err == ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "usb_lib_task alive — no USB events in 10 s");
            continue;
        }
        ESP_LOGI(TAG, "usb_lib_task event: flags=0x%lx", (unsigned long)event_flags);
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
        uint8_t dev_addr   = event->device_connected.dev_addr;
        uint8_t stream_idx = event->device_connected.uvc_stream_index;
        ESP_LOGI(TAG, "UVC device connected (addr=%d, stream=%d, frames=%d)",
                 dev_addr, stream_idx, (int)event->device_connected.frame_info_num);

        /* Log every supported frame format so we know what to request */
        uvc_host_frame_info_t frames[24];
        size_t num = sizeof(frames) / sizeof(frames[0]);
        if (uvc_host_get_frame_list(dev_addr, stream_idx, &frames, &num) == ESP_OK) {
            for (size_t i = 0; i < num; i++) {
                const char *fmt =
                    (frames[i].format == UVC_VS_FORMAT_MJPEG)   ? "MJPEG" :
                    (frames[i].format == UVC_VS_FORMAT_YUY2)    ? "YUY2"  :
                    (frames[i].format == UVC_VS_FORMAT_H264)    ? "H264"  :
                    (frames[i].format == UVC_VS_FORMAT_H265)    ? "H265"  : "OTHER";
                uint32_t fps_val = frames[i].default_interval
                    ? 10000000u / frames[i].default_interval : 0;
                ESP_LOGI(TAG, "  format[%d] %s %ux%u @%"PRIu32"fps",
                         (int)i, fmt, frames[i].h_res, frames[i].v_res, fps_val);
            }
        } else {
            ESP_LOGW(TAG, "  uvc_host_get_frame_list failed");
        }
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
    int64_t           warmup_done_us; /* discard frames until this timestamp */
} capture_ctx_t;

static bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    capture_ctx_t *ctx = (capture_ctx_t *)user_ctx;
    if (!ctx || ctx->captured) return true;

    /* Time-based warmup: discard frames until AEC/AGC has had time to settle.
     * More reliable than frame-count: works regardless of negotiated FPS. */
    if (esp_timer_get_time() < ctx->warmup_done_us) {
        return true;
    }

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

    /* USB library event task (Core 0, priority 15).
     * Must be running BEFORE class drivers start doing USB transfers so that
     * usb_host_lib_handle_events() can drain the completed-transfer queue.
     * Without it the transfer pool fills and the USB daemon asserts. */
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
        if (s_usb_lib_task) {
            vTaskDelete(s_usb_lib_task);
            s_usb_lib_task = NULL;
        }
        usb_host_uninstall();
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UVC camera driver initialized (USB OTG on GPIO 19/20)");
    return ESP_OK;
}

void uvc_camera_start_host_task(void)
{
    if (s_usb_lib_task) return;   /* already started */
    if (!s_initialized) return;   /* init failed — nothing to drive */

    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL,
                                15, &s_usb_lib_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create usb_lib_task — USB events will not be processed");
    } else {
        ESP_LOGI(TAG, "usb_lib_task started — USB host now processing events");
    }
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
        .buf            = buf,
        .buf_size       = buf_size,
        .frame_len      = 0,
        .captured       = false,
        .warmup_done_us = INT64_MAX, /* set after stream starts */
    };
    ctx.sem = xSemaphoreCreateBinary();
    if (!ctx.sem) return ESP_ERR_NO_MEM;

    /* Try formats in order of preference.
     * fps=0 means "use camera default fps" per the UVC host API.
     * h_res=0, v_res=0 with DEFAULT format lets the camera pick everything. */
    typedef struct { uint16_t w, h; float fps; enum uvc_host_stream_format fmt; } try_t;
    static const try_t tries[] = {
        {640, 480,  0, UVC_VS_FORMAT_MJPEG},
        {320, 240,  0, UVC_VS_FORMAT_MJPEG},
        {640, 480, 30, UVC_VS_FORMAT_MJPEG},
        {320, 240, 30, UVC_VS_FORMAT_MJPEG},
        {640, 480, 25, UVC_VS_FORMAT_MJPEG},
        {320, 240, 25, UVC_VS_FORMAT_MJPEG},
        {  0,   0,  0, UVC_VS_FORMAT_MJPEG},
        {  0,   0,  0, UVC_VS_FORMAT_DEFAULT},
    };

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
        .advanced = {
            .number_of_frame_buffers = 2,
            .frame_size              = buf_size,
            .frame_heap_caps         = MALLOC_CAP_SPIRAM,
            .number_of_urbs          = 3,
            .urb_size                = 10 * 1024,
        },
    };

    uvc_host_stream_hdl_t stream_hdl = NULL;
    esp_err_t ret = ESP_FAIL;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        stream_cfg.vs_format.h_res  = tries[i].w;
        stream_cfg.vs_format.v_res  = tries[i].h;
        stream_cfg.vs_format.fps    = tries[i].fps;
        stream_cfg.vs_format.format = tries[i].fmt;
        ESP_LOGI(TAG, "Trying %ux%u @%.0ffps fmt=%d",
                 tries[i].w, tries[i].h, tries[i].fps, (int)tries[i].fmt);
        ret = uvc_host_stream_open(&stream_cfg, pdMS_TO_TICKS(500), &stream_hdl);
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_open failed all formats: %s", esp_err_to_name(ret));
        vSemaphoreDelete(ctx.sem);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Stream opened: %ux%u @%.0ffps fmt=%d",
             stream_cfg.vs_format.h_res, stream_cfg.vs_format.v_res,
             stream_cfg.vs_format.fps,   (int)stream_cfg.vs_format.format);


    ret = uvc_host_stream_start(stream_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_stream_start failed: %s", esp_err_to_name(ret));
        uvc_host_stream_close(stream_hdl);
        vSemaphoreDelete(ctx.sem);
        return ESP_FAIL;
    }

    /* Begin time-based warmup now that the stream is running */
    ctx.warmup_done_us = esp_timer_get_time() + (int64_t)LANG_CAMERA_WARMUP_MS * 1000;
    ESP_LOGI(TAG, "Warming up camera for %d ms (AEC/AGC settle)", LANG_CAMERA_WARMUP_MS);

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
