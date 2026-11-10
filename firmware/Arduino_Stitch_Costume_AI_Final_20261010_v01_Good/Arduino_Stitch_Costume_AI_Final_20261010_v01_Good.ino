#include <Arduino.h>
#include <FastLED.h>
#include <cstring>

// =====================================================
// HARDWARE
// =====================================================

#define STRIP1_PIN 18
#define STRIP2_PIN 19
#define BUTTON_PIN 27

// Strip 1 = ears
// Strip 2 = stomach/body
#define NUM_LEDS_1 62
#define NUM_LEDS_2 120

#define LED_TYPE WS2812B
#define COLOR_ORDER GRB

CRGB strip1[NUM_LEDS_1];
CRGB strip2[NUM_LEDS_2];

// =====================================================
// BUTTON TIMING
// =====================================================

const unsigned long DEBOUNCE_TIME    = 30;
const unsigned long MIN_PRESS_TIME   = 20;
const unsigned long BRIGHTNESS_HOLD  = 1000;
const unsigned long AI_HOLD          = 3500;
const unsigned long OFF_HOLD         = 6000;
const unsigned long MULTI_CLICK_TIME = 400;

// =====================================================
// RAW / DEBOUNCED BUTTON STATE
// =====================================================

bool rawButtonState = HIGH;
bool lastRawButtonState = HIGH;
bool stableButtonState = HIGH;

unsigned long rawChangeTime = 0;
unsigned long pressStartTime = 0;

// =====================================================
// BUTTON GESTURE STATE MACHINE
// =====================================================

enum ButtonGestureState {
  BUTTON_IDLE,
  BUTTON_PRESSED,
  BUTTON_BRIGHTNESS_TRIGGERED,
  BUTTON_AI_TRIGGERED,
  BUTTON_OFF_TRIGGERED
};

ButtonGestureState buttonGesture = BUTTON_IDLE;

// =====================================================
// CLICK TRACKING
// =====================================================

uint8_t clickCount = 0;
unsigned long lastClickTime = 0;

// =====================================================
// MODES
// =====================================================
//
// 0 = Purple / Cyan
// 1 = Opposite Rainbows
// 2 = Meteor / Sparkles
// 3 = Dual Pulse
// 4 = Fire / Ice
// 5 = Party
// 6 = Dual Bounce
// 7 = Snake Round Trip
//
// OFF is a separate system state.
// Hyperspace is a hidden state.
// =====================================================

const int NUM_MODES = 8;

int mode = 0;

// Power-on starts lit, with AI enabled and a green belly flash.
bool systemOff = false;

// Hidden Easter egg
bool hyperspaceActive = false;

// =====================================================
// BRIGHTNESS
// =====================================================

uint8_t brightnessLevels[] = {
  25,
  50,
  90,
  140
};

const int NUM_BRIGHTNESS_LEVELS =
  sizeof(brightnessLevels) /
  sizeof(brightnessLevels[0]);

int brightnessIndex = 0; // Boot at the lowest normal brightness (25).
int brightnessIndexAtPress = 0;
uint8_t brightnessAtPress = 25;
uint8_t aiFeedbackRestoreBrightness = 25;
bool restoreBrightnessAfterAIFeedback = false;

const uint8_t HYPERSPACE_BRIGHTNESS = 200;

// =====================================================
// ANIMATION STATE
// =====================================================

unsigned long lastFrame = 0;
const unsigned long FRAME_TIME = 20;

uint8_t hue1 = 0;
uint8_t hue2 = 100;

int meteor1 = 0;

uint16_t hyperspaceOffset = 0;


