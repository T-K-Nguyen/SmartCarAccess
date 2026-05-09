#include "uwb/lstm_inference.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"

// 1. Global memory allocation (40KB is very safe for Conv1D)
constexpr int kTensorArenaSize = 40 * 1024;
alignas(16) static uint8_t tensor_arena[kTensorArenaSize];

LstmInference::LstmInference()
    : frame_count(0), last_filtered_m(-1.0f), model(nullptr), interpreter(nullptr), input(nullptr), output(nullptr) {
  // Initialize sliding window with zeros
  for (int i = 0; i < TIME_STEPS; i++) {
    for (int j = 0; j < NUM_FEATURES; j++) {
      window[i][j] = 0.0f;
    }
  }
}

bool LstmInference::begin() {
  // 2. Add an Error Reporter to print clear errors to Serial instead of crashing
  static tflite::MicroErrorReporter micro_error_reporter;
  tflite::ErrorReporter* error_reporter = &micro_error_reporter;

  // 3. Load model from the C array file
  model = tflite::GetModel(uwb_lstm_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("[AI] TFLite schema version error!");
    return false;
  }

  // 4. GOLDEN KEY: Move AllOpsResolver inside the begin() function
  // Fully avoid the "Static Initialization Fiasco" that can crash ESP32 at boot
  static tflite::AllOpsResolver resolver;

  // 5. Initialize the interpreter statically (Do not use 'new' to prevent heap overflow)
  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
  interpreter = &static_interpreter;

  // 6. Allocate memory
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    Serial.println("[AI] Tensor Arena allocation error! Increase kTensorArenaSize.");
    return false;
  }

  input = interpreter->input(0);
  output = interpreter->output(0);
  Serial.println("[AI] AI model initialized successfully.");
  return true;
}

float LstmInference::normalize(float value, int feature_index) {
  return (value - scaler_mean[feature_index]) / scaler_scale[feature_index];
}

void LstmInference::shiftWindow() {
  for (int i = 0; i < TIME_STEPS - 1; i++) {
    for (int j = 0; j < NUM_FEATURES; j++) {
      window[i][j] = window[i + 1][j];
    }
  }
}

bool LstmInference::predict(float current_filtered_m, float current_residual_m,
                            float &p_walk, float &p_loiter, float &p_attack) {
  // 1. Compute velocity
  float velocity = 0.0f;
  if (last_filtered_m >= 0.0f) {
    velocity = current_filtered_m - last_filtered_m;
  }
  last_filtered_m = current_filtered_m;

  // 2. Shift the window and add new data
  if (frame_count == TIME_STEPS) {
    shiftWindow();
  } else {
    frame_count++;
  }

  int current_idx = (frame_count == TIME_STEPS) ? (TIME_STEPS - 1) : (frame_count - 1);
  window[current_idx][0] = normalize(current_filtered_m, 0);   
  window[current_idx][1] = normalize(current_residual_m, 1);   
  window[current_idx][2] = normalize(velocity, 2);             

  // 3. Wait for enough frames (warm-up phase)
  if (frame_count < TIME_STEPS) {
    return false;
  }

  // 4. Load the 2D sliding window into the 1D tensor array
  int tensor_idx = 0;
  for (int i = 0; i < TIME_STEPS; i++) {
    for (int j = 0; j < NUM_FEATURES; j++) {
      input->data.f[tensor_idx++] = window[i][j];
    }
  }

  // 5. Run inference
  if (interpreter->Invoke() != kTfLiteOk) {
    Serial.println("[AI] Error while running Invoke()!");
    return false;
  }

  // 6. Update results
  p_walk = output->data.f[0];
  p_loiter = output->data.f[1];
  p_attack = output->data.f[2];

  return true;
}