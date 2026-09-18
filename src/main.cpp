#include <Arduino.h>
#include "S3XYButton.h"
#include "S3XYStalk.h"
#include "S3XYStalkInput.h"
#include "Led.h"

Led led(LED_BUILTIN);

static bool serialInputToAction(char input, S3XYButtonAction* action) {
  if (input == 's') {
    *action = S3XYButtonAction::Single;
    return true;
  }
  if (input == 'd') {
    *action = S3XYButtonAction::Double;
    return true;
  }
  if (input == 'l') {
    *action = S3XYButtonAction::Long;
    return true;
  }
  return false;
}

static bool executeButtonAction(S3XYButtonAction action) {
  if (!s3xy_ready()) {
    Serial.println("[main] button action ignored: button is not ready");
    return false;
  }

  if (action == S3XYButtonAction::Single) {
    s3xy_send_single();
    led.runLed({
      {0, false},
      {100, true}
    }, 0);
    Serial.println("Sent single press");
  } else if (action == S3XYButtonAction::Double) {
    s3xy_send_double();
    led.runLed({
      {0, false},
      {100, true},
      {200, false},
      {300, true}
    }, 0);
    Serial.println("Sent double press");
  } else if (action == S3XYButtonAction::Long) {
    s3xy_send_long();
    led.runLed({
      {0, false},
      {500, true}
    }, 0);
    Serial.println("Sent long press");
  }

  return true;
}

static void processQueuedButtonActions() {
  S3XYButtonAction action;

  while (s3xy_stalk_input_dequeue_action(&action)) {
    executeButtonAction(action);
  }
}

void onButtonConnected() {
  Serial.println("[button] connected");
}

void onButtonDisconnected() {
  Serial.println("[button] disconnected");
}

void onStalkNotify(const uint8_t* data, size_t len) {
  s3xy_stalk_input_handle_notify(data, len);
}

void setup() {
  // Set up BLE peripheral/virtual button
  s3xy_on_connect(onButtonConnected);
  s3xy_on_disconnect(onButtonDisconnected);
  s3xy_begin("ENH_BTN");

  // Set up BLE central/stalk
  s3xy_stalk_on_notify(onStalkNotify);
  s3xy_stalk_begin();

  // Set up LED
  led.begin();
}

void loop() {
  s3xy_loop();

  static int previousReadyState = -1;
  int readyState = (s3xy_ready() ? 1 : 0) |
                   (s3xy_stalk_ready() ? 2 : 0);
  if (readyState != previousReadyState) {
    Serial.printf("[main] readiness state changed to %d (button=%d, stalk=%d)\n",
                  readyState, s3xy_ready(), s3xy_stalk_ready());
    previousReadyState = readyState;
  }

  // neither ready
  if (!s3xy_ready() && !s3xy_stalk_ready()) {
    if (!led.isRunning() || led.isRepeating()) {
      led.runLed({
        {0, true},
        {100, false}
      }, 1000);
    }
  }
  // only virtual button ready
  else if (s3xy_ready() && !s3xy_stalk_ready()) {
    if (!led.isRunning() || led.isRepeating()) {
      led.runLed({
        {0, true},
        {100, false},
        {200, true},
        {300, false}
      }, 1000);
    }
  }
  // only stalk ready
  else if (!s3xy_ready() && s3xy_stalk_ready()) {
    if (!led.isRunning() || led.isRepeating()) {
      led.runLed({
        {0, true},
        {100, false},
        {200, true},
        {300, false},
        {400, true},
        {500, false}
      }, 1000);
    }
  }
  // both ready
  else {
    if (led.isRepeating() || !led.isRunning()) {
      led.set(true);
    }

    if (Serial.available()) {
      S3XYButtonAction action;
      if (serialInputToAction(Serial.read(), &action)) {
        executeButtonAction(action);
      }
    }

    processQueuedButtonActions();
  }

  s3xy_stalk_loop();
}
