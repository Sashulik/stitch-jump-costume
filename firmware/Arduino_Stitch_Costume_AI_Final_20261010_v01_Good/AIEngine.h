#pragma once
#include <Wire.h>
#include <AP8G1s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <atomic>

static_assert(EI_CLASSIFIER_RAW_SAMPLE_COUNT == 100, "Expected 1-second model");
static_assert(EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME == 6, "Expected ax/ay/az/gx/gy/gz");
static_assert(EI_CLASSIFIER_FREQUENCY == 100, "Expected 100 Hz model");

constexpr size_t WINDOW_SAMPLES = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
constexpr uint32_t STRIDE_SAMPLES = 20; // 200 ms at 100 Hz.
constexpr float CONFIDENCE = 0.50f; // Prediction and animation trigger threshold.
float ringBuffer[WINDOW_SAMPLES][6];
float inferenceBuffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
SemaphoreHandle_t bufferMutex;
size_t nextSlot = 0, filled = 0;
uint32_t lastSampleMs = 0;
uint32_t totalSamples = 0, timingResets = 0, clippedSamples = 0, gyroClippedSamples = 0;
uint8_t mpuAddress = 0;
bool sensorFailed = true;
std::atomic<bool> aiInferenceEnabled{true};
bool sensorConfigured = false;
uint32_t lastSensorAttemptMs = 0;

struct PredictionMessage {
  float jump, notJump;
  uint32_t resets, clipped, gyroClipped;
  uint32_t windowEndMs;
  int dspMs, modelMs;
  bool valid;
};
QueueHandle_t predictionQueue;
TaskHandle_t samplerTaskHandle = nullptr;
TaskHandle_t inferenceTaskHandle = nullptr;
constexpr uint32_t MEMORY_REPORT_MS = 60000;

void inferenceTask(void *);
void runInference();
#include "RuntimeSupport.h"

bool readRegisters(uint8_t reg, uint8_t *data, uint8_t count) {
  LedSensorGuard guard(dbgIOWaitMaxUs);
  const uint32_t started = micros();
  DebugIO e = debugBeginIO(0, mpuAddress, reg, count);
  Wire.beginTransmission(mpuAddress);
  size_t written = Wire.write(reg);
  uint8_t status = Wire.endTransmission(written != 1);
  if (written != 1 || status != 0) {
    dbgReadFailed.fetch_add(1);
    debugFailure(e, started, written != 1 ? 1 : 2, status, 0);
    return false;
  }
  size_t received = Wire.requestFrom(mpuAddress, count);
  if (received != count) {
    dbgReadFailed.fetch_add(1);
    debugFailure(e, started, 3, status, static_cast<uint8_t>(received));
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    int value = Wire.read();
    if (value < 0) {
      dbgReadFailed.fetch_add(1); debugFailure(e, started, 4, status, i); return false;
    }
    data[i] = value;
  }
  if (reg == 0x3B && count == 14) {
    dbgReadOK.fetch_add(1); dbgLastGoodMs.store(millis());
    debugMaximum(dbgReadMaxUs, micros() - started);
  }
  return true;
}

bool writeRegister(uint8_t reg, uint8_t value) {
  LedSensorGuard guard(dbgIOWaitMaxUs);
  uint32_t started = micros(); DebugIO e = debugBeginIO(1, mpuAddress, reg, 2);
  Wire.beginTransmission(mpuAddress);
  size_t written = Wire.write(reg);
  written += Wire.write(value);
  uint8_t status = Wire.endTransmission();
  if (written != 2 || status != 0) {
    dbgWriteFailed.fetch_add(1);
    debugFailure(e, started, written != 2 ? 1 : 2, status, static_cast<uint8_t>(written));
    return false;
  }
  return true;
}

int16_t signedWord(uint8_t high, uint8_t low) {
  return static_cast<int16_t>((static_cast<uint16_t>(high) << 8) | low);
}

