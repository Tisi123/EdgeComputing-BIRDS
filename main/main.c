#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "bird_detection_storage.h"
#include "esp_err.h"
#include "main_functions.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include <time.h>
#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>

void wifi_init_softap(void);
void wifi_sync_time_from_router_once(void);
httpd_handle_t start_webserver(void);

static const char *TAG = "main";

#define ACTIVE_START_HOUR_UTC 7
#define ACTIVE_START_MINUTE_UTC 50
#define ACTIVE_END_HOUR_UTC 17
#define ACTIVE_END_MINUTE_UTC 10
#define DEFAULT_HTTP_BOOT_GRACE_SECONDS (10 * 60) // 10 minutes grace period after boot to keep HTTP active for time sync
#define SCHEDULE_TZ_OFFSET_HOURS 1

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
    {.start_hour_utc = 8, .start_minute_utc = 0, .end_hour_utc = 17, .end_minute_utc = 0, .enabled = true},
};

static uint32_t s_http_boot_grace_seconds = DEFAULT_HTTP_BOOT_GRACE_SECONDS;
static time_t s_boot_unix_time = 0;
static httpd_handle_t s_http_server = NULL;
static bool s_ap_running = false;
static volatile bool s_detection_enabled = false;
static bool is_clock_valid_utc(void);

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
    if (!windows || count == 0) {
        return 60;
    }

    for (size_t i = 0; i < count; ++i) {
        uint32_t seconds = seconds_until_next_window_start_utc(now, &windows[i]);
        if (seconds < min_seconds) {
            min_seconds = seconds;
        }
    }

    if (min_seconds == UINT32_MAX) {
        return 60;
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
    uint32_t sleep_seconds = (next_http < next_detection) ? next_http : next_detection;

    if (sleep_seconds == 0) {
        sleep_seconds = 1;
    }

    ESP_LOGI(TAG,
             "Outside all windows, entering deep sleep for %" PRIu32 " seconds",
             sleep_seconds);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL);
    esp_deep_sleep_start();
}

static void tf_inference_task(void *arg)
{
    (void)arg;
    bool tf_ready = false;

    while (true) {
        if (!s_detection_enabled) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (!tf_ready) {
            ESP_LOGI(TAG, "Starting TensorFlow setup");
            setup();
            tf_ready = true;
        }

        loop();
    }
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

        s_detection_enabled = should_run_detection(now);

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
