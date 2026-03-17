#ifndef BIRD_DETECTION_STORAGE_H
#define BIRD_DETECTION_STORAGE_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the bird detection storage system
 */
void bird_storage_init(void);

/**
 * Add a bird detection to the in-memory buffer
 * @param species Bird species name
 * @param confidence Detection confidence (0.0 - 1.0)
 * @param timestamp Unix timestamp
 */
void bird_storage_add_detection(const char *species, float confidence, int64_t timestamp);

/**
 * Add a bird detection with an attached 96x96 grayscale image.
 * @param species Bird species name
 * @param confidence Detection confidence (0.0 - 1.0)
 * @param timestamp Unix timestamp
 * @param image Grayscale image bytes (96x96)
 * @param image_len Size of image buffer in bytes
 */
void bird_storage_add_detection_with_image(const char *species,
                                           float confidence,
                                           int64_t timestamp,
                                           const uint8_t *image,
                                           size_t image_len);

/**
 * Get the number of detections currently in buffer
 */
int bird_storage_get_count(void);

/**
 * Flush all detections to SD card
 * @return ESP_OK on success
 */
esp_err_t bird_storage_flush_to_sd(void);

/**
 * Get all detections as JSON array (for HTTP server)
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written
 */
size_t bird_storage_get_all_jsonl(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // BIRD_DETECTION_STORAGE_H

/**
 * Read all bird detections from SD card file
 * @param buffer Output buffer for JSONL data
 * @param buffer_size Size of buffer
 * @return Number of bytes read, or 0 on error
 */
size_t bird_storage_read_from_sd(char *buffer, size_t buffer_size);
