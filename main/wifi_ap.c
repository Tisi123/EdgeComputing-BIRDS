#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

#define WIFI_SSID "BirdhouseNet"
#define WIFI_PASS "bird1234"

// Set these to your home/office router credentials.
#define ROUTER_SSID "YourRouter"
#define ROUTER_PASS "YourPass"

static const char *TAG = "wifi_ap";
static bool s_wifi_inited = false;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;

#define STA_CONNECTED_BIT BIT0
#define STA_FAIL_BIT      BIT1

static EventGroupHandle_t s_sta_event_group;
static int s_sta_retry_num = 0;

static void wifi_init_if_needed(void)
{
    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        s_wifi_inited = true;
    }
}

static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_retry_num < 5) {
            esp_wifi_connect();
            s_sta_retry_num++;
        } else {
            xEventGroupSetBits(s_sta_event_group, STA_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_retry_num = 0;
        xEventGroupSetBits(s_sta_event_group, STA_CONNECTED_BIT);
    }
}

static bool sync_time_with_sntp(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    for (int i = 0; i < 15; i++) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2020 - 1900)) {
            esp_sntp_stop();
    
                struct timeval tv;
                tv.tv_sec = now;  // Set seconds
                tv.tv_usec = 0;           // Useless for epoch, keep 0
                settimeofday(&tv, NULL);
            
            ESP_LOGI(TAG, "Time sync complete. Unix time: %ld", (long)now);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_sntp_stop();
    ESP_LOGW(TAG, "SNTP sync timeout");
    return false;
}

void wifi_sync_time_from_router_once(void)
{
    if (strlen(ROUTER_SSID) == 0) {
        ESP_LOGW(TAG, "ROUTER_SSID is empty, skipping time sync");
        return;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    wifi_init_if_needed();

    s_sta_event_group = xEventGroupCreate();
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_sta_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_sta_event_handler, NULL, &instance_got_ip));

    wifi_config_t sta_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = 5,
        },
    };
    strncpy((char *)sta_config.sta.ssid, ROUTER_SSID, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, ROUTER_PASS, sizeof(sta_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_sta_event_group,
        STA_CONNECTED_BIT | STA_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(20000));

    if (bits & STA_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to router SSID: %s", ROUTER_SSID);
        (void)sync_time_with_sntp();
    } else {
        ESP_LOGW(TAG, "Could not connect to router SSID: %s", ROUTER_SSID);
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    vEventGroupDelete(s_sta_event_group);
}

void wifi_init_softap(void)
{
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_init_if_needed();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = WIFI_PASS,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started. SSID: %s", WIFI_SSID);
}
