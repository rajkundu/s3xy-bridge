#include "S3XYButton.h"

#include <cstring>
#include "NimBLEDevice.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "compat.h"

static const char* TAG = "s3xy_btn";

// ---- UUIDs ----
static const char* SERVICE_UUID = "00003d46-87d2-479e-7e45-8551415a6de1";
static const char* CHAR_NOTIFY_UUID = "00003d50-87d2-479e-7e45-8551415a6de1";
static const char* CHAR_ID_UUID = "00003d49-87d2-479e-7e45-8551415a6de1";

// ---- State (internal) ----
static NimBLEServer* g_server = nullptr;
static NimBLECharacteristic* g_notify = nullptr;
static NimBLECharacteristic* g_id = nullptr;
static bool g_connected = false;
static bool g_subscribed = false;

// Default ID (edit via s3xy_set_id)
static uint8_t g_buttonId[10] = {'S', 'A', 'B', 'R', 'I', 'N', 'A', 'S', 'N', 'S'};

// User hooks
static s3xy_cb_t g_onConnect = nullptr;
static s3xy_cb_t g_onDisconnect = nullptr;

// ---- Helpers ----
static inline void notifyBytes(const uint8_t* data, size_t len) {
  if (!g_subscribed || !g_notify) return;
  g_notify->setValue((uint8_t*)data, len);
  g_notify->notify();
}

// ---- Callbacks for BLE (internal) ----
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    g_connected = true;
    ESP_LOGI(TAG, "Connected");
    const uint8_t poke = 0x00;
    notifyBytes(&poke, 1);
    if (g_onConnect) g_onConnect();
  }
  
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    g_connected = false;
    g_subscribed = false;
    ESP_LOGI(TAG, "Disconnected, reason: %d", reason);
    if (g_onDisconnect) g_onDisconnect();
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "[ble] authentication complete: bonded=%u encrypted=%u authenticated=%u",
             connInfo.isBonded(),
             connInfo.isEncrypted(),
             connInfo.isAuthenticated());
    if (connInfo.isBonded()) {
        ESP_LOGI(TAG, "Bonded successfully");
    } else {
        ESP_LOGI(TAG, "Bond failed");
    }
  }
};

class NotifyCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    // subValue: 0 = Unsubscribed, 1 = Notifications enabled, 2 = Indications enabled
    g_subscribed = (subValue == 1);
    ESP_LOGI(TAG, "%s", g_subscribed ? "Notifications enabled" : "Notifications disabled");
  }
};

class IDCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "ID read");
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    auto val = c->getValue();
    const uint8_t* data = val.data();
    size_t len = val.length();

    ESP_LOGI(TAG, "ID write (%u bytes)", (unsigned)len);
    ESP_LOG_BUFFER_HEX(TAG, data, len);

    if (len == 1 && data[0] == 0xB6) {  // Set mode ?
      const uint8_t resp[] = {0xC7, 0x00, 0x01};
      notifyBytes(resp, sizeof(resp));
      ESP_LOGI(TAG, "Commander initialized success");
    } else if (len == 1 && data[0] == 0xA1) {  // Disconnect request
      ESP_LOGI(TAG, "Unpair -> disconnect");
      g_server->disconnect(connInfo.getConnHandle());
    } else if (len == 4 && data[0] == 0xA4) {  // Rename request
      const uint8_t resp[] = {0xA4, 0x00, data[1], data[2]};
      notifyBytes(resp, sizeof(resp));
      ESP_LOGI(TAG, "Rename handled");
    }
  }
};

// ---- Public API ----
void s3xy_begin(const char* deviceName) {
  NimBLEDevice::setSecurityAuth(true, false, true); // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  NimBLEService* svc = g_server->createService(SERVICE_UUID);

  g_notify = svc->createCharacteristic(CHAR_NOTIFY_UUID,
                                       NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  g_notify->setCallbacks(new NotifyCB());
  g_notify->setValue((const uint8_t*)"\x00", 1);

  g_id = svc->createCharacteristic(CHAR_ID_UUID, 
    NIMBLE_PROPERTY::READ | 
    NIMBLE_PROPERTY::WRITE | 
    NIMBLE_PROPERTY::READ_ENC | 
    NIMBLE_PROPERTY::WRITE_ENC
  );
  g_id->setValue(g_buttonId, sizeof(g_buttonId));
  g_id->setCallbacks(new IDCB());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(deviceName ? deviceName : "ENH_BTN");
  adv->addServiceUUID(SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();

  ESP_LOGI(TAG, "Advertising as spoofed S3XY button");
}

void s3xy_loop() {
  // no periodic work needed
}

bool s3xy_ready() {
  return g_connected && g_subscribed;
}

void s3xy_send_single() {
  if (!s3xy_ready()) return;
  const uint8_t p1[] = {0x01};
  const uint8_t p0[] = {0x00};
  const uint8_t pC1[] = {0xC1, 0x01};
  notifyBytes(p1, sizeof(p1));
  vTaskDelay(pdMS_TO_TICKS(5));
  notifyBytes(p0, sizeof(p0));
  vTaskDelay(pdMS_TO_TICKS(5));
  notifyBytes(pC1, sizeof(pC1));
  vTaskDelay(pdMS_TO_TICKS(5));
  notifyBytes(p0, sizeof(p0));
  vTaskDelay(pdMS_TO_TICKS(5));
}

void s3xy_send_long() {
  if (!s3xy_ready()) return;
  const uint8_t p[] = {0xC3, 0x01};
  notifyBytes(p, sizeof(p));
  vTaskDelay(pdMS_TO_TICKS(5));
}

void s3xy_send_double() {
  if (!s3xy_ready()) return;
  const uint8_t p[] = {0xC1, 0x02};
  notifyBytes(p, sizeof(p));
  vTaskDelay(pdMS_TO_TICKS(5));
}

void s3xy_set_id(const uint8_t id[10]) {
  memcpy(g_buttonId, id, 10);
  if (g_id) g_id->setValue(g_buttonId, sizeof(g_buttonId));
}

// --- Hook setters ---
void s3xy_on_connect(s3xy_cb_t cb) {
  g_onConnect = cb;
}
void s3xy_on_disconnect(s3xy_cb_t cb) {
  g_onDisconnect = cb;
}
