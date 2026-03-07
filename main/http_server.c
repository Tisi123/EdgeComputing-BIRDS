#include "esp_http_server.h"
#include "esp_log.h"
#include "time.h"
#include "sys/time.h"
#include "bird_detection_storage.h"
#include <stdio.h>

static const char *TAG = "http_server";

static void get_iso8601_time(char *buffer, size_t len)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    gmtime_r(&now, &timeinfo);
    strftime(buffer, len, "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
}

static esp_err_t sync_devices_handler(httpd_req_t *req)
{
    char iso_time[32];
    char response[256];

    struct timeval tv;
    gettimeofday(&tv, NULL);
    get_iso8601_time(iso_time, sizeof(iso_time));

    const long long unix_time_ms = ((long long)tv.tv_sec * 1000LL) + ((long long)tv.tv_usec / 1000LL);
    
    snprintf(response, sizeof(response),
             "{"
             "\"device_time\":\"%s\"," 
             "\"unix_time\":%ld,"
             "\"unix_time_ms\":%lld"
             "}",
             iso_time,
             (long)tv.tv_sec,
             unix_time_ms);

    httpd_resp_set_type(req, "application/json");  // Single JSON object
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Get all detections currently buffered in RAM as JSONL
static esp_err_t get_bird_data_handler(httpd_req_t *req)
{
    static char response[8192];  // Large buffer for multiple detections
    
    // Get JSONL format (newline-separated JSON objects)
    size_t len = bird_storage_get_all_jsonl(response, sizeof(response));
    
    if (len == 0) {
        // Return empty response with 204 No Content
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    
    if (len >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Buffer overflow");
        return ESP_FAIL;
    }

    // JSONL uses text/plain or application/x-ndjson
    httpd_resp_set_type(req, "application/x-ndjson");
    httpd_resp_send(req, response, len);
    return ESP_OK;
}

// Flush detections to SD card
static esp_err_t flush_to_sd_handler(httpd_req_t *req)
{
    int count = bird_storage_get_count();
    
    if (count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"no_data\",\"message\":\"No detections to flush\"}");
        return ESP_OK;
    }
    
    esp_err_t ret = bird_storage_flush_to_sd();
    
    if (ret == ESP_OK) {
        char response[128];
        snprintf(response, sizeof(response), 
                 "{\"status\":\"success\",\"flushed\":%d}", count);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, response);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD flush failed");
    }
    
    return ret;
}

// Get ALL detections from SD card (historical data)
static esp_err_t get_bird_history_handler(httpd_req_t *req)
{
    // Allocate large buffer for SD card data
    char *response = malloc(32768);  // 32KB for historical data
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    
    size_t len = bird_storage_read_from_sd(response, 32768);
    
    if (len == 0) {
        free(response);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No data on SD card");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/x-ndjson");
    httpd_resp_send(req, response, len);
    
    free(response);
    return ESP_OK;
}

httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t bird_data_uri = {
            .uri = "/get_bird_data_request",
            .method = HTTP_GET,
            .handler = get_bird_data_handler,
            .user_ctx = NULL,
        };

        httpd_uri_t sync_devices_uri = {
            .uri = "/sync_devices_request",
            .method = HTTP_GET,
            .handler = sync_devices_handler,
            .user_ctx = NULL,
        };
        
        httpd_uri_t flush_sd_uri = {
            .uri = "/flush_to_sd",
            .method = HTTP_POST,
            .handler = flush_to_sd_handler,
            .user_ctx = NULL,
        };
        httpd_uri_t bird_history_uri = {
            .uri = "/get_bird_history",
            .method = HTTP_GET,
            .handler = get_bird_history_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(server, &bird_history_uri);
        httpd_register_uri_handler(server, &bird_data_uri);
        httpd_register_uri_handler(server, &sync_devices_uri);
        httpd_register_uri_handler(server, &flush_sd_uri);
        
        ESP_LOGI(TAG, "HTTP server started with bird detection endpoints");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }

    return server;
}
