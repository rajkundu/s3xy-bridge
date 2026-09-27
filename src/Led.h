#pragma once

#include <initializer_list>
#include <vector>
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <esp_timer.h>
#include "compat.h"

struct LedState {
  unsigned long timestamp;
  bool on;
};

using LedPattern = std::initializer_list<LedState>;

class Led {
 public:
  explicit Led(gpio_num_t pin, bool activeLow = false,
               bool addressable = false);
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
  void writeAddressable(bool on);

  gpio_num_t pin_;
  bool activeLow_;
  bool addressable_;
  bool hasAddressableState_ = false;
  bool lastAddressableState_ = false;
  rmt_channel_handle_t rmtChannel_ = nullptr;
  rmt_encoder_handle_t rmtEncoder_ = nullptr;
  std::vector<LedState> states_;
  size_t stepCount_ = 0;
  size_t currentStep_ = 0;
  unsigned long patternStartedAt_ = 0;
  unsigned long cycleDuration_ = 0;
  bool running_ = false;
  esp_timer_handle_t ticker_ = nullptr;
};
