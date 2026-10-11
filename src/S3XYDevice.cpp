#include "S3XYDevice.h"
#include "S3XYDeviceManager.h"

#include "compat.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <cstdlib>
#include <map>

// ============================================================
// Static notification dispatch & concurrency locks
// ============================================================

static std::map<NimBLERemoteCharacteristic*, S3XYDevice*> s_notifyOwners;
static portMUX_TYPE s_notifyMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_deviceMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================
// Utility
// ============================================================

static void printHex(const char* tag, const uint8_t* data, size_t len, bool isNotify) {
  char buf[128];
  size_t pos = 0;
  for (size_t i = 0; i < len && pos < sizeof(buf) - 4; ++i) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
  }
  ESP_LOGI(tag, "RX %s: %s", isNotify ? "notify" : "indicate", buf);
}

// ============================================================
// Notification trampoline
// ============================================================

void S3XYDevice::notifyTrampoline(
    NimBLERemoteCharacteristic* characteristic,
    uint8_t* data,
    size_t length,
    bool isNotify) {
  S3XYDevice* self = nullptr;
  portENTER_CRITICAL(&s_notifyMux);
  auto it = s_notifyOwners.find(characteristic);
  if (it != s_notifyOwners.end()) {
    self = it->second;
  }
  portEXIT_CRITICAL(&s_notifyMux);

  if (!self) return;

  printHex(self->tag_, data, length, isNotify);

  if (self->notifyCallback_) {
    self->notifyCallback_(*self, data, length);
  }
}

// ============================================================
// Client connection callbacks
// ============================================================

class DeviceClientCallbacks : public NimBLEClientCallbacks {
 public:
  explicit DeviceClientCallbacks(S3XYDevice* owner) : owner_(owner) {}

  void onConnect(NimBLEClient* pClient) override {
    owner_->linkEstablished_ = true;

    NimBLEConnInfo info = owner_->client_->getConnInfo();
    uint32_t elapsed = millis() - owner_->connectStartTime_;
    ESP_LOGI(owner_->tag_,
             "Connection established after %lu ms: interval=%.2f ms latency=%u timeout=%u ms; "
             "waiting for security/authentication",
             elapsed,
             info.getConnInterval() * 1.25f, info.getConnLatency(),
             info.getConnTimeout() * 10);
  }

  bool onConnParamsUpdateRequest(
      NimBLEClient*, const ble_gap_upd_params* params) override {
    ESP_LOGI(owner_->tag_,
             "Connection parameter update requested: interval=%u-%u ms "
             "latency=%u timeout=%u ms",
             static_cast<unsigned>(params->itvl_min * 1.25f),
             static_cast<unsigned>(params->itvl_max * 1.25f),
             params->latency, params->supervision_timeout * 10);
    return true;
  }

  void onConnectFail(NimBLEClient*, int reason) override {
    uint32_t elapsed = millis() - owner_->connectStartTime_;
    owner_->connectionPending_ = false;
    owner_->linkEstablished_ = false;

    NimBLEAdvertisedDevice* targetToDel = nullptr;
    portENTER_CRITICAL(&s_deviceMux);
    targetToDel = owner_->targetDevice_;
    owner_->targetDevice_ = nullptr;
    portEXIT_CRITICAL(&s_deviceMux);
    delete targetToDel;

    owner_->nextScanTime_ = millis() + 500;
    ESP_LOGW(owner_->tag_,
             "Connection attempt timed out/failed after %lu ms, reason: %d (will resume scanning)",
             elapsed, reason);
  }

  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(owner_->tag_, "Authorization requested: passkey entry for %s",
             connInfo.getAddress().toString().c_str());
  }

  uint32_t onPassKeyDisplay(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(owner_->tag_, "Authorization requested: passkey display for %s",
             connInfo.getAddress().toString().c_str());
    return 0;
  }

  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    ESP_LOGI(owner_->tag_, "Authorization requested: confirm passkey %06lu for %s",
             static_cast<unsigned long>(pin),
             connInfo.getAddress().toString().c_str());
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    ESP_LOGI(owner_->tag_,
             "Authentication complete: bonded=%d encrypted=%d authenticated=%d keySize=%u",
             connInfo.isBonded(), connInfo.isEncrypted(),
             connInfo.isAuthenticated(), connInfo.getSecKeySize());
  }

  void onDisconnect(NimBLEClient*, int reason) override {
    ESP_LOGI(owner_->tag_, "Disconnected, reason: %d", reason);

    owner_->connected_ = false;
    owner_->connectionPending_ = false;
    owner_->linkEstablished_ = false;
    owner_->notificationsSubscribed_ = false;
    owner_->service_ = nullptr;

    if (owner_->notifyChar_) {
      portENTER_CRITICAL(&s_notifyMux);
      s_notifyOwners.erase(owner_->notifyChar_);
      portEXIT_CRITICAL(&s_notifyMux);
      owner_->notifyChar_ = nullptr;
    }
    owner_->idChar_ = nullptr;

    NimBLEAdvertisedDevice* targetToDel = nullptr;
    portENTER_CRITICAL(&s_deviceMux);
    targetToDel = owner_->targetDevice_;
    owner_->targetDevice_ = nullptr;
    portEXIT_CRITICAL(&s_deviceMux);
    delete targetToDel;

    owner_->nextScanTime_ = millis() + 100;

    S3XYDeviceManager* mgr = S3XYDeviceManager::instance();
    if (mgr) {
      mgr->onDeviceDisconnected(owner_);
    }
  }

 private:
  S3XYDevice* owner_;
};

