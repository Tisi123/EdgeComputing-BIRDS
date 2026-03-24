#include "bird_detection_storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "img_converters.h"
#include "esp_camera.h"
#include "app_camera_esp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "bird_storage";

#define MAX_DETECTIONS 10
#define MAX_SPECIES_LEN 32
#define SDCARD_MOUNT_POINT "/sd"
#define BIRDS_FILE SDCARD_MOUNT_POINT"/birds.txt"
#define STORAGE_DEBUG 0
#define SAVE_IMAGES_TO_SD 0
#define FLUSH_ASYNC_RETRY_MS 200
#define FLUSH_ASYNC_POLL_MS 20
#define IMAGE_WIDTH 96
#define IMAGE_HEIGHT 96
#define IMAGE_SIZE (IMAGE_WIDTH * IMAGE_HEIGHT * 2)  // RGB565
#define IMAGES_DIR SDCARD_MOUNT_POINT"/img"
#define JPEG_QUALITY 60

// SD card pins (shared with camera on ESP32-S3-EYE)
#define SD_CLK  GPIO_NUM_39
#define SD_CMD  GPIO_NUM_38
#define SD_D0   GPIO_NUM_40

typedef struct {
    char species[MAX_SPECIES_LEN];
    float confidence;
    time_t timestamp;
    bool has_image;
    uint8_t image[IMAGE_SIZE];
} bird_detection_t;

static struct {
    bird_detection_t *detections;
    int count;
    SemaphoreHandle_t mutex;
    sdmmc_card_t *card;
    bool is_mounted;
} s_storage;

static TaskHandle_t s_flush_task;

static void bird_storage_request_flush_async(void)
{
    if (s_flush_task) {
        xTaskNotifyGive(s_flush_task);
    }
}

static void flush_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bird_storage_flush_to_sd();
    }
}

