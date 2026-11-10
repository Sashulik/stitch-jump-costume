#pragma once
#include <esp_system.h>

// Constant-size records and a non-blocking queue: no printing on successful reads.
constexpr uint32_t DEBUG_I2C_HZ = 100000;
constexpr uint32_t HEALTH_REPORT_MS = 60000;
QueueHandle_t debugQueue = nullptr;
SemaphoreHandle_t ledSensorMutex = nullptr;
std::atomic<uint32_t> dbgIOWaitMaxUs{0}, dbgLEDWaitMaxUs{0};
constexpr uint8_t SENSOR_FAILURE_LIMIT = 3;

// One shared, priority-inheriting mutex; never held across delays or bufferMutex.
class LedSensorGuard {
  SemaphoreHandle_t mutex;
public:
  explicit LedSensorGuard(std::atomic<uint32_t> &waitMax) : mutex(ledSensorMutex) {
    const uint32_t start = micros();
    if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
    uint32_t elapsed = micros() - start, old = waitMax.load();
    while (elapsed > old && !waitMax.compare_exchange_weak(old, elapsed)) {}
  }
  ~LedSensorGuard() { if (mutex) xSemaphoreGive(mutex); }
  LedSensorGuard(const LedSensorGuard &) = delete;
  LedSensorGuard &operator=(const LedSensorGuard &) = delete;
};
std::atomic<uint32_t> dbgDropped{0}, dbgReadOK{0}, dbgReadFailed{0}, dbgWriteFailed{0};
std::atomic<uint32_t> dbgConfigureAttempts{0}, dbgConfigureOK{0}, dbgTimingGaps{0};
std::atomic<uint32_t> dbgLastGoodMs{0}, dbgReadMaxUs{0}, dbgSampleIntervalMaxUs{0};
struct DebugIO {
  uint32_t atMs, durationUs;
  uint8_t kind, address, reg, stage, status, expected, received;
};
// kind: 0=read,1=write,2=unexpected identity,3=range verification
// stage: 1=TX buffer,2=endTransmission,3=requestFrom,4=RX buffer,5=value check
void debugMaximum(std::atomic<uint32_t> &target, uint32_t value) {
  uint32_t old = target.load();
  while (value > old && !target.compare_exchange_weak(old, value)) {}
}

void showCoordinated() {
  LedSensorGuard guard(dbgLEDWaitMaxUs);
  FastLED.show();
}

DebugIO debugBeginIO(uint8_t kind, uint8_t address, uint8_t reg, uint8_t expected) {
  DebugIO e = {};
  e.atMs = millis(); e.kind = kind; e.address = address; e.reg = reg; e.expected = expected;
  return e;
}

void debugFailure(DebugIO e, uint32_t startedUs, uint8_t stage, uint8_t status, uint8_t received) {
  e.durationUs = micros() - startedUs; e.stage = stage; e.status = status; e.received = received;
  if (!debugQueue || xQueueSend(debugQueue, &e, 0) != pdTRUE) dbgDropped.fetch_add(1);
}

const char *debugWireStatus(uint8_t code) {
  switch (code) {
    case 0: return "OK_OR_DEFERRED";
    case 1: return "TX_BUFFER_TOO_LONG";
    case 2: return "NACK_OR_ESP_FAIL";
    case 3: return "DATA_NACK";
    case 4: return "OTHER_ERROR";
    case 5: return "TIMEOUT";
    default: return "UNKNOWN";
  }
}

void initializeRuntime() {
  ledSensorMutex = xSemaphoreCreateMutex();
  if (!ledSensorMutex) {
    Serial.println("FATAL: cannot allocate LED/sensor coordination mutex; restart required");
    while (true) delay(1000);
  }

  debugQueue = xQueueCreate(16, sizeof(DebugIO));

}

void serviceHealth() {
  DebugIO e;
  if (debugQueue && xQueueReceive(debugQueue, &e, 0) == pdTRUE) {
    Serial.printf("I2C_FAIL,t_ms=%lu,op=%u,addr=0x%02X,reg=0x%02X,stage=%u,status=%u(%s),expected=%u,received=%u,io_us=%lu\n",
      static_cast<unsigned long>(e.atMs),e.kind,e.address,e.reg,e.stage,e.status,
      debugWireStatus(e.status),e.expected,e.received,static_cast<unsigned long>(e.durationUs));
  }
  static uint32_t lastReport = 0;
  const uint32_t now = millis();
  if (now - lastReport < HEALTH_REPORT_MS) return;
  lastReport = now;
  bool ready = false; uint32_t resets = 0, clips = 0, gyroClips = 0;
  if (bufferMutex) {
    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    ready = !sensorFailed; resets = timingResets;
    clips = clippedSamples; gyroClips = gyroClippedSamples;
    xSemaphoreGive(bufferMutex);
  }
  Serial.printf("HEALTH,t_ms=%lu,sensor_ready=%u,reads_failed=%lu,writes_failed=%lu,timing_resets=%lu,sample_gaps=%lu,accel_clips=%lu,gyro_clips=%lu,dropped_errors=%lu\n",
    static_cast<unsigned long>(now),static_cast<unsigned>(ready),
    static_cast<unsigned long>(dbgReadFailed.load()),static_cast<unsigned long>(dbgWriteFailed.load()),
    static_cast<unsigned long>(resets),static_cast<unsigned long>(dbgTimingGaps.load()),
    static_cast<unsigned long>(clips),static_cast<unsigned long>(gyroClips),
    static_cast<unsigned long>(dbgDropped.load()));
}
