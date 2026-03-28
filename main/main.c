#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "bird_detection_storage.h"
#include "esp_err.h"
#include "main_functions.h"
#include "detection_responder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "app_camera_esp.h"
#include <time.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_main.h"
#include "bird_detection_storage.h"

#if DISPLAY_SUPPORT
#include "bsp/esp-bsp.h"
#endif
#include "bsp/esp32_s3_eye.h"

void wifi_init_softap(void);
void wifi_sync_time_from_router_once(void);
httpd_handle_t start_webserver(void);
static void disable_http_services(void);

static const char *TAG = "main";

#define ACTIVE_START_HOUR_UTC 7
#define ACTIVE_START_MINUTE_UTC 50
#define ACTIVE_END_HOUR_UTC 17
#define ACTIVE_END_MINUTE_UTC 10
#define DEFAULT_HTTP_BOOT_GRACE_SECONDS (10 * 60) // 10 minutes grace period after boot to keep HTTP active for time sync
#define SCHEDULE_TZ_OFFSET_HOURS 1
#define DEEP_SLEEP_WIFI_DEINIT 0
#define CONTROL_GPIO3_LED 1
// Optional external TFT control (only enable if you are NOT using camera on these pins).
#define CONTROL_EXTERNAL_TFT 0
#define SLEEP_PIN_ISOLATION 1
#define TFT_CS_GPIO   GPIO_NUM_9
#define TFT_DC_GPIO   GPIO_NUM_8
#define TFT_SCLK_GPIO GPIO_NUM_10
#define TFT_MOSI_GPIO GPIO_NUM_11
#define TFT_MISO_GPIO GPIO_NUM_13
#define TFT_RST_GPIO  GPIO_NUM_12

typedef struct {
    int start_hour_utc;
    int start_minute_utc;
    int end_hour_utc;
    int end_minute_utc;
    bool enabled;
} time_window_utc_t;

// HTTP windows (GMT+1 schedule): 07:55-08:00 and 17:00-17:05
static time_window_utc_t s_http_windows[] = {
    {.start_hour_utc = 7, .start_minute_utc = 55, .end_hour_utc = 8, .end_minute_utc = 0, .enabled = true},
    {.start_hour_utc = 17, .start_minute_utc = 0, .end_hour_utc = 17, .end_minute_utc = 5, .enabled = true},
};

// Detection window (GMT+1 schedule): 08:00-17:00
static time_window_utc_t s_detection_windows[] = {
    {.start_hour_utc = 8, .start_minute_utc = 0, .end_hour_utc = 22, .end_minute_utc = 0, .enabled = true},
};

static uint32_t s_http_boot_grace_seconds = DEFAULT_HTTP_BOOT_GRACE_SECONDS;
static time_t s_boot_unix_time = 0;
static httpd_handle_t s_http_server = NULL;
static bool s_ap_running = false;
static volatile bool s_detection_enabled = false;
static volatile bool s_detection_paused = false;
static bool is_clock_valid_utc(void);

static void disable_display_pins_for_sleep(void)
{
    // Force LCD-related pins low and hold them during deep sleep to prevent panel power draw.
    const gpio_num_t pins[] = {
        BSP_LCD_SPI_MOSI,
        BSP_LCD_SPI_CLK,
        BSP_LCD_SPI_CS,
        BSP_LCD_DC,
        BSP_LCD_BACKLIGHT,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] == GPIO_NUM_NC) {
            continue;
        }
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins[i], 0);
        gpio_hold_en(pins[i]);
    }

#if CONTROL_EXTERNAL_TFT
    const gpio_num_t tft_pins[] = {
        TFT_CS_GPIO,
        TFT_DC_GPIO,
        TFT_SCLK_GPIO,
        TFT_MOSI_GPIO,
        TFT_MISO_GPIO,
        TFT_RST_GPIO,
    };
    for (size_t i = 0; i < sizeof(tft_pins) / sizeof(tft_pins[0]); ++i) {
        if (tft_pins[i] == GPIO_NUM_NC) {
            continue;
        }
        gpio_reset_pin(tft_pins[i]);
        gpio_set_direction(tft_pins[i], GPIO_MODE_OUTPUT);
        // Hold reset low (sleep) and keep bus lines low to reduce draw.
        gpio_set_level(tft_pins[i], 0);
        gpio_hold_en(tft_pins[i]);
    }
#endif
}

