#include "Led.h"
#include <esp_log.h>

namespace {

constexpr bool LED_DEBUG = false;
static const char* TAG = "LED";
constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;

static rmt_symbol_word_t makeSymbol(uint16_t duration0, uint16_t duration1,
                  uint32_t level0, uint32_t level1) {
  rmt_symbol_word_t symbol = {};
  symbol.level0 = level0;
  symbol.duration0 = duration0;
  symbol.level1 = level1;
  symbol.duration1 = duration1;
  return symbol;
}

static const rmt_symbol_word_t WS2812_ZERO = makeSymbol(3, 9, 1, 0);
static const rmt_symbol_word_t WS2812_ONE = makeSymbol(9, 3, 1, 0);
static const rmt_symbol_word_t WS2812_RESET = makeSymbol(250, 250, 0, 0);

static size_t encodeWs2812(const void* data, size_t dataSize,
                           size_t symbolsWritten, size_t symbolsFree,
                           rmt_symbol_word_t* symbols, bool* done, void*) {
  if (symbolsFree < 8) return 0;
  size_t byteIndex = symbolsWritten / 8;
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (byteIndex < dataSize) {
    for (int bit = 0; bit < 8; ++bit) {
      symbols[bit] = (bytes[byteIndex] & (0x80 >> bit))
                         ? WS2812_ONE : WS2812_ZERO;
    }
    return 8;
  }
  symbols[0] = WS2812_RESET;
  *done = true;
  return 1;
}

}  // namespace

Led::Led(gpio_num_t pin, bool activeLow, bool addressable)
    : pin_(pin), activeLow_(activeLow), addressable_(addressable) {}

void Led::begin() {
  if (addressable_) {
    rmt_tx_channel_config_t channelConfig = {};
    channelConfig.clk_src = RMT_CLK_SRC_DEFAULT;
    channelConfig.gpio_num = pin_;
    channelConfig.mem_block_symbols = 64;
    channelConfig.resolution_hz = RMT_RESOLUTION_HZ;
    channelConfig.trans_queue_depth = 1;

    esp_err_t result = rmt_new_tx_channel(&channelConfig, &rmtChannel_);
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(result));
      return;
    }

    rmt_simple_encoder_config_t encoderConfig = {};
    encoderConfig.callback = encodeWs2812;
    result = rmt_new_simple_encoder(&encoderConfig, &rmtEncoder_);
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "rmt_new_simple_encoder failed: %s", esp_err_to_name(result));
      return;
    }

    result = rmt_enable(rmtChannel_);
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(result));
      return;
    }

    ESP_LOGI(TAG, "started addressable LED on GPIO %d", pin_);
    write(true);
  } else {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    esp_err_t gpioResult = gpio_config(&io_conf);
    if (gpioResult != ESP_OK) {
      ESP_LOGE(TAG, "gpio_config(%d) failed: %s", pin_,
               esp_err_to_name(gpioResult));
      return;
    }

    set(false);
  }

  esp_timer_create_args_t timer_args = {};
  timer_args.callback = &Led::timer_callback;
  timer_args.arg = this;
  timer_args.name = "led_timer";
  esp_err_t timerResult = esp_timer_create(&timer_args, &ticker_);
  if (timerResult != ESP_OK) {
    ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(timerResult));
    return;
  }

  timerResult = esp_timer_start_periodic(ticker_, UPDATE_INTERVAL_MS * 1000);
  if (timerResult != ESP_OK) {
    ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s",
             esp_err_to_name(timerResult));
    return;
  }

  ESP_LOGI(TAG, "started LED on GPIO %d (%s)", pin_,
           addressable_ ? "addressable" :
           (activeLow_ ? "active-low" : "active-high"));

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

  if (cycleDuration > 0) {
    repeatingStates_.assign(pattern.begin(), pattern.end());
    repeatingCycleDuration_ = cycleDuration;
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
  repeatingCycleDuration_ = 0;
  repeatingStates_.clear();
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
  if (repeatingCycleDuration_ > 0 && !repeatingStates_.empty()) {
    states_ = repeatingStates_;
    stepCount_ = states_.size();
    currentStep_ = 0;
    patternStartedAt_ = millis();
    cycleDuration_ = repeatingCycleDuration_;
    running_ = true;
    if (LED_DEBUG) {
      ESP_LOGD(TAG, "reinstating repeating pattern");
    }
    update();
    return;
  }

  if (LED_DEBUG) {
    ESP_LOGD(TAG, "pattern finished");
  }
}

void Led::write(bool on) {
  if (addressable_) {
    writeAddressable(on);
    return;
  }
  int level = on ? 1 : 0;
  if (activeLow_) level = !level;
  esp_err_t result = gpio_set_level(pin_, level);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "gpio_set_level(%d) failed: %s", pin_,
             esp_err_to_name(result));
  }
}

void Led::writeAddressable(bool on) {
  if (hasAddressableState_ && lastAddressableState_ == on) return;
  hasAddressableState_ = true;
  lastAddressableState_ = on;

  // This board's onboard pixel responds as RGB. Use dim white for on.
  uint8_t pixel[3] = {static_cast<uint8_t>(on ? 16 : 0),
                      static_cast<uint8_t>(on ? 16 : 0),
                      static_cast<uint8_t>(on ? 16 : 0)};
  rmt_transmit_config_t transmitConfig = {};
  esp_err_t result = rmt_transmit(rmtChannel_, rmtEncoder_, pixel,
                                  sizeof(pixel), &transmitConfig);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "addressable LED write failed: %s", esp_err_to_name(result));
  }
}
