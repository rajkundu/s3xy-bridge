#include "Led.h"
#include <esp_log.h>

namespace {

constexpr bool LED_DEBUG = false;
static const char* TAG = "LED";

}  // namespace

Led::Led(gpio_num_t pin) : pin_(pin) {}

void Led::begin() {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << pin_);
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  set(false);

  esp_timer_create_args_t timer_args = {};
  timer_args.callback = &Led::timer_callback;
  timer_args.arg = this;
  timer_args.name = "led_timer";
  esp_timer_create(&timer_args, &ticker_);
  esp_timer_start_periodic(ticker_, UPDATE_INTERVAL_MS * 1000);

  if (LED_DEBUG) {
    ESP_LOGD(TAG, "started on pin %d", pin_);
  }
}

void Led::timer_callback(void* arg) {
  Led* led = static_cast<Led*>(arg);
  led->update();
}

void Led::runLed(const LedPattern& pattern, unsigned long cycleDuration) {
  if (pattern.size() == 0) {
    if (LED_DEBUG) {
      ESP_LOGD(TAG, "ignored empty pattern");
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
    ESP_LOGD(TAG, "starting pattern (%u states, cycle %lu ms)",
             static_cast<unsigned>(pattern.size()), cycleDuration);
  }
  update();
}

void Led::set(bool on) {
  running_ = false;
  if (LED_DEBUG) {
    ESP_LOGD(TAG, "set %s", on ? "on" : "off");
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
        ESP_LOGD(TAG, "repeating pattern");
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
      ESP_LOGD(TAG, "keyframe %lu ms -> %s",
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
    ESP_LOGD(TAG, "pattern finished");
  }
}

void Led::write(bool on) {
  gpio_set_level(pin_, on ? 1 : 0);
}