bool configureSensor() {
  dbgConfigureAttempts.fetch_add(1);
  uint8_t identity = 0;
  for (uint8_t address = 0x68; address <= 0x69; ++address) {
    mpuAddress = address;
    if (readRegisters(0x75, &identity, 1)) {
      if (identity == 0x68 || identity == 0x70) break;
      DebugIO e = debugBeginIO(2, mpuAddress, 0x75, 0x68);
      debugFailure(e, micros(), 5, 0, identity);
    }
    mpuAddress = 0;
  }
  if (!mpuAddress) return false;
  if (!writeRegister(0x6B, 0x01)) return false;
  delay(100);
  if (!writeRegister(0x6C, 0) || !writeRegister(0x1A, 3) ||
      !writeRegister(0x1B, 0x18) || !writeRegister(0x1C, 0x10) ||
      !writeRegister(0x19, 9)) return false;
  if (identity == 0x70 && !writeRegister(0x1D, 3)) return false;
  delay(100);
  uint8_t ac = 0, gc = 0;
  if (!readRegisters(0x1C, &ac, 1) || !readRegisters(0x1B, &gc, 1)) return false;
  if ((ac & 0x18) != 0x10 || (gc & 0x18) != 0x18) {
    const bool badAccel = (ac & 0x18) != 0x10;
    DebugIO e = debugBeginIO(3, mpuAddress, badAccel ? 0x1C : 0x1B, badAccel ? 0x10 : 0x18);
    debugFailure(e, micros(), 5, 0, badAccel ? ac : gc);
    return false;
  }
  dbgConfigureOK.fetch_add(1);
  Serial.println("SENSOR: configured 8g/2000dps/100Hz; collecting fresh window");
  return true;
}
void sampleTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  uint32_t previousUs = 0;
  uint8_t consecutiveFailures = 0;
  while (true) {
    if (!sensorConfigured) {
      if (!lastSensorAttemptMs || millis() - lastSensorAttemptMs >= 1000) {
        lastSensorAttemptMs = millis();
        sensorConfigured = configureSensor();
      }
      previousUs = 0; wake = xTaskGetTickCount();
      if (!sensorConfigured) { delay(10); continue; }
      consecutiveFailures = 0;
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    uint32_t nowUs = micros();
    uint32_t elapsedUs = nowUs - previousUs;
    bool timingBad = previousUs && (elapsedUs < 5000 || elapsedUs > 15000);
    if (previousUs) debugMaximum(dbgSampleIntervalMaxUs, elapsedUs);
    if (timingBad) dbgTimingGaps.fetch_add(1);
    previousUs = nowUs;
    uint8_t bytes[14];
    bool ok = readRegisters(0x3B, bytes, sizeof(bytes));
    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    if (!ok) {
      const bool firstFailure = !sensorFailed;
      sensorFailed = true;
      ++consecutiveFailures;
      sensorConfigured = consecutiveFailures < SENSOR_FAILURE_LIMIT;
      filled = 0; nextSlot = 0;
      ++timingResets; // Invalidates predictions from before this interruption.
      xSemaphoreGive(bufferMutex);
      Serial.printf("SENSOR_RETRY,t_ms=%lu,consecutive_failures=%u,action=%s\n",
        static_cast<unsigned long>(millis()), static_cast<unsigned>(consecutiveFailures),
        sensorConfigured ? "retry_read_next_sample" : "reconfigure_sensor");
      if (firstFailure)
        Serial.println("SENSOR: read interrupted; lights continue; retrying read and rebuilding a full window");
      previousUs = 0;
      wake = xTaskGetTickCount(); // Retry next cycle, without catch-up samples.
      continue;
    }
    consecutiveFailures = 0;
    if (timingBad) {
      filled = 0; ++timingResets;
      wake = xTaskGetTickCount(); // Do not invent catch-up samples.
    }
    bool clipped = false;
    for (size_t axis = 0; axis < 3; ++axis) {
      int16_t raw = signedWord(bytes[axis * 2], bytes[axis * 2 + 1]);
      ringBuffer[nextSlot][axis] = raw / 4096.0f; // g, same as training.
      if (raw >= 32700 || raw <= -32700) clipped = true;
    }
    if (clipped) ++clippedSamples;
    bool gyroClipped = false;
    for (size_t axis = 0; axis < 3; ++axis) {
      // Bytes 6/7 are temperature; gyro starts at byte 8.
      int16_t raw = signedWord(bytes[8 + axis * 2], bytes[9 + axis * 2]);
      ringBuffer[nextSlot][axis + 3] = raw / 16.4f; // degrees/second, training units.
      if (raw >= 32700 || raw <= -32700) gyroClipped = true;
    }
    if (gyroClipped) ++gyroClippedSamples;
    nextSlot = (nextSlot + 1) % WINDOW_SAMPLES;
    if (filled < WINDOW_SAMPLES) ++filled;
    ++totalSamples;
    lastSampleMs = millis();
    const bool recovered = sensorFailed && filled == WINDOW_SAMPLES;
    if (recovered) sensorFailed = false;
    xSemaphoreGive(bufferMutex);
    if (recovered)
      Serial.println("SENSOR: recovered; full 1-second window ready; AI resumed");
  }
}
int getSignalData(size_t offset, size_t length, float *out) {
  if (offset > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE ||
      length > EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE - offset) return -1;
  memcpy(out, inferenceBuffer + offset, length * sizeof(float));
  return 0;
}
void runInference() {
  if (!aiInferenceEnabled.load()) { delay(10); return; }
  static bool predicted = false;
  static uint32_t previousPredictionSamples = 0;
  xSemaphoreTake(bufferMutex, portMAX_DELAY);
  if (sensorFailed) {
    xSemaphoreGive(bufferMutex);
    delay(10); return; // Keep lights running while a fresh window is collected.
  }
  if (filled < WINDOW_SAMPLES ||
      (predicted && totalSamples - previousPredictionSamples < STRIDE_SAMPLES)) {
    xSemaphoreGive(bufferMutex); delay(1); return;
  }
  // nextSlot is the oldest sample once the circular buffer is full.
  for (size_t i = 0; i < WINDOW_SAMPLES; ++i)
    memcpy(inferenceBuffer + i * 6, ringBuffer[(nextSlot + i) % WINDOW_SAMPLES],
           6 * sizeof(float));
  const uint32_t windowEndMs = lastSampleMs;
  previousPredictionSamples = totalSamples;
  uint32_t resets = timingResets, clipped = clippedSamples, gyroClipped = gyroClippedSamples;
  xSemaphoreGive(bufferMutex);
  predicted = true;

  signal_t signal;
  signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
  signal.get_data = &getSignalData;
  ei_impulse_result_t result = {};
  EI_IMPULSE_ERROR status = run_classifier(&signal, &result, false);
  if (status != EI_IMPULSE_OK) {
    Serial.print("ERROR: classifier status "); Serial.println(static_cast<int>(status));
    PredictionMessage failure = {}; failure.valid = false;
    xQueueOverwrite(predictionQueue, &failure);
    delay(500); return;
  }
  float jump = 0, notJump = 0;
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; ++i) {
    if (strcmp(result.classification[i].label, "jump") == 0)
      jump = result.classification[i].value;
    else if (strcmp(result.classification[i].label, "not_jump") == 0)
      notJump = result.classification[i].value;
  }
  PredictionMessage message = {jump, notJump, resets, clipped, gyroClipped, windowEndMs,
                               static_cast<int>(result.timing.dsp),
                               static_cast<int>(result.timing.classification), true};
  xQueueOverwrite(predictionQueue, &message);
}
void inferenceTask(void *) {
  while (true) { runInference(); delay(1); }
}

