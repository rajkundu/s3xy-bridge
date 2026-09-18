#pragma once

#include <Arduino.h>
#include <Ticker.h>
#include <initializer_list>
#include <vector>

struct LedState {
  unsigned long timestamp;
  bool on;
};

using LedPattern = std::initializer_list<LedState>;

class Led {
 public:
  explicit Led(uint8_t pin);
  void begin();
  void runLed(const LedPattern& pattern, unsigned long cycleDuration);
  void set(bool on);
  bool isRunning() const;
  bool isRepeating() const;

 private:
  static constexpr unsigned long UPDATE_INTERVAL_MS = 25;

  void update();
  void finishPattern();
  void write(bool on);

  uint8_t pin_;
  std::vector<LedState> states_;
  size_t stepCount_ = 0;
  size_t currentStep_ = 0;
  unsigned long patternStartedAt_ = 0;
  unsigned long cycleDuration_ = 0;
  bool running_ = false;
  Ticker ticker_;
};