// Temporary effects never change the selected mode or secret-mode state.
bool aiEnabled = true;
bool jumpEffectActive = false;
bool feedbackActive = false;
bool feedbackPending = false;
bool pendingFeedbackEnabled = true;
uint32_t jumpEffectStartedMs = 0, feedbackStartedMs = 0;
uint32_t aiEnabledSinceMs = 0;
uint32_t baseRevision = 0, savedBaseRevision = 0;
constexpr uint32_t DUAL_BOUNCE_CYCLE_MS = 4200;
// Another 50% faster than the previous sweep: duration / 1.50.
constexpr uint32_t SWEEP_LEG_MS = (DUAL_BOUNCE_CYCLE_MS * 1000UL / 3003UL) / 2;
constexpr uint32_t JUMP_COOLDOWN_MS = 500;
uint32_t jumpEffectEndedMs = 0;
bool jumpCooldownActive = false;
constexpr size_t SWEEP_WIDTH = 12;
constexpr bool REVERSE_BODY = false; // Pixel zero at bottom of belly path.
constexpr bool REVERSE_EARS = false; // Ear path continues after the belly.
constexpr uint32_t FEEDBACK_MS = 600; // Two 150ms flashes with dark intervals.
CRGB savedStrip1[NUM_LEDS_1], savedStrip2[NUM_LEDS_2];

#include "AIEngine.h"

void saveBasePixels() {
  memcpy(savedStrip1, strip1, sizeof(strip1));
  memcpy(savedStrip2, strip2, sizeof(strip2));
  savedBaseRevision = baseRevision;
}

void restoreBasePixels() {
  if (savedBaseRevision == baseRevision) {
    memcpy(strip1, savedStrip1, sizeof(strip1));
    memcpy(strip2, savedStrip2, sizeof(strip2));
  } else FastLED.clear(); // A button selected a new mode during the effect.
}

void showBaseChange() {
  if (!jumpEffectActive && !feedbackActive) showCoordinated();
}

void startFeedback(bool enabled) {
  if (jumpEffectActive) {
    feedbackPending = true; pendingFeedbackEnabled = enabled;
    return; // A jump always finishes before feedback begins.
  }
  if (feedbackActive) restoreBasePixels();
  saveBasePixels();
  feedbackActive = true; pendingFeedbackEnabled = enabled;
  feedbackStartedMs = millis();
}

void enableAIOnWake() {
  aiEnabled = true;
  aiEnabledSinceMs = millis();
  aiInferenceEnabled.store(true);
  if (predictionQueue) xQueueReset(predictionQueue);
  startFeedback(true);
  Serial.println("AI: enabled on wake");
}

void toggleAI() {
  aiEnabled = !aiEnabled;
  aiEnabledSinceMs = millis();
  aiInferenceEnabled.store(aiEnabled && !systemOff);
  if (predictionQueue) xQueueReset(predictionQueue);
  startFeedback(aiEnabled);
  Serial.println(aiEnabled ? "AI: enabled" : "AI: disabled");
}

void drawJumpSweep(uint32_t elapsed) {
  FastLED.clear();
  const uint32_t legTime = elapsed < SWEEP_LEG_MS ? elapsed :
    (elapsed < 2 * SWEEP_LEG_MS ? 2 * SWEEP_LEG_MS - elapsed : 0);
  // Both zones traverse their own length together, like mode 6.
  // Each direction takes 699ms on both ears and belly.
  for (uint8_t zone = 0; zone < 2; ++zone) {
    CRGB *pixels = zone == 0 ? strip2 : strip1;
    const size_t count = zone == 0 ? NUM_LEDS_2 : NUM_LEDS_1;
    const bool reverse = zone == 0 ? REVERSE_BODY : REVERSE_EARS;
    const size_t center = (count - 1) * legTime / SWEEP_LEG_MS;
    for (size_t i = 0; i < count; ++i) {
      const size_t distance = i > center ? i - center : center - i;
      if (distance >= SWEEP_WIDTH) continue;
      pixels[reverse ? count - 1 - i : i] = CHSV(
        static_cast<uint8_t>(i * 255 / count), 255,
        255 - distance * 200 / SWEEP_WIDTH);
    }
  }
}