// ============================================================
// Construction / destruction
// ============================================================

S3XYDevice::S3XYDevice(const char* tag,
                       const char* bleName,
                       const char* serviceUUID,
                       const char* notifyUUID,
                       const char* idUUID)
    : tag_(tag),
      bleName_(bleName),
      serviceUUID_(serviceUUID),
      notifyUUID_(notifyUUID),
      idUUID_(idUUID) {}

S3XYDevice::~S3XYDevice() {
  NimBLEAdvertisedDevice* targetToDel = nullptr;
  portENTER_CRITICAL(&s_deviceMux);
  targetToDel = targetDevice_;
  targetDevice_ = nullptr;
  portEXIT_CRITICAL(&s_deviceMux);
  delete targetToDel;

  cleanupClient();
}

// ============================================================
// Internal helpers
// ============================================================

void S3XYDevice::setTargetDevice(const NimBLEAdvertisedDevice& device) {
  NimBLEAdvertisedDevice* copy = new NimBLEAdvertisedDevice(device);
  NimBLEAdvertisedDevice* old = nullptr;
  portENTER_CRITICAL(&s_deviceMux);
  old = targetDevice_;
  targetDevice_ = copy;
  nextScanTime_ = 0;
  portEXIT_CRITICAL(&s_deviceMux);
  delete old;
}

void S3XYDevice::attachClient(NimBLEClient* client) {
  cleanupClient();
  client_ = client;
  client_->setSelfDelete(false, false);
  client_->setClientCallbacks(new DeviceClientCallbacks(this), true);
  linkEstablished_ = true;
  connectionPending_ = true;
  connectStartTime_ = millis();
  saveMac(client_->getPeerAddress());

  NimBLEConnInfo info = client_->getConnInfo();
  ESP_LOGI(tag_,
           "Client attached via whitelist: interval=%.2f ms latency=%u timeout=%u ms",
           info.getConnInterval() * 1.25f, info.getConnLatency(),
           info.getConnTimeout() * 10);
}

void S3XYDevice::cleanupClient() {
  if (notifyChar_) {
    portENTER_CRITICAL(&s_notifyMux);
    s_notifyOwners.erase(notifyChar_);
    portEXIT_CRITICAL(&s_notifyMux);
    notifyChar_ = nullptr;
  }

  if (client_) {
    if (client_->isConnected()) {
      client_->disconnect();
    }

    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  service_ = nullptr;
  notifyChar_ = nullptr;
  idChar_ = nullptr;
  connected_ = false;
  notificationsSubscribed_ = false;
  connectionPending_ = false;
  linkEstablished_ = false;
}

void S3XYDevice::loadSavedMac() {
  nvs_handle_t handle;
  if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
    size_t len = 0;
    if (nvs_get_str(handle, prefsKey_.c_str(), nullptr, &len) == ESP_OK && len > 0) {
      char* buf = new char[len];
      nvs_get_str(handle, prefsKey_.c_str(), buf, &len);
      savedMac_ = buf;
      delete[] buf;
    }
    nvs_close(handle);
  }

  if (!savedMac_.empty()) {
    ESP_LOGI(tag_, "Loaded saved MAC: %s", savedMac_.c_str());
  }
}

