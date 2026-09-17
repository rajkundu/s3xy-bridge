#include <Arduino.h>
#include "S3XYButton.h"
#include "S3XYStalk.h"
#include "S3XYStalkInput.h"
#include "LedStatus.h"

unsigned long lastTriggerTime = 0;
int autoStep = 0;

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
    led_status_action(S3XYButtonAction::Single);
    Serial.println("Sent single press");
  } else if (action == S3XYButtonAction::Double) {
    s3xy_send_double();
    led_status_action(S3XYButtonAction::Double);
    Serial.println("Sent double press");
  } else if (action == S3XYButtonAction::Long) {
    s3xy_send_long();
    led_status_action(S3XYButtonAction::Long);
    Serial.println("Sent long press");
  }

  return true;
}

static void processQueuedButtonActions() {
  S3XYButtonAction action;

  while (s3xy_stalk_input_dequeue_action(&action)) {
    lastTriggerTime = millis();
    autoStep = 0;
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
  s3xy_on_connect(onButtonConnected);
  s3xy_on_disconnect(onButtonDisconnected);
  s3xy_begin("ENH_BTN");

  // New central
  s3xy_stalk_on_notify(onStalkNotify);
  s3xy_stalk_begin();

  // LED manager
  led_status_begin();
}

void loop() {
  s3xy_loop();
  s3xy_stalk_loop();

  if (s3xy_ready() && s3xy_stalk_connected()) {

    if (Serial.available()) {
      S3XYButtonAction action;
      if (serialInputToAction(Serial.read(), &action)) {
        lastTriggerTime = millis();
        autoStep = 0;
        executeButtonAction(action);
      }
    }

    processQueuedButtonActions();
  }
}