void updateTemporaryEffects(uint32_t now) {
  if (jumpEffectActive && now - jumpEffectStartedMs >= 2 * SWEEP_LEG_MS) {
    jumpEffectActive = false; restoreBasePixels();
    jumpEffectEndedMs = now; jumpCooldownActive = true;
    Serial.println("JUMP_END: sequence complete; previous mode restored; cooldown500ms");
  }
  if (!jumpEffectActive && feedbackPending) {
    feedbackPending = false; startFeedback(pendingFeedbackEnabled);
  }
  if (feedbackActive && now - feedbackStartedMs >= FEEDBACK_MS) {
    feedbackActive = false; restoreBasePixels();
    if (restoreBrightnessAfterAIFeedback) {
      FastLED.setBrightness(aiFeedbackRestoreBrightness);
      restoreBrightnessAfterAIFeedback = false;
    }
  }
}

void consumePrediction(uint32_t now) {
  if (!predictionQueue || !bufferMutex) return;
  bool failed; uint32_t resets;
  xSemaphoreTake(bufferMutex, portMAX_DELAY);
  failed = sensorFailed; resets = timingResets;
  xSemaphoreGive(bufferMutex);
  PredictionMessage message;
  if (xQueueReceive(predictionQueue, &message, 0) != pdTRUE) return;
  bool fresh = message.valid && !failed && message.resets == resets &&
               now - message.windowEndMs <= 400 &&
               static_cast<int32_t>(message.windowEndMs - aiEnabledSinceMs) >= 0;
  if (jumpCooldownActive && now - jumpEffectEndedMs >= JUMP_COOLDOWN_MS)
    jumpCooldownActive = false;
  // Busy effects/cooldown ignore predictions, without queuing them.
  if (fresh && aiEnabled && !systemOff && !jumpEffectActive && !feedbackActive &&
      !feedbackPending && !jumpCooldownActive && message.jump >= CONFIDENCE) {
    saveBasePixels(); jumpEffectActive = true; jumpEffectStartedMs = now;
    Serial.println("JUMP_START: complete colorful up/down sweep");
  }

}

void setup() {
  Serial.setTxBufferSize(2048); // Buffer fault logs to keep button polling responsive.
  Serial.begin(115200);
  initializeRuntime();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  FastLED.addLeds<LED_TYPE, STRIP1_PIN, COLOR_ORDER>(strip1, NUM_LEDS_1);
  FastLED.addLeds<LED_TYPE, STRIP2_PIN, COLOR_ORDER>(strip2, NUM_LEDS_2);
  FastLED.setBrightness(brightnessLevels[brightnessIndex]);
  FastLED.clear(); showCoordinated();
  rawButtonState = digitalRead(BUTTON_PIN);
  lastRawButtonState = rawButtonState;
  stableButtonState = rawButtonState;
  rawChangeTime = millis();
  runMode();
  startFeedback(true);
  initializeAI(); // Sensor initialization/recovery runs in its own task.
  Serial.printf("Costume: D18 ears62 / D19 body130 / D27 button; threshold=%.2f; FFT1s; updates200ms; I2C=%luHz\n", CONFIDENCE, static_cast<unsigned long>(DEBUG_I2C_HZ));
}

void loop() {
  handleButton();
  handleClicks();
  uint32_t now = millis();
  updateTemporaryEffects(now);
  consumePrediction(now);
  if (now - lastFrame >= FRAME_TIME) {
    lastFrame = now;
    if (systemOff) FastLED.clear();
    else if (jumpEffectActive) drawJumpSweep(now - jumpEffectStartedMs);
    else if (feedbackActive) {
      const bool flashOn = ((now - feedbackStartedMs) / 150) % 2 == 0;
      const CRGB feedbackColor = flashOn ?
        (pendingFeedbackEnabled ? CRGB::Green : CRGB::Red) : CRGB::Black;
      fill_solid(strip1, NUM_LEDS_1, feedbackColor);
      fill_solid(strip2, NUM_LEDS_2, feedbackColor);
    } else if (hyperspaceActive) modeHyperspaceJump();
    else runMode();
    showCoordinated();
  }
  static uint32_t lastMemoryReport = 0;
  if (now - lastMemoryReport >= MEMORY_REPORT_MS) {
    lastMemoryReport = now; reportMemory();
  }
  serviceHealth();
  delay(1);
}

