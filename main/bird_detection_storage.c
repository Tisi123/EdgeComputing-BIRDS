#include "bird_detection_storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "img_converters.h"
#include "esp_camera.h"
#include "app_camera_esp.h"
#include "esp_main.h"
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

#if ESP_CAMERA_SUPPORTED
// Fallback declaration in case a different app_camera_esp.h is picked up first.
void app_camera_mark_deinit(void);
#endif

#define MAX_DETECTIONS 10
#define MAX_SPECIES_LEN 32
#define SDCARD_MOUNT_POINT "/sd"
#define BIRDS_FILE SDCARD_MOUNT_POINT"/birds.txt"
#define STORAGE_DEBUG 1
#define SAVE_IMAGES_TO_SD 1
// Set to 1 to save grayscale JPEGs instead of RGB565 color.
#define SAVE_IMAGES_GRAYSCALE 0
#define FLUSH_WRITE_DELAY_MS 50
#define MAX_FLUSH_BATCH 10
#define FLUSH_DEBUG 1
#define IMAGE_WIDTH 96
#define IMAGE_HEIGHT 96
#define IMAGE_SIZE (IMAGE_WIDTH * IMAGE_HEIGHT * 2)  // RGB565
#define IMAGES_DIR SDCARD_MOUNT_POINT"/img"
#define JPEG_QUALITY 60
// Reduce SDMMC clock to improve stability on shared camera/SD pins.
#define SDMMC_MAX_FREQ_KHZ 10000
// Cooldown after SD I/O error to avoid hammering the driver.
#define SD_ERROR_COOLDOWN_MS 10000

// SD card pins (shared with camera on ESP32-S3-EYE)
#define SD_CLK  GPIO_NUM_39
#define SD_CMD  GPIO_NUM_38
#define SD_D0   GPIO_NUM_40

// Use SDMMC (built-in SD socket).
#define USE_SDSPI 0

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
static volatile bool s_flush_in_progress;
static bool s_keep_mounted = false;
static volatile bool s_flush_had_io_error = false;
static int64_t s_sd_cooldown_until_ms = 0;

static esp_err_t sdcard_mount(void);
static esp_err_t sdcard_unmount(void);

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

