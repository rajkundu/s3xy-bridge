#include <Arduino.h>
#include "S3XYButton.h"

const int LED_PIN = LED_BUILTIN;

unsigned long lastTriggerTime = 0;
int autoStep = 0;

void onConnected() {
  Serial.println("[user] connected");
  digitalWrite(LED_PIN, HIGH);
  lastTriggerTime = millis();
}

void onDisconnected() {
  Serial.println("[user] disconnected");
  digitalWrite(LED_PIN, LOW);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  s3xy_on_connect(onConnected);
  s3xy_on_disconnect(onDisconnected);

  s3xy_begin("ENH_BTN");
}

void loop() {
  s3xy_loop();

  if (s3xy_ready()) {
    char input = 0;

    // 1. Manual Serial input
    if (Serial.available()) {
      input = Serial.read();
      if (input == 's' || input == 'd' || input == 'l') {
        lastTriggerTime = millis(); // Reset timer on manual action
        autoStep = 0;               // Reset cycle back to single press
      }
    }
    
    // 2. Auto-trigger every 5s if idle
    else if (millis() - lastTriggerTime >= 5000) {
      char steps[] = {'s', 'd', 'l'};
      input = steps[autoStep % 3];
      autoStep++;
      lastTriggerTime = millis();
    }

    if (input == 's') {
      s3xy_send_single();

      // Short flash off
      digitalWrite(LED_PIN, LOW);
      delay(100);
      digitalWrite(LED_PIN, HIGH);

      Serial.println("Sent single press");
    } else if (input == 'd') {
      s3xy_send_double();

      // Short double-flash off
      digitalWrite(LED_PIN, LOW);
      delay(100);
      digitalWrite(LED_PIN, HIGH);
      delay(100);
      digitalWrite(LED_PIN, LOW);
      delay(100);
      digitalWrite(LED_PIN, HIGH);

      Serial.println("Sent double press");
    } else if (input == 'l') {
      s3xy_send_long();

      // Long-flash off
      digitalWrite(LED_PIN, LOW);
      delay(500);
      digitalWrite(LED_PIN, HIGH);

      Serial.println("Sent long press");
    }
  }
}