void handleButton() {

  unsigned long now = millis();

  rawButtonState =
    digitalRead(BUTTON_PIN);

  // -------------------------------------------------
  // RAW INPUT CHANGED
  // -------------------------------------------------

  if (
    rawButtonState !=
    lastRawButtonState
  ) {

    rawChangeTime = now;

    lastRawButtonState =
      rawButtonState;
  }

  // -------------------------------------------------
  // DEBOUNCED STATE CHANGE
  // -------------------------------------------------

  if (
    now - rawChangeTime >= DEBOUNCE_TIME &&
    rawButtonState != stableButtonState
  ) {

    stableButtonState =
      rawButtonState;

    // -----------------------------------------------
    // CLEAN PRESS
    // -----------------------------------------------

    if (
      stableButtonState == LOW
    ) {

      pressStartTime = now;
      brightnessIndexAtPress = brightnessIndex;
      brightnessAtPress = FastLED.getBrightness();

      buttonGesture =
        BUTTON_PRESSED;
    }

    // -----------------------------------------------
    // CLEAN RELEASE
    // -----------------------------------------------

    else {

      unsigned long pressDuration =
        now - pressStartTime;

      // Only an untouched BUTTON_PRESSED state
      // can become a short click.
      if (
        buttonGesture ==
        BUTTON_PRESSED
      ) {

        if (
          pressDuration >=
          MIN_PRESS_TIME && (!systemOff || pressDuration < BRIGHTNESS_HOLD)
        ) {

          clickCount++;

          lastClickTime = now;

          // -----------------------------------------
          // TRIPLE CLICK = HYPERSPACE
          // -----------------------------------------

          if (
            clickCount >= 3
          ) {

            activateHyperspace();

            clickCount = 0;
          }
        }
      }

      buttonGesture =
        BUTTON_IDLE;
    }
  }

  // -------------------------------------------------
  // BUTTON CURRENTLY HELD
  // -------------------------------------------------

  if (
    stableButtonState == LOW
  ) {

    unsigned long heldTime =
      now - pressStartTime;

    // If OFF, only a short tap wakes it.
    if (systemOff) {
      return;
    }

    // -----------------------------------------------
    // 1 SECOND = BRIGHTNESS
    // -----------------------------------------------

    if (
      buttonGesture ==
        BUTTON_PRESSED &&
      heldTime >=
        BRIGHTNESS_HOLD
    ) {

      clickCount = 0;

      changeBrightness();

      buttonGesture =
        BUTTON_BRIGHTNESS_TRIGGERED;
    }

    // -----------------------------------------------
    // 3.5 SECONDS = AI TOGGLE
    // -----------------------------------------------

    if (
      (
        buttonGesture ==
          BUTTON_PRESSED ||
        buttonGesture ==
          BUTTON_BRIGHTNESS_TRIGGERED
      ) &&
      heldTime >=
        AI_HOLD
    ) {

      clickCount = 0;

      // Undo the 1-second brightness step if this becomes an AI gesture.
      // Keep its current output level for feedback; restore the independent
      // press-time value when feedback ends (even if feedback was deferred).
      brightnessIndex = brightnessIndexAtPress;
      aiFeedbackRestoreBrightness = brightnessAtPress;
      restoreBrightnessAfterAIFeedback = true;
      toggleAI();

      buttonGesture =
        BUTTON_AI_TRIGGERED;
    }
    // Continuing the same hold to six seconds turns the lights off.
    if (buttonGesture == BUTTON_AI_TRIGGERED && heldTime >= OFF_HOLD) {
      clickCount = 0;
      turnSystemOff();
      buttonGesture = BUTTON_OFF_TRIGGERED;
    }
  }
}

