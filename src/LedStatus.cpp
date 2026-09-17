#include "LedStatus.h"

#include <Ticker.h>

#include "S3XYButton.h"
#include "S3XYStalk.h"

namespace {

const int LED_PIN = LED_BUILTIN;
const unsigned long LED_PULSE_ON_MS = 100;
const unsigned long LED_PULSE_PERIOD_MS = 1000;
const unsigned long LED_UPDATE_INTERVAL_MS = 25;
const unsigned long LED_BLINK_OFF_MS = 100;

unsigned long connectionPatternStartedAt = 0;
char connectionPattern = 0;
unsigned long actionPatternStartedAt = 0;
char actionPattern = 0;
Ticker statusTicker;

char getConnectionPattern() {
  bool buttonReady = s3xy_ready();
  bool stalkReady = s3xy_stalk_ready();

  if (buttonReady && stalkReady) {
    return 0;
  }

  if (buttonReady) {
    return 2;
  }

  if (stalkReady) {
    return 3;
  }

  return 1;
}

void updateLed() {
  if (actionPattern != 0) {
    unsigned long elapsed = millis() - actionPatternStartedAt;
    bool finished = false;
    bool ledOn = false;

    if (actionPattern == 's') {
      finished = elapsed >= 100UL;
    } else if (actionPattern == 'd') {
      finished = elapsed >= 300UL;
      ledOn = elapsed >= 100UL && elapsed < 200UL;
    } else if (actionPattern == 'l') {
      finished = elapsed >= 500UL;
    } else {
      finished = true;
    }

    if (!finished) {
      digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
      return;
    }

    actionPattern = 0;
  }

  char currentPattern = getConnectionPattern();
  if (currentPattern != connectionPattern) {
    connectionPattern = currentPattern;
    connectionPatternStartedAt = millis();
  }

  if (currentPattern == 0) {
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  unsigned long elapsed = millis() - connectionPatternStartedAt;
  unsigned long cyclePosition = elapsed % LED_PULSE_PERIOD_MS;
  unsigned long blinkWindow = LED_PULSE_ON_MS + LED_BLINK_OFF_MS;
  bool ledOn = cyclePosition < currentPattern * blinkWindow &&
               (cyclePosition % blinkWindow) < LED_PULSE_ON_MS;

  digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
}

}  // namespace

void led_status_begin() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  connectionPatternStartedAt = millis();
  statusTicker.attach_ms(LED_UPDATE_INTERVAL_MS, updateLed);
}

void led_status_action(S3XYButtonAction action) {
  if (action == S3XYButtonAction::Single) {
    actionPattern = 's';
  } else if (action == S3XYButtonAction::Double) {
    actionPattern = 'd';
  } else {
    actionPattern = 'l';
  }
  actionPatternStartedAt = millis();
  digitalWrite(LED_PIN, LOW);
}