static void apply_sleep_pin_isolation(void)
{
#if SLEEP_PIN_ISOLATION
    // Prevent back-powering during deep sleep.
    const gpio_num_t pins_high[] = { GPIO_NUM_42, GPIO_NUM_39 };
    const gpio_num_t pins_low[] = { GPIO_NUM_45, GPIO_NUM_40, GPIO_NUM_41 };

    for (size_t i = 0; i < sizeof(pins_high) / sizeof(pins_high[0]); ++i) {
        gpio_reset_pin(pins_high[i]);
        gpio_set_direction(pins_high[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins_high[i], 1);
        gpio_hold_en(pins_high[i]);
    }
    for (size_t i = 0; i < sizeof(pins_low) / sizeof(pins_low[0]); ++i) {
        gpio_reset_pin(pins_low[i]);
        gpio_set_direction(pins_low[i], GPIO_MODE_OUTPUT);
        gpio_set_level(pins_low[i], 0);
        gpio_hold_en(pins_low[i]);
    }
#endif
}

static void release_sleep_pin_isolation(void)
{
#if SLEEP_PIN_ISOLATION
    const gpio_num_t pins_all[] = { GPIO_NUM_42, GPIO_NUM_39, GPIO_NUM_45, GPIO_NUM_40, GPIO_NUM_41 };
    for (size_t i = 0; i < sizeof(pins_all) / sizeof(pins_all[0]); ++i) {
        gpio_hold_dis(pins_all[i]);
    }
#endif
}

static void release_display_pins_after_wake(void)
{
    // Clear deep-sleep holds so the display can be re-initialized.
    gpio_deep_sleep_hold_dis();
    const gpio_num_t pins[] = {
        BSP_LCD_SPI_MOSI,
        BSP_LCD_SPI_CLK,
        BSP_LCD_SPI_CS,
        BSP_LCD_DC,
        BSP_LCD_BACKLIGHT,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] == GPIO_NUM_NC) {
            continue;
        }
        gpio_hold_dis(pins[i]);
    }

#if CONTROL_EXTERNAL_TFT
    const gpio_num_t tft_pins[] = {
        TFT_CS_GPIO,
        TFT_DC_GPIO,
        TFT_SCLK_GPIO,
        TFT_MOSI_GPIO,
        TFT_MISO_GPIO,
        TFT_RST_GPIO,
    };
    for (size_t i = 0; i < sizeof(tft_pins) / sizeof(tft_pins[0]); ++i) {
        if (tft_pins[i] == GPIO_NUM_NC) {
            continue;
        }
        gpio_hold_dis(tft_pins[i]);
    }
    // Bring TFT reset high after wake so the panel can initialize.
    if (TFT_RST_GPIO != GPIO_NUM_NC) {
        gpio_reset_pin(TFT_RST_GPIO);
        gpio_set_direction(TFT_RST_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(TFT_RST_GPIO, 1);
    }
#endif

    release_sleep_pin_isolation();
}

static void prepare_for_deep_sleep(void)
{
    // Stop network services to reduce draw.
    disable_http_services();
    esp_err_t wifi_stop_ret = esp_wifi_stop();
    if (wifi_stop_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(wifi_stop_ret));
    }
#if DEEP_SLEEP_WIFI_DEINIT
    esp_err_t wifi_deinit_ret = esp_wifi_deinit();
    if (wifi_deinit_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(wifi_deinit_ret));
    }
#endif

#if ESP_CAMERA_SUPPORTED
    // Release camera resources.
    esp_err_t cam_ret = esp_camera_deinit();
    if (cam_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_deinit failed: %s", esp_err_to_name(cam_ret));
    }
    app_camera_mark_deinit();
#endif

    // Unmount SD card before deep sleep to avoid filesystem issues.
    bird_storage_force_unmount();

#if DISPLAY_SUPPORT
    // Force LCD pins low before sleep to avoid panel power draw.
    // Blank display, stop LVGL, and turn off backlight to avoid a stuck frame.
    display_prepare_for_sleep();
    disable_display_pins_for_sleep();
#endif

    // Turn off the on-board status LED (GPIO3 on S3-EYE).
#ifdef BSP_LED_GREEN
    gpio_reset_pin(BSP_LED_GREEN);
    gpio_set_direction(BSP_LED_GREEN, GPIO_MODE_OUTPUT_OD);
    // Open-drain: drive high to turn LED off (active-low).
    gpio_set_level(BSP_LED_GREEN, 1);
    gpio_hold_en(BSP_LED_GREEN);
#endif

#if CONTROL_GPIO3_LED
    gpio_reset_pin(GPIO_NUM_3);
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_3, 0);
    gpio_hold_en(GPIO_NUM_3);
#endif

    apply_sleep_pin_isolation();

    // Keep GPIO states through deep sleep (e.g., backlight off).
    gpio_deep_sleep_hold_en();

    // Reduce power in unused RTC domains.
    // Disabled for now: can assert on some IDF builds if domain refs are held.
    // esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_OFF);
}