void handleClicks() {

  if (
    clickCount == 0
  ) {

    return;
  }

  if (
    stableButtonState == LOW
  ) {

    return;
  }

  if (
    millis() - lastClickTime <
    MULTI_CLICK_TIME
  ) {

    return;
  }

  uint8_t clicksToProcess =
    clickCount;

  clickCount = 0;

  for (
    uint8_t i = 0;
    i < clicksToProcess;
    i++
  ) {

    processShortPress();
  }
}

void processShortPress() {

  // -------------------------------------------------
  // OFF -> WAKE
  // -------------------------------------------------

  if (systemOff) {

    systemOff = false;
    hyperspaceActive = false;

    FastLED.setBrightness(
      brightnessLevels[brightnessIndex]
    );

    resetAnimation();
    enableAIOnWake();

    return;
  }

  // -------------------------------------------------
  // HYPERSPACE -> NORMAL
  // -------------------------------------------------

  if (hyperspaceActive) {

    hyperspaceActive = false;

    FastLED.setBrightness(
      brightnessLevels[brightnessIndex]
    );

    resetAnimation();

    return;
  }

  // -------------------------------------------------
  // NEXT NORMAL MODE
  // -------------------------------------------------

  mode++;

  if (
    mode >= NUM_MODES
  ) {

    mode = 0;
  }

  resetAnimation();
}

void turnSystemOff() {

  jumpEffectActive = false; feedbackActive = false; feedbackPending = false;
  restoreBrightnessAfterAIFeedback = false;
  aiInferenceEnabled.store(false);
  if (predictionQueue) xQueueReset(predictionQueue);
  systemOff = true;
  hyperspaceActive = false;

  FastLED.setBrightness(
    brightnessLevels[brightnessIndex]
  );

  FastLED.clear();
  showCoordinated();
}

void activateHyperspace() {

  const bool wasOff = systemOff;
  ++baseRevision;
  systemOff = false;
  hyperspaceActive = true;

  hyperspaceOffset = 0;

  FastLED.clear();

  FastLED.setBrightness(
    HYPERSPACE_BRIGHTNESS
  );

  showBaseChange();
  if (wasOff) enableAIOnWake();
}

void resetAnimation() {

  ++baseRevision;
  FastLED.clear();

  meteor1 = 0;

  hue1 = 0;
  hue2 = 100;

  hyperspaceOffset = 0;

  showBaseChange();
}

void changeBrightness() {

  brightnessIndex++;

  if (
    brightnessIndex >=
    NUM_BRIGHTNESS_LEVELS
  ) {

    brightnessIndex = 0;
  }

  if (hyperspaceActive) {

    FastLED.setBrightness(
      HYPERSPACE_BRIGHTNESS
    );
  }

  else {

    FastLED.setBrightness(
      brightnessLevels[brightnessIndex]
    );
  }

  showBaseChange();
}

void runMode() {

  switch (mode) {

    case 0:
      modePurpleCyan();
      break;

    case 1:
      modeOppositeRainbows();
      break;

    case 2:
      modeMeteorAndSparkle();
      break;

    case 3:
      modeDualPulse();
      break;

    case 4:
      modeFireAndIce();
      break;

    case 5:
      modeParty();
      break;

    case 6:
      modeDualBounce();
      break;

    case 7:
      modeSnakeRoundTrip();
      break;
  }
}

void modePurpleCyan() {

  fill_solid(
    strip1,
    NUM_LEDS_1,
    CRGB(180, 0, 255)
  );

  fill_solid(
    strip2,
    NUM_LEDS_2,
    CRGB(0, 200, 255)
  );
}

