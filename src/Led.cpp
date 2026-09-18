#include "Led.h"

namespace {

constexpr bool LED_DEBUG = false;

}  // namespace

Led::Led(uint8_t pin) : pin_(pin) {}

void Led::begin() {
  pinMode(pin_, OUTPUT);
  set(false);
  ticker_.attach_ms(UPDATE_INTERVAL_MS, [this]() { update(); });
  if (LED_DEBUG) {
    Serial.printf("[led] started on pin %u\n", pin_);
  }
}

void Led::runLed(const LedPattern& pattern, unsigned long cycleDuration) {
  if (pattern.size() == 0) {
    if (LED_DEBUG) {
      Serial.println("[led] ignored empty pattern");
    }
    return;
  }

  bool samePattern = running_ && cycleDuration_ == cycleDuration &&
                     states_.size() == pattern.size();
  if (samePattern) {
    size_t index = 0;
    for (const LedState& state : pattern) {
      if (states_[index].timestamp != state.timestamp ||
          states_[index].on != state.on) {
        samePattern = false;
        break;
      }
      ++index;
    }
  }

  if (samePattern) {
    return;
  }

  states_.assign(pattern.begin(), pattern.end());
  stepCount_ = states_.size();
  currentStep_ = 0;
  patternStartedAt_ = millis();
  cycleDuration_ = cycleDuration;
  running_ = true;
  if (LED_DEBUG) {
    Serial.printf("[led] starting pattern (%u states, cycle %lu ms)\n",
                  static_cast<unsigned>(pattern.size()), cycleDuration);
  }
  update();
}

void Led::set(bool on) {
  running_ = false;
  if (LED_DEBUG) {
    Serial.printf("[led] set %s\n", on ? "on" : "off");
  }
  write(on);
}

bool Led::isRunning() const {
  return running_;
}

bool Led::isRepeating() const {
  return running_ && cycleDuration_ > 0;
}

void Led::update() {
  if (!running_) {
    return;
  }

  while (running_) {
    if (cycleDuration_ > 0 &&
        millis() - patternStartedAt_ >= cycleDuration_) {
      currentStep_ = 0;
      patternStartedAt_ = millis();
      if (LED_DEBUG) {
        Serial.println("[led] repeating pattern");
      }
    }

    unsigned long elapsed = millis() - patternStartedAt_;
    if (currentStep_ >= stepCount_) {
      if (cycleDuration_ == 0) {
        finishPattern();
      }
      return;
    }

    if (elapsed < states_[currentStep_].timestamp) {
      return;
    }

    write(states_[currentStep_].on);
    if (LED_DEBUG) {
      Serial.printf("[led] keyframe %u ms -> %s\n",
                    states_[currentStep_].timestamp,
                    states_[currentStep_].on ? "on" : "off");
    }
    ++currentStep_;

    if (currentStep_ < stepCount_) {
      continue;
    }

    if (cycleDuration_ == 0) {
      finishPattern();
      return;
    }

    return;
  }
}

void Led::finishPattern() {
  running_ = false;
  if (LED_DEBUG) {
    Serial.println("[led] pattern finished");
  }
}

void Led::write(bool on) {
  digitalWrite(pin_, on ? HIGH : LOW);
}
