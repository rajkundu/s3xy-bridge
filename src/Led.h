#pragma once

#include <initializer_list>
#include <vector>
#include <driver/gpio.h>
#include <esp_timer.h>
#include "compat.h"

struct LedState {
  unsigned long timestamp;
  bool on;
};

using LedPattern = std::initializer_list<LedState>;

class Led {
 public:
  explicit Led(gpio_num_t pin);
  void begin();
  void runLed(const LedPattern& pattern, unsigned long cycleDuration);
  void set(bool on);
  bool isRunning() const;
  bool isRepeating() const;

 private:
  static constexpr unsigned long UPDATE_INTERVAL_MS = 25;

  static void timer_callback(void* arg);
  void update();
  void finishPattern();
  void write(bool on);

  gpio_num_t pin_;
  std::vector<LedState> states_;
  size_t stepCount_ = 0;
  size_t currentStep_ = 0;
  unsigned long patternStartedAt_ = 0;
  unsigned long cycleDuration_ = 0;
  bool running_ = false;
  esp_timer_handle_t ticker_ = nullptr;
};