void modeOppositeRainbows() {

  fill_rainbow(
    strip1,
    NUM_LEDS_1,
    hue1,
    4
  );

  for (
    int i = 0;
    i < NUM_LEDS_2;
    i++
  ) {

    strip2[i] =
      CHSV(
        hue2 - i * 4,
        255,
        255
      );
  }

  hue1++;
  hue2--;
}

void modeMeteorAndSparkle() {

  // -------------------------------------------------
  // EARS - METEOR
  // -------------------------------------------------

  fadeToBlackBy(
    strip1,
    NUM_LEDS_1,
    45
  );

  for (
    int i = 0;
    i < 12;
    i++
  ) {

    int pixel =
      meteor1 - i;

    if (
      pixel >= 0 &&
      pixel < NUM_LEDS_1
    ) {

      strip1[pixel] =
        CHSV(
          190,
          255,
          255 - i * 18
        );
    }
  }

  meteor1++;

  if (
    meteor1 >
    NUM_LEDS_1 + 12
  ) {

    meteor1 = 0;
  }

  // -------------------------------------------------
  // BODY - SPARKLES
  // -------------------------------------------------

  fadeToBlackBy(
    strip2,
    NUM_LEDS_2,
    30
  );

  if (
    random8() < 160
  ) {

    int pixel =
      random16(
        NUM_LEDS_2
      );

    uint8_t sparkleHue =
      random8(
        200,
        245
      );

    strip2[pixel] =
      CHSV(
        sparkleHue,
        220,
        255
      );
  }

  if (
    random8() < 30
  ) {

    strip2[
      random16(
        NUM_LEDS_2
      )
    ] = CRGB::White;
  }
}

void modeDualPulse() {

  uint8_t pulse1 =
    beatsin8(
      20,
      30,
      255
    );

  uint8_t pulse2 =
    beatsin8(
      20,
      30,
      255,
      0,
      128
    );

  fill_solid(
    strip1,
    NUM_LEDS_1,
    CHSV(
      205,
      220,
      pulse1
    )
  );

  fill_solid(
    strip2,
    NUM_LEDS_2,
    CHSV(
      235,
      210,
      pulse2
    )
  );
}

void modeFireAndIce() {

  // -------------------------------------------------
  // EARS - FIRE
  // -------------------------------------------------

  for (
    int i = 0;
    i < NUM_LEDS_1;
    i++
  ) {

    uint8_t flicker =
      random8(
        150,
        255
      );

    strip1[i] =
      CRGB(
        flicker,
        random8(
          20,
          90
        ),
        0
      );
  }

  // -------------------------------------------------
  // BODY - ICE
  // -------------------------------------------------

  for (
    int i = 0;
    i < NUM_LEDS_2;
    i++
  ) {

    uint8_t value =
      sin8(
        i * 7 +
        millis() / 8
      );

    strip2[i] =
      CHSV(
        145 + value / 8,
        180,
        120 + value / 2
      );
  }
}

void modeParty() {

  // -------------------------------------------------
  // EARS
  // -------------------------------------------------

  for (
    int i = 0;
    i < NUM_LEDS_1;
    i++
  ) {

    strip1[i] =
      CHSV(
        hue1 + i * 3,
        255,
        255
      );
  }

  hue1 += 2;

  // -------------------------------------------------
  // BODY
  // -------------------------------------------------

  fadeToBlackBy(
    strip2,
    NUM_LEDS_2,
    60
  );

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int pixel =
      random16(
        NUM_LEDS_2
      );

    if (
      random8() < 80
    ) {

      strip2[pixel] =
        CRGB::White;
    }

    else {

      strip2[pixel] =
        CHSV(
          random8(),
          255,
          255
        );
    }
  }
}

