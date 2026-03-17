/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "main_functions.h"

#include "detection_responder.h"
#include "image_provider.h"
#include "model_settings.h"
#include "bird_detector_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <new>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include "esp_main.h"

namespace {
float GetScoreFloat(const TfLiteTensor* tensor, int index) {
  if (tensor == nullptr) {
    return 0.0f;
  }

  switch (tensor->type) {
    case kTfLiteFloat32:
      return tensor->data.f[index];
    case kTfLiteInt8: {
      const int32_t q = tensor->data.int8[index];
      return (q - tensor->params.zero_point) * tensor->params.scale;
    }
    case kTfLiteUInt8: {
      const int32_t q = tensor->data.uint8[index];
      return (q - tensor->params.zero_point) * tensor->params.scale;
    }
    default:
      return 0.0f;
  }
}
}  // namespace

// Globals, used for compatibility with Arduino-style sketches.
namespace
{
  const tflite::Model *model = nullptr;
  tflite::MicroInterpreter *interpreter = nullptr;
  TfLiteTensor *input = nullptr;
  bool g_inference_ready = false;
  bool g_ops_registered = false;
  uint16_t *g_last_image_rgb565 = nullptr;

  // In order to use optimized tensorflow lite kernels, a signed int8_t quantized
  // model is preferred over the legacy unsigned model format. This means that
  // throughout this project, input images must be converted from unisgned to
  // signed format. The easiest and quickest way to convert from unsigned to
  // signed 8-bit integers is to subtract 128 from the unsigned value to get a
  // signed value.

#ifdef CONFIG_IDF_TARGET_ESP32S3
  constexpr int scratchBufSize = 40 * 1024;
#else
  constexpr int scratchBufSize = 0;
#endif
  // An area of memory to use for input, output, and intermediate arrays.
  // Model currently requires significantly more than 100 KB at allocation time.
  constexpr int kTensorArenaSize = 520 * 1024 + scratchBufSize;
  static uint8_t *tensor_arena; //[kTensorArenaSize]; // Maybe we should move this to external
  alignas(tflite::MicroInterpreter) static uint8_t interpreter_buffer[sizeof(tflite::MicroInterpreter)];
} // namespace

void setup()
{
  g_inference_ready = false;

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  model = tflite::GetModel(bird_detector_model);
  if (model->version() != TFLITE_SCHEMA_VERSION)
  {
    MicroPrintf("Model provided is schema version %d not equal to supported "
                "version %d.",
                model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  if (tensor_arena == NULL)
  {
    // Prefer PSRAM for arena size; fallback to internal RAM.
    tensor_arena = (uint8_t *)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (tensor_arena == NULL) {
      tensor_arena = (uint8_t *)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
  }
  if (tensor_arena == NULL)
  {
    printf("Couldn't allocate memory of %d bytes\n", kTensorArenaSize);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  // tflite::AllOpsResolver resolver;
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroMutableOpResolver<17> micro_op_resolver;

  if (!g_ops_registered) {
    // Include only the necessary operations.
    micro_op_resolver.AddConv2D();          // Conv2D layer
    micro_op_resolver.AddFullyConnected();  // Dense layer
    micro_op_resolver.AddMaxPool2D();       // MaxPooling2D layer
    micro_op_resolver.AddSoftmax();         // Softmax activation
    micro_op_resolver.AddLogistic();        // Sigmoid activation
    micro_op_resolver.AddQuantize();        // Quantize operation
    micro_op_resolver.AddDequantize();      // Dequantize operation
    micro_op_resolver.AddDepthwiseConv2D(); // DepthwiseConv2D layer
    micro_op_resolver.AddReshape();         // Reshape layer
    micro_op_resolver.AddAveragePool2D();   // AveragePooling2D layer
    // Ops used by folded/non-folded normalization arithmetic.
    micro_op_resolver.AddMul();
    micro_op_resolver.AddAdd();
    micro_op_resolver.AddSub();
    micro_op_resolver.AddDiv();
    micro_op_resolver.AddMean();
    micro_op_resolver.AddRsqrt();
    g_ops_registered = true;
  }

  // Build (or rebuild) the interpreter each setup attempt.
  interpreter = new (interpreter_buffer) tflite::MicroInterpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize);

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk)
  {
    MicroPrintf("AllocateTensors() failed");
    input = nullptr;
    return;
  }

  // Get information about the memory area to use for the model's input.
  input = interpreter->input(0);

#ifndef CLI_ONLY_INFERENCE
  // Initialize Camera
  TfLiteStatus init_status = InitCamera();
  if (init_status != kTfLiteOk)
  {
    MicroPrintf("InitCamera failed\n");
    input = nullptr;
    return;
  }

#if DISPLAY_SUPPORT
  create_gui();
#endif
#endif

  g_inference_ready = true;
}

bool inference_ready()
{
  return g_inference_ready && (interpreter != nullptr) && (input != nullptr);
}

#ifndef CLI_ONLY_INFERENCE
// The name of this function is important for Arduino compatibility.
void loop()
{
  if (!inference_ready()) {
    MicroPrintf("Inference setup not ready; retrying setup.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  // Get image from provider.
  if (kTfLiteOk != GetImage(kNumCols, kNumRows, kNumChannels, input->data.int8))
  {
    MicroPrintf("Image capture failed.");
  }
  // Cache the latest 96x96 RGB565 image for storage (if available).
  g_last_image_rgb565 = static_cast<uint16_t *>(image_provider_get_rgb565_buf());

  // Run the model on this input and make sure it succeeds.
  if (kTfLiteOk != interpreter->Invoke())
  {
    MicroPrintf("Invoke failed.");
  }

  TfLiteTensor *output = interpreter->output(0);

  // Process inference for the two labels: bird and not_bird.
  const float bird_score = GetScoreFloat(output, kBirdIndex);
  const float not_bird_score = GetScoreFloat(output, kNotBirdIndex);

  // Respond to detection.
  const uint8_t *image_bytes = reinterpret_cast<const uint8_t *>(g_last_image_rgb565);
  const size_t image_len = g_last_image_rgb565 ? (kNumCols * kNumRows * sizeof(uint16_t)) : 0;
  RespondToDetection(bird_score, not_bird_score, image_bytes, image_len);

  // Yield briefly to avoid starving other tasks.
  vTaskDelay(1);
}
#endif
