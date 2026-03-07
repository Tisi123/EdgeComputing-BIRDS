#include "detection_responder.h"
#include "tensorflow/lite/micro/micro_log.h"
#include <sys/time.h>
extern "C" {
#include "bird_detection_storage.h"
#include "esp_timer.h"
}

// Confidence threshold for storing detections
#define CONFIDENCE_THRESHOLD 0.5f

void RespondToDetection(float cup_score, float laptop_score, float unknown_score) {
    // Determine which class has highest score
    float max_score = unknown_score;
    int max_class = 2;
    const char *species = "unknown";
    
    if (laptop_score > max_score) {
        max_score = laptop_score;
        max_class = 1;
        species = "laptop";  // TODO: Change to actual bird species name
    }
    if (cup_score > max_score) {
        max_score = cup_score;
        max_class = 0;
        species = "cup";  // TODO: Change to actual bird species name
    }

    // Only store if above confidence threshold
    if (max_score > CONFIDENCE_THRESHOLD) {
        time_t timestamp;
        time(&timestamp);
        bird_storage_add_detection(species, max_score, timestamp);

        MicroPrintf("Bird detected: %s (%.2f%%) - Buffered: %d - Timestamp: %lld", 
                   species, max_score * 100.0f, bird_storage_get_count(), timestamp);
    }
}