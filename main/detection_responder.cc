#include "detection_responder.h"
#include "tensorflow/lite/micro/micro_log.h"
#include <sys/time.h>
extern "C" {
#include "bird_detection_storage.h"
#include "esp_timer.h"
}

// Confidence threshold for storing detections
#define CONFIDENCE_THRESHOLD 0.5f

void RespondToDetection(float bird_score, float not_bird_score) {
    // Only store when the model predicts "bird" with at least 50% confidence.
    if (bird_score >= CONFIDENCE_THRESHOLD) {
        time_t timestamp;
        time(&timestamp);
        bird_storage_add_detection("bird", bird_score, timestamp);

        MicroPrintf("Bird detected: bird (%.2f%%), not_bird=%.2f%% - Buffered: %d - Timestamp: %lld",
                    bird_score * 100.0f, not_bird_score * 100.0f,
                    bird_storage_get_count(), timestamp);
    }
}
