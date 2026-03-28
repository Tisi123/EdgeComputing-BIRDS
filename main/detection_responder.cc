#include "detection_responder.h"
#include "tensorflow/lite/micro/micro_log.h"
#include <sys/time.h>
#include "esp_main.h"

#if DISPLAY_SUPPORT
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "image_provider.h"
#include <cstring>

// Camera frame (96x96) is upscaled to 192x192 for display.
#define IMG_WD (96 * 2)
#define IMG_HT (96 * 2)

static lv_obj_t *camera_canvas = nullptr;
static lv_obj_t *status_indicator = nullptr;
static lv_obj_t *label = nullptr;
static lv_color_t *canvas_buf = nullptr;
#endif  // DISPLAY_SUPPORT

extern "C" {
#include "bird_detection_storage.h"
#include "esp_timer.h"
}

// Confidence threshold applied to the single bird probability output.
// Storage throttling is handled separately by STORE_COOLDOWN_MS.
#define CONFIDENCE_THRESHOLD 0.68f
// Minimum time between stored detections (ms) to avoid duplicates.
#define STORE_COOLDOWN_MS 300

#if DISPLAY_SUPPORT
void create_gui() {
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority = CONFIG_BSP_DISPLAY_LVGL_TASK_PRIORITY,
            .task_stack = 6144,
            .task_affinity = 1,
            .timer_period_ms = CONFIG_BSP_DISPLAY_LVGL_TICK,
        },
        .buffer_size = 240 * 20,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);
    if (canvas_buf == nullptr) {
        canvas_buf = static_cast<lv_color_t *>(
            heap_caps_malloc(IMG_WD * IMG_HT * sizeof(lv_color_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (canvas_buf == nullptr) {
        bsp_display_unlock();
        return;
    }

    camera_canvas = lv_canvas_create(lv_scr_act());
    assert(camera_canvas);
    lv_canvas_set_buffer(camera_canvas, canvas_buf, IMG_WD, IMG_HT, LV_COLOR_FORMAT_NATIVE);
    lv_obj_align(camera_canvas, LV_ALIGN_TOP_MID, 0, 0);

    status_indicator = lv_led_create(lv_scr_act());
    assert(status_indicator);
    lv_obj_align(status_indicator, LV_ALIGN_BOTTOM_MID, -70, 0);

    label = lv_label_create(lv_scr_act());
    assert(label);
    lv_obj_align_to(label, status_indicator, LV_ALIGN_OUT_RIGHT_MID, 20, 0);
    lv_label_set_text_static(label, "Status: waiting");
    bsp_display_unlock();
}
#else
void create_gui() {}
#endif  // DISPLAY_SUPPORT

#if DISPLAY_SUPPORT
void display_prepare_for_sleep() {
    // Best-effort: if display wasn't initialized, just turn off backlight/stop LVGL.
    if (bsp_display_lock(100)) {
        if (camera_canvas && canvas_buf) {
            std::memset(canvas_buf, 0, IMG_WD * IMG_HT * sizeof(lv_color_t));
            lv_obj_invalidate(camera_canvas);
        }
        if (label) {
            lv_label_set_text_static(label, "");
        }
        if (status_indicator) {
            lv_led_off(status_indicator);
        }
        bsp_display_unlock();
    }
    // Stop LVGL task and turn off backlight before pin holds.
    lvgl_port_stop();
    bsp_display_backlight_off();
}
#else
void display_prepare_for_sleep() {}
#endif  // DISPLAY_SUPPORT

void RespondToDetection(float bird_score,
                        float not_bird_score,
                        const uint8_t *image,
                        size_t image_len) {
#if DISPLAY_SUPPORT
    if (!camera_canvas || !status_indicator || !label) {
        create_gui();
    }
    if (!camera_canvas || !status_indicator || !label) {
        return;
    }

    uint16_t *buf = static_cast<uint16_t *>(image_provider_get_display_buf());
    if (buf == nullptr) {
        return;
    }

    const bool is_bird = bird_score >= CONFIDENCE_THRESHOLD;
    const char *predicted_label = is_bird ? "bird" : "not_bird";
    const int bird_pct = static_cast<int>(bird_score * 100.0f + 0.5f);
    const int not_bird_pct = static_cast<int>(not_bird_score * 100.0f + 0.5f);

    bsp_display_lock(0);
    if (is_bird) {
        lv_led_set_color(status_indicator, lv_palette_main(LV_PALETTE_GREEN));
        lv_led_on(status_indicator);
    } else {
        lv_led_set_color(status_indicator, lv_palette_main(LV_PALETTE_RED));
        lv_led_on(status_indicator);
    }

    std::memcpy(canvas_buf, buf, IMG_WD * IMG_HT * sizeof(uint16_t));
    lv_obj_invalidate(camera_canvas);
    lv_label_set_text_fmt(label,
                          "Status: %s\nbird: %d%%\nnot_bird: %d%%",
                          predicted_label, bird_pct, not_bird_pct);
    bsp_display_unlock();
#endif  // DISPLAY_SUPPORT

    // Only store when the model predicts "bird" above threshold,
    // and not more often than STORE_COOLDOWN_MS.
    static int64_t last_store_time_ms = 0;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (bird_score >= CONFIDENCE_THRESHOLD &&
        (now_ms - last_store_time_ms) >= STORE_COOLDOWN_MS) {
        time_t timestamp;
        time(&timestamp);
        bird_storage_add_detection_with_image("bird", bird_score, timestamp, image, image_len);
        last_store_time_ms = now_ms;

        MicroPrintf("Bird detected: bird (%.2f%%), not_bird=%.2f%% - Buffered: %d - Timestamp: %lld",
                    bird_score * 100.0f, not_bird_score * 100.0f,
                    bird_storage_get_count(), timestamp);
    }
}