void modeDualBounce() {

  const unsigned long CYCLE_TIME = DUAL_BOUNCE_CYCLE_MS;

  const int EAR_TRAIL_LENGTH = 20;
  const int BODY_TRAIL_LENGTH = 28;

  unsigned long phase =
    millis() % CYCLE_TIME;

  FastLED.clear();

  // -------------------------------------------------
  // FORWARD / REVERSE HALF
  // -------------------------------------------------

  bool movingForward =
    phase < (CYCLE_TIME / 2);

  unsigned long travelPhase;

  if (movingForward) {

    travelPhase = phase;
  }

  else {

    travelPhase =
      phase - (CYCLE_TIME / 2);
  }

  // -------------------------------------------------
  // EAR POSITION
  // -------------------------------------------------

  int earHead;

  if (movingForward) {

    earHead =
      map(
        travelPhase,
        0,
        CYCLE_TIME / 2,
        0,
        NUM_LEDS_1 - 1
      );
  }

  else {

    earHead =
      map(
        travelPhase,
        0,
        CYCLE_TIME / 2,
        NUM_LEDS_1 - 1,
        0
      );
  }

  // -------------------------------------------------
  // BODY POSITION
  // -------------------------------------------------

  int bodyHead;

  if (movingForward) {

    bodyHead =
      map(
        travelPhase,
        0,
        CYCLE_TIME / 2,
        0,
        NUM_LEDS_2 - 1
      );
  }

  else {

    bodyHead =
      map(
        travelPhase,
        0,
        CYCLE_TIME / 2,
        NUM_LEDS_2 - 1,
        0
      );
  }

  // -------------------------------------------------
  // COLOR EVOLUTION
  // -------------------------------------------------

  uint8_t baseHue =
    millis() / 18;

  uint8_t earHue =
    baseHue;

  uint8_t bodyHue =
    baseHue + 90;

  // -------------------------------------------------
  // DRAW EAR COMET
  // -------------------------------------------------

  for (
    int tail = 0;
    tail < EAR_TRAIL_LENGTH;
    tail++
  ) {

    int pixel;

    if (movingForward) {

      pixel =
        earHead - tail;
    }

    else {

      pixel =
        earHead + tail;
    }

    if (
      pixel < 0 ||
      pixel >= NUM_LEDS_1
    ) {

      continue;
    }

    uint8_t brightness =
      map(
        tail,
        0,
        EAR_TRAIL_LENGTH - 1,
        255,
        15
      );

    strip1[pixel] =
      CHSV(
        earHue + tail * 3,
        255,
        brightness
      );
  }

  // White head
  if (
    earHead >= 0 &&
    earHead < NUM_LEDS_1
  ) {

    strip1[earHead] =
      CRGB::White;
  }

  // -------------------------------------------------
  // DRAW BODY COMET
  // -------------------------------------------------

  for (
    int tail = 0;
    tail < BODY_TRAIL_LENGTH;
    tail++
  ) {

    int pixel;

    if (movingForward) {

      pixel =
        bodyHead - tail;
    }

    else {

      pixel =
        bodyHead + tail;
    }

    if (
      pixel < 0 ||
      pixel >= NUM_LEDS_2
    ) {

      continue;
    }

    uint8_t brightness =
      map(
        tail,
        0,
        BODY_TRAIL_LENGTH - 1,
        255,
        15
      );

    strip2[pixel] =
      CHSV(
        bodyHue + tail * 2,
        255,
        brightness
      );
  }

  // White head
  if (
    bodyHead >= 0 &&
    bodyHead < NUM_LEDS_2
  ) {

    strip2[bodyHead] =
      CRGB::White;
  }
}

