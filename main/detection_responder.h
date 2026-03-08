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

// Provides an interface to take an action based on the output from the OBJECT
// detection model.

#ifndef TENSORFLOW_LITE_MICRO_EXAMPLES_OBJECT_DETECTION_DETECTION_RESPONDER_H_
#define TENSORFLOW_LITE_MICRO_EXAMPLES_OBJECT_DETECTION_DETECTION_RESPONDER_H_

#include "tensorflow/lite/c/common.h"

// Called every time the results of a detection run are available.
// `bird_score` is the model confidence for label "bird",
// `not_bird_score` is the model confidence for label "not_bird".
void RespondToDetection(float bird_score, float not_bird_score);

// Initializes GUI components when display support is enabled.
void create_gui();

#endif  // TENSORFLOW_LITE_MICRO_EXAMPLES_OBJECT_DETECTION_DETECTION_RESPONDER_H_