static void restore_buffer_entries(const bird_detection_t *detections, int count, const char *reason)
{
    if (!detections || count <= 0 || !s_storage.mutex) {
        return;
    }

    if (xSemaphoreTake(s_storage.mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to restore buffered detections (%s)", reason ? reason : "unknown");
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
        ESP_LOGW(TAG, "Dropped %d detections while restoring (%s)", count - to_restore,
                 reason ? reason : "unknown");
    }

    xSemaphoreGive(s_storage.mutex);
}

void bird_storage_init(void)
{
    memset(&s_storage, 0, sizeof(s_storage));
    s_flush_in_progress = false;
    s_sd_cooldown_until_ms = 0;
    // Default to unmounting after each flush. The ESP32-S3-EYE shares SDMMC pins with the camera,
    // so keeping the card mounted can leave the bus in a bad state after camera re-init.
    s_keep_mounted = false;
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

    // Only pre-mount if we intend to keep the card mounted.
    if (s_keep_mounted) {
        esp_err_t mount_ret = sdcard_mount();
        if (mount_ret != ESP_OK) {
            ESP_LOGW(TAG, "Initial SD mount failed: %s", esp_err_to_name(mount_ret));
        }
    }
}

void bird_storage_set_keep_mounted(bool keep)
{
    s_keep_mounted = keep;
}

bool bird_storage_is_flushing(void)
{
    return s_flush_in_progress;
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
            ESP_LOGW(TAG, "Buffer full (%d detections), dropping detection", s_storage.count);
            xSemaphoreGive(s_storage.mutex);
            bird_storage_request_flush_async();
            return;
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
        if (s_keep_mounted) {
            ESP_LOGW(TAG, "SD card already mounted");
            return ESP_OK;
        }
        // We don't want it mounted across operations; force a clean remount.
        ESP_LOGW(TAG, "SD card already mounted (keep_mounted=0), remounting");
        sdcard_unmount();
    }
    
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Mounting SD card...");
#if FLUSH_DEBUG
    ESP_LOGI(TAG, "SD pins: CLK=%d CMD=%d D0=%d", SD_CLK, SD_CMD, SD_D0);
#endif
    
    // Deinitialize camera before SD access to avoid SDMMC bus conflicts.
    // Deinitialize SDMMC host if it was initialized by camera or something else.
    // This is safe to call even if not initialized.
    sdmmc_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    #if !USE_SDSPI
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
    #endif
    
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    #if USE_SDSPI
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDSPI_HOST_ID;
    host.max_freq_khz = SDMMC_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = host.slot;
    slot_config.gpio_cs = SD_SPI_CS;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_SPI_MOSI,
        .miso_io_num = SD_SPI_MISO,
        .sclk_io_num = SD_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_spi_bus_inited = true;

    ret = esp_vfs_fat_sdspi_mount(
        SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_storage.card);
    #else
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_MAX_FREQ_KHZ;
    // Increase command timeout to tolerate slower card responses.
    host.command_timeout_ms = 4000;

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
    #endif
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    s_storage.is_mounted = true;
    ESP_LOGI(TAG, "✓ SD card mounted");
#if FLUSH_DEBUG
    if (s_storage.card) {
        ESP_LOGI(TAG, "SD card: name=%s type=%s max_freq=%d kHz",
                 s_storage.card->cid.name,
                 (s_storage.card->is_sdio ? "SDIO" :
                  (s_storage.card->is_mmc ? "MMC" : "SD")),
                 s_storage.card->max_freq_khz);
    }
#endif
    
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
    
    if (s_keep_mounted) {
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
    
    #if USE_SDSPI
    if (s_spi_bus_inited) {
        spi_bus_free(SDSPI_HOST_ID);
        s_spi_bus_inited = false;
    }
    #else
    // Deinitialize the SDMMC host to fully release it
    sdmmc_host_deinit();
    #endif

    ESP_LOGI(TAG, "✓ SD card unmounted");
    
    return ESP_OK;
}

void bird_storage_force_unmount(void)
{
    s_keep_mounted = false;
    sdcard_unmount();
    s_keep_mounted = true;
}

esp_err_t bird_storage_flush_to_sd(void)
{
    if (!s_storage.mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_flush_in_progress) {
        ESP_LOGW(TAG, "Flush already in progress, skipping");
        return ESP_ERR_INVALID_STATE;
    }
    s_flush_in_progress = true;

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms < s_sd_cooldown_until_ms) {
        if (xSemaphoreTake(s_storage.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_storage.count = 0;  // drop buffered detections to keep running
            xSemaphoreGive(s_storage.mutex);
        }
        ESP_LOGW(TAG, "SD in cooldown, skipping flush");
        s_flush_in_progress = false;
        return ESP_FAIL;
    }
    
    // Take mutex to get snapshot of detections
    if (xSemaphoreTake(s_storage.mutex, portMAX_DELAY) != pdTRUE) {
        s_flush_in_progress = false;
        return ESP_ERR_TIMEOUT;
    }
    
    int count = s_storage.count;
    
    if (count == 0) {
        xSemaphoreGive(s_storage.mutex);
        ESP_LOGI(TAG, "No detections to flush");
        s_flush_in_progress = false;
        return ESP_OK;
    }
    
    // Make a copy of detections
    bird_detection_t *detections_copy = malloc(count * sizeof(bird_detection_t));
    if (!detections_copy) {
        xSemaphoreGive(s_storage.mutex);
        ESP_LOGE(TAG, "Failed to allocate memory for detections copy");
        s_flush_in_progress = false;
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(detections_copy, s_storage.detections, count * sizeof(bird_detection_t));
    
    // Clear the buffer immediately so camera can continue
    s_storage.count = 0;
    
    xSemaphoreGive(s_storage.mutex);
    
    int write_count = count;
    int remaining_count = 0;
    if (count > MAX_FLUSH_BATCH) {
        write_count = MAX_FLUSH_BATCH;
        remaining_count = count - write_count;
    }

    ESP_LOGI(TAG, "Flushing %d detections to SD card (batch of %d)...", count, write_count);
#if FLUSH_DEBUG
    ESP_LOGI(TAG, "Flush: count=%d write_count=%d remaining=%d", count, write_count, remaining_count);
#endif

    set_detection_paused(true);

    if (!app_camera_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "Camera busy, cannot flush to SD right now");
        set_detection_paused(false);
        free(detections_copy);
        s_flush_in_progress = false;
        return ESP_ERR_TIMEOUT;
    }

#if ESP_CAMERA_SUPPORTED
    if (app_camera_is_initialized()) {
        esp_camera_deinit();
        app_camera_mark_deinit();
    }
#endif
    // Give the SDMMC bus a moment to settle after camera deinit.
    vTaskDelay(pdMS_TO_TICKS(150));

    // Mount SD card
    esp_err_t ret = sdcard_mount();
    if (ret != ESP_OK) {
        s_sd_cooldown_until_ms = esp_timer_get_time() / 1000 + SD_ERROR_COOLDOWN_MS;
        app_camera_unlock();
        set_detection_paused(false);
        free(detections_copy);
        s_flush_in_progress = false;
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
            s_sd_cooldown_until_ms = esp_timer_get_time() / 1000 + SD_ERROR_COOLDOWN_MS;
            sdcard_unmount();
            app_camera_unlock();
            free(detections_copy);
            s_flush_in_progress = false;
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "✓ File opened successfully, writing data...");
    
    s_flush_had_io_error = false;

    // Write each detection as JSON line
    for (int i = 0; i < write_count; i++) {
        if (s_flush_had_io_error) {
            ESP_LOGW(TAG, "Stopping SD writes early due to I/O error");
            break;
        }
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
            bool ok = false;
#if SAVE_IMAGES_GRAYSCALE
            uint8_t *gray = malloc(IMAGE_WIDTH * IMAGE_HEIGHT);
            if (gray) {
                for (int i = 0; i < IMAGE_WIDTH * IMAGE_HEIGHT; ++i) {
                    uint16_t pixel = ((uint16_t *)det->image)[i];
                    uint8_t hb = pixel & 0xFF;
                    uint8_t lb = pixel >> 8;
                    uint8_t r = (lb & 0x1F) << 3;
                    uint8_t g = ((hb & 0x07) << 5) | ((lb & 0xE0) >> 3);
                    uint8_t b = (hb & 0xF8);
                    gray[i] = (uint8_t)((305 * r + 600 * g + 119 * b) >> 10);
                }
                ok = fmt2jpg(gray, IMAGE_WIDTH * IMAGE_HEIGHT,
                             IMAGE_WIDTH, IMAGE_HEIGHT,
                             PIXFORMAT_GRAYSCALE,
                             JPEG_QUALITY, &jpg_out, &jpg_len);
                free(gray);
            }
#else
            ok = fmt2jpg(det->image, IMAGE_SIZE,
                         IMAGE_WIDTH, IMAGE_HEIGHT,
                         PIXFORMAT_RGB565,
                         JPEG_QUALITY, &jpg_out, &jpg_len);
#endif
        if (ok && jpg_out && jpg_len > 0) {
            FILE *img = fopen(image_file, "wb");
            if (img) {
                fwrite(jpg_out, 1, jpg_len, img);
                fclose(img);
            } else {
                ESP_LOGW(TAG, "Failed to open image file %s (errno: %d - %s)",
                         image_file, errno, strerror(errno));
                image_file[0] = '\0';
                s_flush_had_io_error = true;
                s_sd_cooldown_until_ms = esp_timer_get_time() / 1000 + SD_ERROR_COOLDOWN_MS;
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
            s_flush_had_io_error = true;
            s_sd_cooldown_until_ms = esp_timer_get_time() / 1000 + SD_ERROR_COOLDOWN_MS;
        }
#if FLUSH_DEBUG
        else {
            ESP_LOGI(TAG, "Wrote detection %d/%d", i + 1, write_count);
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(FLUSH_WRITE_DELAY_MS));
    }
    
    // Explicitly flush and close
#if FLUSH_DEBUG
    ESP_LOGI(TAG, "Flushing file buffers...");
#endif
    fflush(f);
    int close_result = fclose(f);
    if (close_result != 0) {
        ESP_LOGE(TAG, "fclose failed (errno: %d - %s)", errno, strerror(errno));
        s_flush_had_io_error = true;
        s_sd_cooldown_until_ms = esp_timer_get_time() / 1000 + SD_ERROR_COOLDOWN_MS;
    }
    
    ESP_LOGI(TAG, "✓ Wrote %d detections to SD card", write_count);

    // Release the SD bus before resuming the camera (shared pins on ESP32-S3-EYE).
    esp_err_t unmount_ret = sdcard_unmount();
    if (unmount_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD unmount after flush failed: %s", esp_err_to_name(unmount_ret));
        s_flush_had_io_error = true;
    }
    
    if (s_flush_had_io_error) {
        ESP_LOGW(TAG, "I/O error during flush; skipping SD remount to keep detection running");
    }
    // Try to bring the camera back immediately so detection can resume smoothly.
    if (!app_camera_is_initialized()) {
        int cam_ret = app_camera_init();
        if (cam_ret != 0) {
            ESP_LOGE(TAG, "Camera re-init after flush failed");
        } else {
            ESP_LOGI(TAG, "Camera re-initialized after flush");
        }
    }
    app_camera_unlock();
    set_detection_paused(false);

    if (!s_flush_had_io_error && remaining_count > 0) {
        restore_buffer_entries(&detections_copy[write_count], remaining_count, "remaining batch");
    }

    free(detections_copy);
    s_flush_in_progress = false;

    return s_flush_had_io_error ? ESP_FAIL : ESP_OK;
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

    if (!app_camera_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "Camera busy, cannot read SD right now");
        return 0;
    }
    
    // Mount SD card
    esp_err_t ret = sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD for reading");
        app_camera_unlock();
        return 0;
    }
    
    // Open file for reading
    FILE *f = fopen(BIRDS_FILE, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for reading (errno: %d - %s)", 
                 BIRDS_FILE, errno, strerror(errno));
        sdcard_unmount();
        app_camera_unlock();
        return 0;
    }
    
    // Read entire file
    size_t bytes_read = fread(buffer, 1, buffer_size - 1, f);
    buffer[bytes_read] = '\0';  // Null terminate
    
    fclose(f);
    ESP_LOGI(TAG, "✓ Read %zu bytes from SD card", bytes_read);
    
    // Keep SD mounted during active windows.
    sdcard_unmount();
    app_camera_unlock();
    
    return bytes_read;
}