void S3XYDevice::saveMac(const NimBLEAddress& address) {
  std::string mac = address.toString();

  if (mac.empty() || mac == savedMac_) {
    return;
  }

  savedMac_ = mac;

  nvs_handle_t handle;
  if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_str(handle, prefsKey_.c_str(), savedMac_.c_str());
    nvs_commit(handle);
    nvs_close(handle);
  }

  ESP_LOGI(tag_, "Saved MAC: %s", savedMac_.c_str());
}

void S3XYDevice::clearSavedMac() {
  savedMac_ = "";
  NimBLEAdvertisedDevice* targetToDel = nullptr;
  portENTER_CRITICAL(&s_deviceMux);
  targetToDel = targetDevice_;
  targetDevice_ = nullptr;
  portEXIT_CRITICAL(&s_deviceMux);
  delete targetToDel;

  nvs_handle_t handle;
  if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
    nvs_erase_key(handle, prefsKey_.c_str());
    nvs_commit(handle);
    nvs_close(handle);
  }

  ESP_LOGI(tag_, "Cleared saved MAC");
}

// ============================================================
// Connect and subscribe
// ============================================================

bool S3XYDevice::connectToDevice() {
  uint32_t stageStartedAt = millis();
  if (connectionPending_) {
    if (!linkEstablished_) return true;
    connectionPending_ = false;
    ESP_LOGI(tag_, "GATT link ready; starting service discovery");
  } else {
    NimBLEAdvertisedDevice* device = nullptr;
    portENTER_CRITICAL(&s_deviceMux);
    device = targetDevice_;
    targetDevice_ = nullptr;
    portEXIT_CRITICAL(&s_deviceMux);

    if (!device) {
      return false;
    }

    cleanupClient();

    if (NimBLEDevice::getScan()->isScanning()) {
      NimBLEDevice::getScan()->stop();
    }

    stageStartedAt = millis();
    client_ = NimBLEDevice::createClient();
    ESP_LOGI(tag_, "create client took %lu ms", millis() - stageStartedAt);

    if (!client_) {
      ESP_LOGE(tag_, "Failed to create BLE client");
      delete device;
      return false;
    }

    // 15 ms initial connection interval (12 * 1.25ms) for fast GATT discovery.
    // scan_itvl=24 (15 ms), window=22 (13.75 ms) gives 91.7% duty cycle.
    client_->setConnectionParams(12, 12, 0, 200, 24, 22);
    client_->setConnectTimeout(7000);
    client_->setClientCallbacks(new DeviceClientCallbacks(this));

    connectStartTime_ = millis();
    stageStartedAt = millis();
    ESP_LOGI(tag_, "Connecting to %s (from scan, timeout=7000 ms)",
             device->getAddress().toString().c_str());
    bool didConnect = client_->connect(device, true, true, false);
    if (didConnect) {
      saveMac(device->getAddress());
    }
    delete device;
    ESP_LOGI(tag_, "async connect request took %lu ms", millis() - stageStartedAt);

    if (!didConnect) {
      ESP_LOGW(tag_, "Connection request failed");
      cleanupClient();
      return false;
    }

    connectionPending_ = true;
    return true;
  }

  ESP_LOGI(tag_, "GATT connected");

  // ----------------------------------------------------------
  // Find service
  // ----------------------------------------------------------

  stageStartedAt = millis();
  service_ = client_->getService(NimBLEUUID(serviceUUID_));
  ESP_LOGI(tag_, "service discovery took %lu ms", millis() - stageStartedAt);

  if (!service_ || !client_->isConnected()) {
    ESP_LOGI(tag_, "ERROR: Service not found or disconnected");

    client_->disconnect();
    cleanupClient();
    return false;
  }

  ESP_LOGI(tag_, "Found service");

  // ----------------------------------------------------------
  // Fetch all characteristics in one GATT round-trip
  // ----------------------------------------------------------

  stageStartedAt = millis();
  service_->getCharacteristics(true);
  if (!client_ || !client_->isConnected() || !service_) {
    ESP_LOGW(tag_, "Disconnected during characteristic discovery");
    cleanupClient();
    return false;
  }
  notifyChar_ = service_->getCharacteristic(NimBLEUUID(notifyUUID_));
  ESP_LOGI(tag_, "characteristic discovery took %lu ms", millis() - stageStartedAt);

  if (!notifyChar_) {
    ESP_LOGI(tag_, "ERROR: Notify characteristic not found");

    if (client_ && client_->isConnected()) {
      client_->disconnect();
    }
    cleanupClient();
    return false;
  }

  ESP_LOGI(tag_,
      "notify handle=%u notify=%d indicate=%d read=%d write=%d",
      notifyChar_->getHandle(),
      notifyChar_->canNotify(),
      notifyChar_->canIndicate(),
      notifyChar_->canRead(),
      notifyChar_->canWrite());

  if (!notifyChar_->canNotify()) {
    ESP_LOGI(tag_, "ERROR: Characteristic does not advertise NOTIFY");

    if (client_ && client_->isConnected()) {
      client_->disconnect();
    }
    cleanupClient();
    return false;
  }

  ESP_LOGI(tag_, "Registering for notifications...");

  portENTER_CRITICAL(&s_notifyMux);
  s_notifyOwners[notifyChar_] = this;
  portEXIT_CRITICAL(&s_notifyMux);

  stageStartedAt = millis();
  bool subOk = notifyChar_->subscribe(true, notifyTrampoline, true);
  ESP_LOGI(tag_, "notification registration took %lu ms (ok=%d)",
           millis() - stageStartedAt, subOk);

  if (!subOk || !client_ || !client_->isConnected() || !service_) {
    ESP_LOGW(tag_, "Notification subscription failed or device disconnected");
    cleanupClient();
    return false;
  }

  notificationsSubscribed_ = true;
  ESP_LOGI(tag_, "Notification subscription registered");

  // ----------------------------------------------------------
  // Find ID characteristic (0 ms lookup from cached vector)
  // ----------------------------------------------------------

  stageStartedAt = millis();
  idChar_ = service_->getCharacteristic(NimBLEUUID(idUUID_));
  ESP_LOGI(tag_, "ID characteristic lookup took %lu ms", millis() - stageStartedAt);

  if (!idChar_ ||
      (!idChar_->canWrite() && !idChar_->canWriteNoResponse())) {
    ESP_LOGI(tag_, "ERROR: ID characteristic is not writable");

    if (client_ && client_->isConnected()) {
      client_->disconnect();
    }
    cleanupClient();
    return false;
  }

  if (!client_ || !client_->isConnected()) {
    ESP_LOGW(tag_, "Device disconnected before completing connection");
    cleanupClient();
    return false;
  }

  connected_ = true;
  return true;
}