static void set_gpio3_led(bool on)
{
#if CONTROL_GPIO3_LED
    gpio_reset_pin(GPIO_NUM_3);
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    // On this board LOW turns LED off, HIGH turns it on.
    gpio_set_level(GPIO_NUM_3, on ? 1 : 0);
#endif
}

void set_http_server_boot_grace_seconds(uint32_t seconds)
{
    s_http_boot_grace_seconds = seconds;
}

static bool sync_time_before_schedule(void)
{
    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        ESP_LOGI(TAG, "Time sync attempt %d/%d", attempt, max_attempts);
        wifi_sync_time_from_router_once();
        if (is_clock_valid_utc()) {
            ESP_LOGI(TAG, "UTC time is valid after sync");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGW(TAG, "UTC time is still invalid after sync attempts");
    return false;
}

static bool is_clock_valid_utc(void)
{
    time_t now = 0;
    struct tm utc_tm = {0};
    time(&now);
    gmtime_r(&now, &utc_tm);
    return utc_tm.tm_year >= (2020 - 1900);
}

static int seconds_since_midnight_utc(time_t now)
{
    now += (SCHEDULE_TZ_OFFSET_HOURS * 3600);
    struct tm utc_tm = {0};
    gmtime_r(&now, &utc_tm);
    return (utc_tm.tm_hour * 3600) + (utc_tm.tm_min * 60) + utc_tm.tm_sec;
}

static bool is_within_window_utc(time_t now, const time_window_utc_t *window)
{
    if (!window || !window->enabled) {
        return false;
    }

    const int start_sec = (window->start_hour_utc * 3600) + (window->start_minute_utc * 60);
    const int end_sec = (window->end_hour_utc * 3600) + (window->end_minute_utc * 60);
    const int now_sec = seconds_since_midnight_utc(now);
    return (now_sec >= start_sec) && (now_sec < end_sec);
}

static bool is_within_any_window_utc(time_t now, const time_window_utc_t *windows, size_t count)
{
    if (!windows || count == 0) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        if (is_within_window_utc(now, &windows[i])) {
            return true;
        }
    }
    return false;
}

static uint32_t seconds_until_next_window_start_utc(time_t now, const time_window_utc_t *window)
{
    if (!window || !window->enabled) {
        return UINT32_MAX;
    }

    const int start_sec = (window->start_hour_utc * 3600) + (window->start_minute_utc * 60);
    const int now_sec = seconds_since_midnight_utc(now);

    if (now_sec < start_sec) {
        return (uint32_t)(start_sec - now_sec);
    }
    return (uint32_t)((24 * 3600 - now_sec) + start_sec);
}

static uint32_t seconds_until_next_any_window_start_utc(time_t now, const time_window_utc_t *windows, size_t count)
{
    uint32_t min_seconds = UINT32_MAX;
    bool any_enabled = false;
    if (!windows || count == 0) {
        return UINT32_MAX;
    }

    for (size_t i = 0; i < count; ++i) {
        if (!windows[i].enabled) {
            continue;
        }
        any_enabled = true;
        uint32_t seconds = seconds_until_next_window_start_utc(now, &windows[i]);
        if (seconds < min_seconds) {
            min_seconds = seconds;
        }
    }

    if (!any_enabled) {
        return UINT32_MAX;
    }
    return min_seconds;
}

static bool is_http_boot_grace_active(time_t now)
{
    if (s_http_boot_grace_seconds == 0 || s_boot_unix_time <= 0) {
        return false;
    }
    return (now - s_boot_unix_time) < (time_t)s_http_boot_grace_seconds;
}

static bool should_http_server_run(time_t now)
{
    if (is_http_boot_grace_active(now)) {
        return true;
    }
    return is_within_any_window_utc(now, s_http_windows, sizeof(s_http_windows) / sizeof(s_http_windows[0]));
}

static bool should_run_detection(time_t now)
{
    return is_within_any_window_utc(now, s_detection_windows, sizeof(s_detection_windows) / sizeof(s_detection_windows[0]));
}

static bool should_device_stay_awake(time_t now)
{
    return should_http_server_run(now) || should_run_detection(now);
}

static void enable_http_services(void)
{
    if (!s_ap_running) {
        wifi_init_softap();
        s_ap_running = true;
    }
    if (s_http_server == NULL) {
        s_http_server = start_webserver();
    }
}

static void disable_http_services(void)
{
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    if (s_ap_running) {
        esp_wifi_stop();
        s_ap_running = false;
    }
}

