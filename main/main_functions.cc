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
  constexpr int kTensorArenaSize = 81 * 1024 + scratchBufSize;
  //constexpr int kTensorArenaSize = 150 * 1024 + scratchBufSize;
  static uint8_t *tensor_arena; //[kTensorArenaSize]; // Maybe we should move this to external
} // namespace

void setup()
{
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
    tensor_arena = (uint8_t *)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
  static tflite::MicroMutableOpResolver<16> micro_op_resolver;

  // Include only the necessary operations
  micro_op_resolver.AddConv2D();         // Conv2D layer
  micro_op_resolver.AddFullyConnected(); // Dense layer
  micro_op_resolver.AddMaxPool2D();      // MaxPooling2D layer
  micro_op_resolver.AddSoftmax();        // Softmax activation
  micro_op_resolver.AddQuantize();       // Quantize operation (if using quantized model)
  micro_op_resolver.AddDequantize();     // Dequantize operation (if using quantized model)
  micro_op_resolver.AddDepthwiseConv2D(); // DepthwiseConv2D layer
  micro_op_resolver.AddReshape();         // Reshape layer
  micro_op_resolver.AddAveragePool2D();  // AveragePooling2D layer
  // Add operations for BatchNormalization layers
  micro_op_resolver.AddMul();   // Used in BatchNormalization
  micro_op_resolver.AddAdd();   // Used in BatchNormalization
  micro_op_resolver.AddSub();   // Used in BatchNormalization
  micro_op_resolver.AddDiv();   // Used in BatchNormalization
  micro_op_resolver.AddMean();  // Used in BatchNormalization
  micro_op_resolver.AddRsqrt(); // Used in BatchNormalization

  // Build an interpreter to run the model with.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk)
  {
    MicroPrintf("AllocateTensors() failed");
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
    return;
  }
#endif
}

#ifndef CLI_ONLY_INFERENCE
// The name of this function is important for Arduino compatibility.
void loop()
{
  // Get image from provider.
  if (kTfLiteOk != GetImage(kNumCols, kNumRows, kNumChannels, input->data.int8))
  {
    MicroPrintf("Image capture failed.");
  }

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
  RespondToDetection(bird_score, not_bird_score);

  // Add a recognition cooldown to reduce capture/inference frequency.
  vTaskDelay(pdMS_TO_TICKS(2000));
}
#endif