static void restore_buffer_on_flush_failure(const bird_detection_t *detections, int count)
{
    if (!detections || count <= 0 || !s_storage.mutex) {
        return;
    }

    if (xSemaphoreTake(s_storage.mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to recover buffered detections after SD write error");
        return;
    }

    int to_restore = count;
    if (to_restore > MAX_DETECTIONS - s_storage.count) {
        to_restore = MAX_DETECTIONS - s_storage.count;
    }

    if (to_restore > 0) {
        memcpy(&s_storage.detections[s_storage.count], detections, to_restore * sizeof(bird_detection_t));
        s_storage.count += to_restore;
    }

    if (to_restore < count) {
        ESP_LOGW(TAG, "Dropped %d detections while recovering from SD flush failure", count - to_restore);
    }

    xSemaphoreGive(s_storage.mutex);
}

void bird_storage_init(void)
{
    memset(&s_storage, 0, sizeof(s_storage));
    s_storage.mutex = xSemaphoreCreateMutex();
    s_storage.detections = heap_caps_malloc(sizeof(bird_detection_t) * MAX_DETECTIONS,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_storage.detections) {
        s_storage.detections = heap_caps_malloc(sizeof(bird_detection_t) * MAX_DETECTIONS,
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_storage.detections) {
        ESP_LOGE(TAG, "Failed to allocate detection buffer");
    }
    if (s_flush_task == NULL) {
        if (xTaskCreate(flush_task, "bird_flush", 8192, NULL, 6, &s_flush_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create bird flush task");
            s_flush_task = NULL;
        }
    }
    ESP_LOGI(TAG, "Bird detection storage initialized");
}

void bird_storage_add_detection(const char *species, float confidence, time_t timestamp)
{
    bird_storage_add_detection_with_image(species, confidence, timestamp, NULL, 0);
}

void bird_storage_add_detection_with_image(const char *species,
                                           float confidence,
                                           time_t timestamp,
                                           const uint8_t *image,
                                           size_t image_len)
{
    if (!s_storage.mutex) {
        ESP_LOGE(TAG, "Storage not initialized");
        return;
    }
    if (!s_storage.detections) {
        ESP_LOGE(TAG, "Storage buffer not allocated");
        return;
    }
    
    if (xSemaphoreTake(s_storage.mutex, portMAX_DELAY) == pdTRUE) {
        if (s_storage.count >= MAX_DETECTIONS) {
            ESP_LOGW(TAG, "Buffer full (%d detections), scheduling async flush...", s_storage.count);
            xSemaphoreGive(s_storage.mutex);
            bird_storage_request_flush_async();

            // Give the flush task a brief window to free space.
            int waited = 0;
            bool have_lock = false;
            while (waited < FLUSH_ASYNC_RETRY_MS) {
                vTaskDelay(pdMS_TO_TICKS(FLUSH_ASYNC_POLL_MS));
                waited += FLUSH_ASYNC_POLL_MS;
                if (xSemaphoreTake(s_storage.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (s_storage.count < MAX_DETECTIONS) {
                        have_lock = true;
                        break;
                    }
                    xSemaphoreGive(s_storage.mutex);
                }
            }

            if (!have_lock) {
                if (xSemaphoreTake(s_storage.mutex, portMAX_DELAY) != pdTRUE) {
                    return;
                }
            }
        }
        
        if (s_storage.count < MAX_DETECTIONS) {
            bird_detection_t *det = &s_storage.detections[s_storage.count];
            strncpy(det->species, species, MAX_SPECIES_LEN - 1);
            det->species[MAX_SPECIES_LEN - 1] = '\0';
            det->confidence = confidence;
            det->timestamp = timestamp;
            det->has_image = false;
            if (image && image_len >= IMAGE_SIZE) {
                memcpy(det->image, image, IMAGE_SIZE);
                det->has_image = true;
            }
            s_storage.count++;
            
            ESP_LOGI(TAG, "Added detection #%d: %s (%.2f)", 
                     s_storage.count, species, confidence);
        } else {
            ESP_LOGW(TAG, "Buffer still full after async flush attempt, dropping detection");
        }
        
        xSemaphoreGive(s_storage.mutex);
    }
}

int bird_storage_get_count(void)
{
    int count = 0;
    if (s_storage.mutex && xSemaphoreTake(s_storage.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = s_storage.count;
        xSemaphoreGive(s_storage.mutex);
    }
    return count;
}

static esp_err_t sdcard_mount(void)
{
    if (s_storage.is_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }
    
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Mounting SD card...");
    
    // Deinitialize camera before SD access to avoid SDMMC bus conflicts.
#if ESP_CAMERA_SUPPORTED
    esp_camera_deinit();
#endif
    // Deinitialize SDMMC host if it was initialized by camera or something else.
    // This is safe to call even if not initialized.
    sdmmc_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Reset GPIO pins
    gpio_reset_pin(SD_CLK);
    gpio_reset_pin(SD_CMD);
    gpio_reset_pin(SD_D0);
    
    // Configure pull-ups
    gpio_set_pull_mode(SD_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CMD, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_D0, GPIO_PULLUP_ONLY);
    
    gpio_pulldown_dis(SD_CLK);
    gpio_pulldown_dis(SD_CMD);
    gpio_pulldown_dis(SD_D0);
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = SD_CLK;
    slot_config.cmd = SD_CMD;
    slot_config.d0 = SD_D0;
    slot_config.d1 = GPIO_NUM_NC;
    slot_config.d2 = GPIO_NUM_NC;
    slot_config.d3 = GPIO_NUM_NC;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    ret = esp_vfs_fat_sdmmc_mount(
        SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_storage.card);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    s_storage.is_mounted = true;
    ESP_LOGI(TAG, "✓ SD card mounted");
    
#if STORAGE_DEBUG
    // Verify mount by listing directory (slow on some cards)
    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (dir) {
        ESP_LOGI(TAG, "✓ Mount point accessible");
        struct dirent *entry;
        ESP_LOGI(TAG, "Files on SD card:");
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "  - %s", entry->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGE(TAG, "✗ Cannot access mount point (errno: %d - %s)", errno, strerror(errno));
        s_storage.is_mounted = false;
        return ESP_FAIL;
    }
#endif
    
    return ESP_OK;
}

static esp_err_t sdcard_unmount(void)
{
    if (!s_storage.is_mounted || !s_storage.card) {
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Unmounting SD card...");
    
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_storage.card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD unmount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_storage.card = NULL;
    s_storage.is_mounted = false;
    
    // Deinitialize the SDMMC host to fully release it
    sdmmc_host_deinit();

#if ESP_CAMERA_SUPPORTED
    // Re-init camera after SD operations.
    if (app_camera_init() != 0) {
        ESP_LOGW(TAG, "Camera re-init failed after SD unmount");
    }
#endif
    
    ESP_LOGI(TAG, "✓ SD card unmounted");
    
    return ESP_OK;
}

esp_err_t bird_storage_flush_to_sd(void)
{
    if (!s_storage.mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Take mutex to get snapshot of detections
    if (xSemaphoreTake(s_storage.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    int count = s_storage.count;
    
    if (count == 0) {
        xSemaphoreGive(s_storage.mutex);
        ESP_LOGI(TAG, "No detections to flush");
        return ESP_OK;
    }
    
    // Make a copy of detections
    bird_detection_t *detections_copy = malloc(count * sizeof(bird_detection_t));
    if (!detections_copy) {
        xSemaphoreGive(s_storage.mutex);
        ESP_LOGE(TAG, "Failed to allocate memory for detections copy");
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(detections_copy, s_storage.detections, count * sizeof(bird_detection_t));
    
    // Clear the buffer immediately so camera can continue
    s_storage.count = 0;
    
    xSemaphoreGive(s_storage.mutex);
    
    ESP_LOGI(TAG, "Flushing %d detections to SD card...", count);
    
    // Mount SD card
    esp_err_t ret = sdcard_mount();
    if (ret != ESP_OK) {
        restore_buffer_on_flush_failure(detections_copy, count);
        free(detections_copy);
        return ret;
    }
    
    // Give filesystem time to stabilize
    vTaskDelay(pdMS_TO_TICKS(50));
    
#if SAVE_IMAGES_TO_SD
    // Ensure images directory exists
    struct stat st;
    if (stat(IMAGES_DIR, &st) != 0) {
        if (mkdir(IMAGES_DIR, 0755) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "Failed to create images directory %s (errno: %d - %s)",
                     IMAGES_DIR, errno, strerror(errno));
        }
    }
#endif

    // Open file for appending
    ESP_LOGI(TAG, "Opening file: %s", BIRDS_FILE);
    FILE *f = fopen(BIRDS_FILE, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for append (errno: %d - %s)", 
                 BIRDS_FILE, errno, strerror(errno));
        
        // Try creating it
        ESP_LOGW(TAG, "Attempting to create file with 'w' mode...");
        f = fopen(BIRDS_FILE, "w");
        if (!f) {
            ESP_LOGE(TAG, "Failed to create %s (errno: %d - %s)", 
                     BIRDS_FILE, errno, strerror(errno));
            sdcard_unmount();
            restore_buffer_on_flush_failure(detections_copy, count);
            free(detections_copy);
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "✓ File opened successfully, writing data...");
    
    // Write each detection as JSON line
    for (int i = 0; i < count; i++) {
        bird_detection_t *det = &detections_copy[i];
        
        // Convert timestamp to ISO8601
        time_t timestamp_sec = det->timestamp;
        struct tm timeinfo;
        char iso_time[32];
        localtime_r(&timestamp_sec, &timeinfo);
        strftime(iso_time, sizeof(iso_time), "%Y-%m-%dT%H:%M:%S", &timeinfo);
        
#if SAVE_IMAGES_TO_SD
        char image_file[64] = {0};
        if (det->has_image) {
            // FATFS without LFN requires 8.3 filenames. Use 8-hex timestamp.
            snprintf(image_file, sizeof(image_file),
                     IMAGES_DIR"/%08lx.jpg",
                     (unsigned long)det->timestamp);

            uint8_t *jpg_out = NULL;
            size_t jpg_len = 0;
            bool ok = fmt2jpg(det->image, IMAGE_SIZE,
                              IMAGE_WIDTH, IMAGE_HEIGHT,
                              PIXFORMAT_RGB565,
                              JPEG_QUALITY, &jpg_out, &jpg_len);
            if (ok && jpg_out && jpg_len > 0) {
                FILE *img = fopen(image_file, "wb");
                if (img) {
                    fwrite(jpg_out, 1, jpg_len, img);
                    fclose(img);
                } else {
                    ESP_LOGW(TAG, "Failed to open image file %s (errno: %d - %s)",
                             image_file, errno, strerror(errno));
                    image_file[0] = '\0';
                }
                free(jpg_out);
            } else {
                ESP_LOGW(TAG, "JPEG encode failed for %s (ok=%d len=%zu)", image_file, ok, jpg_len);
                image_file[0] = '\0';
                if (jpg_out) {
                    free(jpg_out);
                }
            }
        }

        // Write JSON line
        int written = fprintf(f,
                "{\"species\":\"%s\",\"confidence\":%.2f,\"timestamp\":%lld,\"datetime\":\"%s\"%s%s}\n",
                det->species,
                det->confidence,
                det->timestamp,
                iso_time,
                image_file[0] ? ",\"image\":\"" : "",
                image_file[0] ? image_file : "");
        if (image_file[0]) {
            fputc('"', f);
        }
#else
        int written = fprintf(f,
                "{\"species\":\"%s\",\"confidence\":%.2f,\"timestamp\":%lld,\"datetime\":\"%s\"}\n",
                det->species,
                det->confidence,
                det->timestamp,
                iso_time);
#endif
        
        if (written < 0) {
            ESP_LOGE(TAG, "Write error on detection %d (errno: %d)", i, errno);
        }
    }
    
    // Explicitly flush and close
    fflush(f);
    int close_result = fclose(f);
    if (close_result != 0) {
        ESP_LOGE(TAG, "fclose failed (errno: %d - %s)", errno, strerror(errno));
    }
    
    ESP_LOGI(TAG, "✓ Wrote %d detections to SD card", count);
    
    // Unmount SD card
    sdcard_unmount();
    
    free(detections_copy);
    
    return ESP_OK;
}

size_t bird_storage_get_all_jsonl(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0 || !s_storage.mutex) {
        return 0;
    }
    
    size_t offset = 0;
    
    if (xSemaphoreTake(s_storage.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_storage.count && offset < buffer_size - 200; i++) {
            bird_detection_t *det = &s_storage.detections[i];
            
            // Convert timestamp to ISO8601
            time_t timestamp_sec = det->timestamp;
            struct tm timeinfo;
            char iso_time[32];
            localtime_r(&timestamp_sec, &timeinfo);
            strftime(iso_time, sizeof(iso_time), "%Y-%m-%dT%H:%M:%S", &timeinfo);
            
            // Write JSON object followed by newline (JSONL format)
            offset += snprintf(buffer + offset, buffer_size - offset,
                             "{\"species\":\"%s\",\"confidence\":%.2f,\"timestamp\":%lld,\"datetime\":\"%s\",\"has_image\":%s}\n",
                             det->species,
                             det->confidence,
                             det->timestamp,
                             iso_time,
                             det->has_image ? "true" : "false");
        }
        
        xSemaphoreGive(s_storage.mutex);
    }
    
    return offset;
}

size_t bird_storage_read_from_sd(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return 0;
    }
    
    ESP_LOGI(TAG, "Reading birds from SD card...");
    
    // Mount SD card
    esp_err_t ret = sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD for reading");
        return 0;
    }
    
    // Open file for reading
    FILE *f = fopen(BIRDS_FILE, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for reading (errno: %d - %s)", 
                 BIRDS_FILE, errno, strerror(errno));
        sdcard_unmount();
        return 0;
    }
    
    // Read entire file
    size_t bytes_read = fread(buffer, 1, buffer_size - 1, f);
    buffer[bytes_read] = '\0';  // Null terminate
    
    fclose(f);
    ESP_LOGI(TAG, "✓ Read %zu bytes from SD card", bytes_read);
    
    // Unmount SD card
    sdcard_unmount();
    
    return bytes_read;
}