// ============================================================
// Public API
// ============================================================

void S3XYDevice::begin() {
  prefsKey_ = std::string("mac_") + tag_;
  if (prefsKey_.length() > 15) {
    prefsKey_ = prefsKey_.substr(0, 15);
  }

  ESP_LOGI(tag_, "Beginning central...");

  loadSavedMac();
  nextScanTime_ = 0;
}

void S3XYDevice::loop() {
  if (connected_) {
    if (!client_ || !client_->isConnected()) {
      cleanupClient();
    } else {
      return;
    }
  }

  if (connectionPending_ || linkEstablished_) {
    if (!connectToDevice()) {
      nextScanTime_ = millis() + 1000;
    }
    return;
  }

  if (!hasTargetDevice() || millis() < nextScanTime_)
    return;

  if (!connectToDevice()) {
    ESP_LOGI(tag_, "Connection failed. Will retry in 1 second...");
    nextScanTime_ = millis() + 1000;
  } else {
    nextScanTime_ = millis();
  }
}

bool S3XYDevice::connected() const {
  return connected_;
}

bool S3XYDevice::ready() const {
  return connected_ && notificationsSubscribed_;
}

bool S3XYDevice::canAttemptConnect() const {
  return !connected_ && !connecting() && !hasTargetDevice() && millis() >= nextScanTime_;
}

void S3XYDevice::onNotify(NotifyCallback cb) {
  notifyCallback_ = cb;
}
