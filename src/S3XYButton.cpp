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
    NimBLEAddress addr = connInfo.getAddress();
    bool nvsBonded = NimBLEDevice::isBonded(addr);
    ESP_LOGI(TAG, "Central connected: %s (handle=%u, interval=%.2f ms, latency=%u, timeout=%u ms, encrypted=%u, bonded=%u, nvsBonded=%s)",
             addr.toString().c_str(),
             connInfo.getConnHandle(),
             connInfo.getConnInterval() * 1.25f,
             connInfo.getConnLatency(),
             connInfo.getConnTimeout() * 10,
             connInfo.isEncrypted(),
             connInfo.isBonded(),
             nvsBonded ? "YES" : "NO");

    if (!connInfo.isEncrypted()) {
      int rc = 0;
      bool ok = NimBLEDevice::startSecurity(connInfo.getConnHandle(), &rc);
      ESP_LOGI(TAG, "Initiated security request for connHandle=%u: %s (rc=%d)",
               connInfo.getConnHandle(), ok ? "OK" : "FAIL", rc);
    }

    const uint8_t poke = 0x00;
    notifyBytes(&poke, 1);
    if (g_onConnect) g_onConnect();
  }

  uint32_t onPassKeyDisplay() override {
    ESP_LOGI(TAG, "[smp] PassKey display requested -> 0");
    return 0;
  }

  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    ESP_LOGI(TAG, "[smp] Confirm passkey requested for %s: %06lu (auto-confirming)",
             connInfo.getAddress().toString().c_str(), (unsigned long)pin);
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "[smp] Passkey entry requested for %s",
             connInfo.getAddress().toString().c_str());
  }

  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "MTU changed to %u for %s",
             MTU, connInfo.getAddress().toString().c_str());
  }

  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "Conn params updated for %s: interval=%.2f ms, latency=%u, timeout=%u ms",
             connInfo.getAddress().toString().c_str(),
             connInfo.getConnInterval() * 1.25f,
             connInfo.getConnLatency(),
             connInfo.getConnTimeout() * 10);
  }

  void onIdentity(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "[smp] Peer identity resolved: %s (id: %s)",
             connInfo.getAddress().toString().c_str(),
             connInfo.getIdAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    g_connected = false;
    g_subscribed = false;
    ESP_LOGI(TAG, "Disconnected from %s, reason: %d (%s)",
             connInfo.getAddress().toString().c_str(),
             reason,
             NimBLEUtils::returnCodeToString(reason));
    if (g_onDisconnect) g_onDisconnect();
    NimBLEDevice::startAdvertising();
    ESP_LOGI(TAG, "Resumed advertising as ENH_BTN");
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "[smp] Authentication complete for %s: bonded=%u encrypted=%u authenticated=%u keySize=%u",
             connInfo.getAddress().toString().c_str(),
             connInfo.isBonded(),
             connInfo.isEncrypted(),
             connInfo.isAuthenticated(),
             connInfo.getSecKeySize());
    if (connInfo.isBonded()) {
        ESP_LOGI(TAG, "Bonded successfully with Commander");
    } else {
        ESP_LOGW(TAG, "Bonding not established with Commander");
    }
  }
};

class NotifyCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    // subValue: 0 = Unsubscribed, 1 = Notifications enabled, 2 = Indications enabled
    g_subscribed = (subValue == 1);
    ESP_LOGI(TAG, "Notifications %s by %s (subValue=%u)",
             g_subscribed ? "ENABLED" : "DISABLED",
             connInfo.getAddress().toString().c_str(),
             subValue);
  }
};

class IDCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    ESP_LOGI(TAG, "ID read by %s", connInfo.getAddress().toString().c_str());
  }

  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    auto val = c->getValue();
    const uint8_t* data = val.data();
    size_t len = val.length();

    ESP_LOGI(TAG, "ID write from %s (%u bytes)", connInfo.getAddress().toString().c_str(), (unsigned)len);
    ESP_LOG_BUFFER_HEX(TAG, data, len);

    if (len == 1 && data[0] == 0xB6) {  // Set mode ?
      const uint8_t resp[] = {0xC7, 0x00, 0x01};
      notifyBytes(resp, sizeof(resp));
      ESP_LOGI(TAG, "Commander initialized success (sent C7 00 01)");
    } else if (len == 1 && data[0] == 0xA1) {  // Disconnect request
      ESP_LOGI(TAG, "Commander requested unpair -> disconnect");
      g_server->disconnect(connInfo.getConnHandle());
    } else if (len == 4 && data[0] == 0xA4) {  // Rename request
      const uint8_t resp[] = {0xA4, 0x00, data[1], data[2]};
      notifyBytes(resp, sizeof(resp));
      ESP_LOGI(TAG, "Commander rename request handled");
    }
  }
};

// ---- Public API ----
void s3xy_begin(const char* deviceName) {
  NimBLEDevice::setSecurityAuth(true, false, true); // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

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

  NimBLEAdvertisementData advData;
  advData.setAppearance(0x0000);
  advData.setFlags(0x06);
  advData.setName(deviceName ? deviceName : "ENH_BTN");

  NimBLEAdvertisementData scanResp;
  scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));

  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanResp);
  adv->enableScanResponse(true);

  // Fast advertising interval: 20 ms (32) to 40 ms (64) in 0.625ms units.
  // Standard BLE accessory fast-connection interval so Commander discovers
  // and connects quickly rather than waiting for slow advertising cycles.
  adv->setMinInterval(32);
  adv->setMaxInterval(64);
  adv->start();

  ESP_LOGI(TAG, "Advertising as spoofed S3XY button '%s' (interval 20-40 ms, bonds stored: %d)",
           deviceName ? deviceName : "ENH_BTN",
           NimBLEDevice::getNumBonds());
}

void s3xy_loop() {
  static uint32_t lastHeartbeat = 0;
  uint32_t now = millis();
  if (now - lastHeartbeat >= 10000) {
    lastHeartbeat = now;
    if (g_connected) {
      ESP_LOGI(TAG, "Heartbeat: CONNECTED to Commander (notifications=%s, ready=%s)",
               g_subscribed ? "ENABLED" : "AWAITING_SUB",
               s3xy_ready() ? "YES" : "NO");
    } else {
      NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
      bool active = adv && adv->isAdvertising();
      ESP_LOGI(TAG, "Heartbeat: ADVERTISING (active=%s, bonds=%d, waiting for Commander)",
               active ? "YES" : "NO",
               NimBLEDevice::getNumBonds());
    }
  }
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