static void enter_deep_sleep_until_next_active_start(void)
{
    time_t now = 0;
    time(&now);
    uint32_t next_http = seconds_until_next_any_window_start_utc(
        now,
        s_http_windows,
        sizeof(s_http_windows) / sizeof(s_http_windows[0]));
    uint32_t next_detection = seconds_until_next_any_window_start_utc(
        now,
        s_detection_windows,
        sizeof(s_detection_windows) / sizeof(s_detection_windows[0]));
    uint32_t sleep_seconds;
    if (next_http == UINT32_MAX && next_detection == UINT32_MAX) {
        sleep_seconds = 3600;  // fallback: 1 hour
    } else if (next_http == UINT32_MAX) {
        sleep_seconds = next_detection;
    } else if (next_detection == UINT32_MAX) {
        sleep_seconds = next_http;
    } else {
        sleep_seconds = (next_http < next_detection) ? next_http : next_detection;
    }

    if (sleep_seconds == 0) {
        sleep_seconds = 1;
    }

    struct tm local_tm = {0};
    time_t now_offset = now + (SCHEDULE_TZ_OFFSET_HOURS * 3600);
    gmtime_r(&now_offset, &local_tm);
    ESP_LOGI(TAG,
             "Sleep calc: now=%02d:%02d:%02d GMT+%d, next_http=%" PRIu32 "s, next_detection=%" PRIu32 "s, grace=%s",
             local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
             SCHEDULE_TZ_OFFSET_HOURS,
             next_http, next_detection,
             is_http_boot_grace_active(now) ? "on" : "off");

    ESP_LOGI(TAG,
             "Outside all windows, entering deep sleep for %" PRIu32 " seconds",
             sleep_seconds);
    ESP_LOGI(TAG, "Preparing peripherals for deep sleep");
    prepare_for_deep_sleep();
    ESP_LOGI(TAG, "Entering deep sleep now");
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL);
    esp_deep_sleep_start();
}

static void tf_inference_task(void *arg)
{
    (void)arg;
    bool tf_ready = false;

    while (true) {
        if (!s_detection_enabled || s_detection_paused) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!tf_ready) {
            ESP_LOGI(TAG, "Starting TensorFlow setup");
            setup();
            tf_ready = inference_ready();
            if (!tf_ready) {
                ESP_LOGE(TAG, "TensorFlow setup failed, retrying in 2 seconds");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        loop();
    }
}

void set_detection_paused(bool paused)
{
    s_detection_paused = paused;
}

bool is_detection_paused(void)
{
    return s_detection_paused;
}

static void schedule_guard_task(void *arg)
{
    (void)arg;
    while (true) {
        time_t now = 0;
        time(&now);

        if (should_http_server_run(now)) {
            enable_http_services();
        } else {
            disable_http_services();
        }

        bool new_detection_enabled = should_run_detection(now);
        if (new_detection_enabled != s_detection_enabled) {
            s_detection_enabled = new_detection_enabled;
            set_gpio3_led(s_detection_enabled);
        } else {
            s_detection_enabled = new_detection_enabled;
        }

        if (!is_http_boot_grace_active(now) && !should_device_stay_awake(now)) {
            ESP_LOGI(TAG, "No active windows, preparing for deep sleep");
            bird_storage_flush_to_sd();
            disable_http_services();
            vTaskDelay(pdMS_TO_TICKS(200));
            enter_deep_sleep_until_next_active_start();
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    release_display_pins_after_wake();

    bool time_valid = sync_time_before_schedule();
    time(&s_boot_unix_time);

    // Keep HTTP active after power-up for sync testing.
    set_http_server_boot_grace_seconds(DEFAULT_HTTP_BOOT_GRACE_SECONDS);

    if (!time_valid) {
        ESP_LOGW(TAG, "UTC clock is not valid, keeping device awake to avoid missing schedule");
    } else {
        time_t now = 0;
        time(&now);
        if (!is_http_boot_grace_active(now) && !should_device_stay_awake(now)) {
            enter_deep_sleep_until_next_active_start();
        }
    }

    time_t now = 0;
    time(&now);
    s_detection_enabled = should_run_detection(now);

    if (should_http_server_run(now) || !time_valid) {
        enable_http_services();
    }

    set_gpio3_led(s_detection_enabled);

    // Initialize bird detection storage
    bird_storage_init();

    BaseType_t task_ok = xTaskCreate(
        tf_inference_task,
        "tf_inference",
        8 * 1024,
        NULL,
        8,
        NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create tf_inference task");
    } else {
        ESP_LOGI(TAG, "Inference task created (runs only during detection windows)");
    }

    BaseType_t guard_ok = xTaskCreate(
        schedule_guard_task,
        "schedule_guard",
        4 * 1024,
        NULL,
        5,
        NULL);
    if (guard_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create schedule_guard task");
    }
}
