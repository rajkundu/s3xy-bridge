#include "NimBLEDevice.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compat.h"
#include "S3XYButton.h"
#include "S3XYDeviceManager.h"
#include "S3XYDeviceProfile.h"
#include "S3XYStalkInput.h"
#include "Led.h"

static const char* TAG = "main";

// Waveshare ESP32-C6-Zero Mini: onboard WS2812 RGB LED data is GPIO 8.
static constexpr gpio_num_t LED_PIN = GPIO_NUM_8;

Led led(LED_PIN, false, true);

// ----- Notification callbacks -----

static void onStalkNotify(const uint8_t* data, size_t len) {
  s3xy_stalk_input_handle_notify(data, len);
}

static void onButtonNotify(const uint8_t* data, size_t len) {
  ESP_LOGI("btn_notify", "RX %u bytes", (unsigned)len);
  ESP_LOG_BUFFER_HEX("btn_notify", data, len);
}

// ----- Device profiles -----

static const S3XYDeviceProfile PROFILES[] = {
  {"ENH_STLK",
   "00003D69-87D2-479E-7E45-8551415A6DE1",
   "00003D50-87D2-479E-7E45-8551415A6DE1",
   "00003D49-87D2-479E-7E45-8551415A6DE1",
   "stalk", onStalkNotify},

  {"ENH_BTN",
   "00003D46-87D2-479E-7E45-8551415A6DE1",
   "00003D50-87D2-479E-7E45-8551415A6DE1",
   "00003D49-87D2-479E-7E45-8551415A6DE1",
   "button", onButtonNotify},
};

static constexpr size_t PROFILE_COUNT =
    sizeof(PROFILES) / sizeof(PROFILES[0]);

S3XYDeviceManager manager(PROFILES, PROFILE_COUNT);

// ----- Virtual button callbacks -----

static void onCommanderConnected() {
  ESP_LOGI(TAG, "Commander connected");
}

static void onCommanderDisconnected() {
  ESP_LOGI(TAG, "Commander disconnected");
}

// ----- LED state machine -----

static void updateLed() {
  if (manager.isPairing()) {
    // Fast blink — pairing mode
    led.runLed({{0, true}, {100, false}}, 1000);
  } else if (manager.deviceCount() == 0) {
    // Solid OFF — no devices paired
    led.set(false);
  } else if (manager.allReady()) {
    // Solid ON — everything connected
    led.set(true);
  } else {
    // Slow double-blink — not all ready
    led.runLed({{0, true}, {100, false},
                {200, true}, {300, false}}, 1000);
  }
}

// ----- Main task -----

static void main_task(void* pvParameters) {
  // Init BLE stack
  NimBLEDevice::init("ENH_BTN");

  // Virtual button (peripheral)
  s3xy_on_connect(onCommanderConnected);
  s3xy_on_disconnect(onCommanderDisconnected);
  s3xy_begin("ENH_BTN");

  // Device manager (central + pairing button on GPIO 9)
  manager.begin(GPIO_NUM_9, 5000, 30000, 15000);

  // LED
  led.begin();

  ESP_LOGI(TAG, "S3XY Bridge started");

  // Main loop
  while (1) {
    s3xy_loop();
    manager.loop();
    updateLed();

    // Process stalk input actions → forward to virtual button
    S3XYButtonAction action;
    while (s3xy_stalk_input_dequeue_action(&action)) {
      if (s3xy_ready()) {
        if (action == S3XYButtonAction::Single) {
          s3xy_send_single();
          ESP_LOGI(TAG, "Forwarded single press");
        } else if (action == S3XYButtonAction::Double) {
          s3xy_send_double();
          ESP_LOGI(TAG, "Forwarded double press");
        } else if (action == S3XYButtonAction::Long) {
          s3xy_send_long();
          ESP_LOGI(TAG, "Forwarded long press");
        }
      }
    }

    delay_ms(10);  // yield to FreeRTOS
  }
}

extern "C" void app_main(void) {
  // Initialize NVS (required before any nvs_open or NimBLE calls)
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "Erasing NVS flash...");
    nvs_flash_erase();
    nvs_flash_init();
  }

  xTaskCreate(main_task, "main_task", 8192, nullptr, 5, nullptr);
}
