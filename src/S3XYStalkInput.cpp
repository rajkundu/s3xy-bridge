#include "S3XYStalkInput.h"

namespace {

const size_t ACTION_QUEUE_SIZE = 8;
const unsigned long LONG_PRESS_MS = 750;

S3XYButtonAction actionQueue[ACTION_QUEUE_SIZE];
size_t actionQueueHead = 0;
size_t actionQueueTail = 0;

unsigned long upPressedAt = 0;
unsigned long downPressedAt = 0;
unsigned long endCapPressedAt = 0;

struct StalkTrigger {
  uint32_t pressCode;
  uint32_t releaseCode;
  const char* label;
  unsigned long* pressTime;
  S3XYButtonAction pressAction;
  S3XYButtonAction longReleaseAction;
};

uint32_t parseStalkCode(const uint8_t* data, size_t len) {
  if (len == 0) {
    return 0;
  }

  if (len >= 3) {
    return ((uint32_t)data[0] << 16) |
           ((uint32_t)data[1] << 8) |
           (uint32_t)data[2];
  }

  if (len >= 2) {
    return ((uint32_t)data[0] << 8) | (uint32_t)data[1];
  }

  return data[0];
}

void handleTrigger(const StalkTrigger& trigger, uint32_t code) {
  if (code == trigger.pressCode) {
    *trigger.pressTime = millis();
    s3xy_stalk_input_queue_action(trigger.pressAction);
    Serial.printf("[stalk-input] %s pressed\n", trigger.label);
    return;
  }

  if (code != trigger.releaseCode) {
    return;
  }

  if (*trigger.pressTime == 0) {
    Serial.printf(
        "[stalk-input] %s release before press -> ignored\n",
        trigger.label
    );
    return;
  }

  unsigned long elapsed = millis() - *trigger.pressTime;
  if (elapsed >= LONG_PRESS_MS) {
    s3xy_stalk_input_queue_action(trigger.longReleaseAction);

    Serial.printf(
        "[stalk-input] %s long release (elapsed=%lu ms)\n",
        trigger.label,
        elapsed
    );
  } else {
    Serial.printf(
        "[stalk-input] %s short release -> ignored (elapsed=%lu ms)\n",
        trigger.label,
        elapsed
    );
  }

  *trigger.pressTime = 0;
}

}  // namespace

void s3xy_stalk_input_queue_action(S3XYButtonAction action) {
  size_t nextTail = (actionQueueTail + 1) % ACTION_QUEUE_SIZE;
  if (nextTail == actionQueueHead) {
    Serial.println("[stalk-input] action queue full -> dropping oldest event");
    actionQueueHead = (actionQueueHead + 1) % ACTION_QUEUE_SIZE;
  }

  actionQueue[actionQueueTail] = action;
  actionQueueTail = nextTail;
}

bool s3xy_stalk_input_dequeue_action(S3XYButtonAction* action) {
  if (actionQueueHead == actionQueueTail) {
    return false;
  }

  *action = actionQueue[actionQueueHead];
  actionQueueHead = (actionQueueHead + 1) % ACTION_QUEUE_SIZE;
  return true;
}

void s3xy_stalk_input_handle_notify(const uint8_t* data, size_t len) {
  Serial.print("[stalk-input] packet: ");
  for (size_t i = 0; i < len; ++i) {
    Serial.printf("%02X", data[i]);
    if (i + 1 < len) {
      Serial.print(" ");
    }
  }
  Serial.println();

  uint32_t code = parseStalkCode(data, len);
  static const StalkTrigger triggers[] = {
      {0xA101, 0xA100, "end-cap", &endCapPressedAt, S3XYButtonAction::Single, S3XYButtonAction::Single},
      {0xA201, 0xA200, "down", &downPressedAt, S3XYButtonAction::Double, S3XYButtonAction::Double},
      {0xA301, 0xA300, "up", &upPressedAt, S3XYButtonAction::Long, S3XYButtonAction::Long},
  };

  for (const auto& trigger : triggers) {
    handleTrigger(trigger, code);
  }
}