void modeSnakeRoundTrip() {

  const int TOTAL_PATH =
    NUM_LEDS_2 +
    NUM_LEDS_1;

  const int SNAKE_LENGTH = 28;

  const unsigned long TRIP_TIME = 7000;

  unsigned long phase =
    millis() % TRIP_TIME;

  unsigned long halfTrip =
    TRIP_TIME / 2;

  bool forward =
    phase < halfTrip;

  unsigned long localPhase =
    forward
      ? phase
      : phase - halfTrip;

  int head;

  if (forward) {

    head =
      map(
        localPhase,
        0,
        halfTrip,
        0,
        TOTAL_PATH - 1
      );
  }

  else {

    head =
      map(
        localPhase,
        0,
        halfTrip,
        TOTAL_PATH - 1,
        0
      );
  }

  FastLED.clear();

  uint8_t snakeHue =
    millis() / 20;

  // -------------------------------------------------
  // DRAW TRAIL
  //
  // Trail reflects at each endpoint so there is
  // no dark/dead period during the turnaround.
  // -------------------------------------------------

  for (
    int tail = 0;
    tail < SNAKE_LENGTH;
    tail++
  ) {

    int position;

    if (forward) {

      position =
        head - tail;
    }

    else {

      position =
        head + tail;
    }

    // Reflect trail back into the valid path
    // instead of allowing it to disappear
    // beyond either endpoint.

    if (position < 0) {

      position = -position;
    }

    if (position >= TOTAL_PATH) {

      position =
        (TOTAL_PATH - 1) -
        (position - (TOTAL_PATH - 1));
    }

    position =
      constrain(
        position,
        0,
        TOTAL_PATH - 1
      );

    uint8_t brightness =
      map(
        tail,
        0,
        SNAKE_LENGTH - 1,
        255,
        20
      );

    CRGB color =
      CHSV(
        snakeHue + tail * 2,
        255,
        brightness
      );

    if (
      position < NUM_LEDS_2
    ) {

      strip2[position] += color;
    }

    else {

      int earPixel =
        position -
        NUM_LEDS_2;

      if (
        earPixel >= 0 &&
        earPixel < NUM_LEDS_1
      ) {

        strip1[earPixel] += color;
      }
    }
  }

  // -------------------------------------------------
  // WHITE HEAD
  // -------------------------------------------------

  if (
    head < NUM_LEDS_2
  ) {

    strip2[head] =
      CRGB::White;
  }

  else {

    int earPixel =
      head -
      NUM_LEDS_2;

    if (
      earPixel >= 0 &&
      earPixel < NUM_LEDS_1
    ) {

      strip1[earPixel] =
        CRGB::White;
    }
  }
}

void modeHyperspaceJump() {

  fadeToBlackBy(
    strip1,
    NUM_LEDS_1,
    75
  );

  fadeToBlackBy(
    strip2,
    NUM_LEDS_2,
    75
  );

  uint8_t baseHue =
    millis() / 8;

  // -------------------------------------------------
  // EARS
  // -------------------------------------------------

  for (
    int i = 0;
    i < NUM_LEDS_1;
    i++
  ) {

    uint8_t wave =
      sin8(
        i * 18 +
        hyperspaceOffset
      );

    if (
      wave > 220
    ) {

      uint8_t brightness =
        map(
          wave,
          220,
          255,
          80,
          255
        );

      strip1[i] =
        CHSV(
          baseHue + i * 5,
          255,
          brightness
        );
    }
  }

  // -------------------------------------------------
  // BODY
  // -------------------------------------------------

  for (
    int i = 0;
    i < NUM_LEDS_2;
    i++
  ) {

    uint8_t wave =
      sin8(
        i * 14 +
        hyperspaceOffset
      );

    if (
      wave > 218
    ) {

      uint8_t brightness =
        map(
          wave,
          218,
          255,
          80,
          255
        );

      strip2[i] =
        CHSV(
          baseHue + i * 4,
          255,
          brightness
        );
    }
  }

  // -------------------------------------------------
  // WHITE STARS
  // -------------------------------------------------

  if (
    random8() < 110
  ) {

    strip1[
      random16(
        NUM_LEDS_1
      )
    ] = CRGB::White;
  }

  if (
    random8() < 150
  ) {

    strip2[
      random16(
        NUM_LEDS_2
      )
    ] = CRGB::White;
  }

  hyperspaceOffset += 18;
}