void initializeAI() {
  bufferMutex = xSemaphoreCreateMutex();
  predictionQueue = xQueueCreate(1, sizeof(PredictionMessage));
  if (!bufferMutex || !predictionQueue) {
    Serial.println("AI unavailable: memory allocation failed; normal lights/button continue");
    return;
  }
  if (!Wire.begin(21, 22, DEBUG_I2C_HZ)) {
    Serial.println("AI unavailable: I2C initialization failed; normal lights/button continue");
    return;
  }
  Wire.setTimeOut(20);
  if (xTaskCreatePinnedToCore(sampleTask, "accel100Hz", 4096, nullptr, 2,
                            &samplerTaskHandle, 0) != pdPASS) {
    Serial.println("AI unavailable: sampler task allocation failed; normal lights continue");
    return;
  }
  if (xTaskCreatePinnedToCore(inferenceTask, "jumpInference", 12288, nullptr, 1,
                            &inferenceTaskHandle, 1) != pdPASS)
    Serial.println("AI unavailable: inference task allocation failed; normal lights continue");
}
void reportMemory() {
  // ESP-IDF reports stack high-water marks in bytes: lowest free space so far.
  Serial.printf("MEM,uptime_ms=%lu,led_count=%u,led_buffer_B=%u,effect_backup_B=%u,heap_free_B=%u,heap_min_B=%u,heap_largest_B=%u,internal_free_B=%u,internal_min_B=%u,internal_largest_B=%u,loop_stack_min_free_B=%u,sampler_stack_min_free_B=%u,inference_stack_min_free_B=%u\n",
    static_cast<unsigned long>(millis()), static_cast<unsigned>(NUM_LEDS_1 + NUM_LEDS_2),
    static_cast<unsigned>(sizeof(strip1) + sizeof(strip2)),
    static_cast<unsigned>(sizeof(savedStrip1) + sizeof(savedStrip2)),
    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
    static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
    static_cast<unsigned>(samplerTaskHandle ? uxTaskGetStackHighWaterMark(samplerTaskHandle) : 0),
    static_cast<unsigned>(inferenceTaskHandle ? uxTaskGetStackHighWaterMark(inferenceTaskHandle) : 0));
}
