#include "S3XYDevice.h"

#include "compat.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <map>

// ============================================================
// Static notification dispatch
// ============================================================
//
// NimBLE's subscribe() takes a raw C function pointer with no
// user-data / context parameter.  To route notifications back to the
// correct S3XYDevice instance we keep a small map keyed by the
// NimBLERemoteCharacteristic pointer (which is unique per subscription).

static std::map<NimBLERemoteCharacteristic*, S3XYDevice*> s_notifyOwners;

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
  auto it = s_notifyOwners.find(characteristic);
  if (it == s_notifyOwners.end())
    return;

  S3XYDevice* self = it->second;

  printHex(self->tag_, data, length, isNotify);

  if (self->notifyCallback_) {
    self->notifyCallback_(data, length);
  }
}

// ============================================================
// Client connection callbacks
// ============================================================

class DeviceClientCallbacks : public NimBLEClientCallbacks {
 public:
  explicit DeviceClientCallbacks(S3XYDevice* owner) : owner_(owner) {}

  void onConnect(NimBLEClient*) override {
    owner_->linkEstablished_ = true;
    NimBLEConnInfo info = owner_->client_->getConnInfo();
    ESP_LOGI(owner_->tag_,
             "Connection established: interval=%.2f ms latency=%u timeout=%u ms; "
             "waiting for security/authentication",
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
    owner_->connectionPending_ = false;
    owner_->linkEstablished_ = false;
    owner_->nextScanTime_ = millis() + 1000;
    ESP_LOGW(owner_->tag_, "Connection attempt failed, reason: %d", reason);
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
    owner_->notifyChar_ = nullptr;
    owner_->idChar_ = nullptr;

    owner_->nextScanTime_ = millis() + 1000;
  }

 private:
  S3XYDevice* owner_;
};

// ============================================================
// Scan / locate callbacks
// ============================================================

class DeviceLocatorCallbacks : public NimBLEScanCallbacks {
 public:
  DeviceLocatorCallbacks(const S3XYDevice* owner,
                         const char* targetAddress = nullptr)
      : owner_(owner) {
    if (targetAddress) {
      address_ = targetAddress;
      searchByMac_ = true;
    }
  }

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    bool match = false;

    if (searchByMac_) {
      match = (advertisedDevice->getAddress().toString() == address_);
    } else {
      bool nameMatches =
          advertisedDevice->haveName() &&
          advertisedDevice->getName() == owner_->bleName_;

      bool serviceMatches =
          advertisedDevice->haveServiceUUID() &&
          advertisedDevice->isAdvertisingService(
              NimBLEUUID(owner_->serviceUUID_));

      match = (nameMatches || serviceMatches);
    }

    if (match) {
      if (searchByMac_) {
        ESP_LOGI(owner_->tag_, "*** Found device by saved MAC ***");
      } else {
        ESP_LOGI(owner_->tag_, "*** Found device by name & service UUID ***");
      }

      result_ = new NimBLEAdvertisedDevice(*advertisedDevice);
      NimBLEDevice::getScan()->stop();  // Early exit!
    }
  }

  NimBLEAdvertisedDevice* result() const { return result_; }

 private:
  const S3XYDevice* owner_;
  std::string address_;
  bool searchByMac_ = false;
  NimBLEAdvertisedDevice* result_ = nullptr;
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
  cleanupClient();
}

// ============================================================
// Internal helpers
// ============================================================

void S3XYDevice::cleanupClient() {
  // Remove from trampoline map before pointers are invalidated
  if (notifyChar_) {
    s_notifyOwners.erase(notifyChar_);
  }

  if (client_) {
    if (client_->isConnected()) {
      client_->disconnect();
      delay_ms(150);
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

  if (mac.empty()) {
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

  nvs_handle_t handle;
  if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
      nvs_erase_key(handle, prefsKey_.c_str());
      nvs_commit(handle);
      nvs_close(handle);
  }

  ESP_LOGI(tag_, "Cleared saved MAC");
}

// ============================================================
// Scanning
// ============================================================

NimBLEAdvertisedDevice* S3XYDevice::findByAddress(const char* address) {
  NimBLEScan* scan = NimBLEDevice::getScan();

  // Passive scan is enough when looking for a known MAC
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(80);  // 80% duty cycle

  ESP_LOGI(tag_, "Looking for saved MAC %s", address);

  DeviceLocatorCallbacks callbacks(this, address);
  scan->setScanCallbacks(&callbacks);

  // Blocks for max 5 seconds; exits instantly if callbacks call stop().
  scan->getResults(5000, false);

  scan->setScanCallbacks(nullptr);
  scan->clearResults();
  return callbacks.result();
}

NimBLEAdvertisedDevice* S3XYDevice::findByNameOrService() {
  NimBLEScan* scan = NimBLEDevice::getScan();

  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);  // 80% duty cycle

  ESP_LOGI(tag_, "Scanning for %s...", bleName_);

  DeviceLocatorCallbacks callbacks(this);
  scan->setScanCallbacks(&callbacks);

  // Scan up to 5 sec to catch slow background advertisements
  scan->getResults(5000, false);

  scan->setScanCallbacks(nullptr);
  scan->clearResults();

  return callbacks.result();
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
  cleanupClient();

  NimBLEAdvertisedDevice* device = nullptr;

  if (!savedMac_.empty()) {
    device = findByAddress(savedMac_.c_str());
  }

  if (!device) {
    device = findByNameOrService();
  }

  if (!device) {
    ESP_LOGI(tag_, "Device not found");
    return false;
  }

  ESP_LOGI(tag_, "Connecting to %s", device->getAddress().toString().c_str());

  // A scan can still be completing internally when start() returns.
  // Stop it explicitly before asking NimBLE to create a connection.
  NimBLEDevice::getScan()->stop();
  delay_ms(200);
  ESP_LOGI(tag_, "scan shutdown took %lu ms", millis() - stageStartedAt);

  stageStartedAt = millis();
  client_ = NimBLEDevice::createClient();
  ESP_LOGI(tag_, "create client took %lu ms", millis() - stageStartedAt);

  if (!client_) {
    ESP_LOGI(tag_, "Failed to create BLE client");
    delete device;
    return false;
  }

  client_->setClientCallbacks(new DeviceClientCallbacks(this));

  stageStartedAt = millis();
  bool didConnect = client_->connect(device, true, true, false);
  ESP_LOGI(tag_, "async connect request took %lu ms", millis() - stageStartedAt);

  if (!didConnect) {
    ESP_LOGI(tag_, "Connection failed");

    delete device;
    cleanupClient();
    return false;
  }

  saveMac(device->getAddress());
  delete device;
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

  if (!service_) {
    ESP_LOGI(tag_, "ERROR: Service not found");

    client_->disconnect();
    cleanupClient();
    return false;
  }

  ESP_LOGI(tag_, "Found service");

  // ----------------------------------------------------------
  // Find notify characteristic
  // ----------------------------------------------------------

  stageStartedAt = millis();
  notifyChar_ = service_->getCharacteristic(NimBLEUUID(notifyUUID_));
  ESP_LOGI(tag_, "notify characteristic lookup took %lu ms", millis() - stageStartedAt);

  if (!notifyChar_) {
    ESP_LOGI(tag_, "ERROR: Notify characteristic not found");

    client_->disconnect();
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

    client_->disconnect();
    cleanupClient();
    return false;
  }

  ESP_LOGI(tag_, "Registering for notifications...");

  // Register in trampoline map BEFORE subscribing so the callback
  // can dispatch immediately if the first notification arrives fast.
  s_notifyOwners[notifyChar_] = this;

  stageStartedAt = millis();
  notifyChar_->subscribe(true, notifyTrampoline, true);
  ESP_LOGI(tag_, "notification registration took %lu ms", millis() - stageStartedAt);
  notificationsSubscribed_ = true;

  ESP_LOGI(tag_, "Notification subscription registered");

  // ----------------------------------------------------------
  // Find ID characteristic
  // ----------------------------------------------------------

  stageStartedAt = millis();
  idChar_ = service_->getCharacteristic(NimBLEUUID(idUUID_));
  ESP_LOGI(tag_, "ID characteristic lookup took %lu ms", millis() - stageStartedAt);

  if (!idChar_ ||
      (!idChar_->canWrite() && !idChar_->canWriteNoResponse())) {
    ESP_LOGI(tag_, "ERROR: ID characteristic is not writable");

    client_->disconnect();
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
  // Derive NVS key from tag (max 15 chars for Preferences)
  prefsKey_ = std::string("mac_") + tag_;
  if (prefsKey_.length() > 15) {
    prefsKey_ = prefsKey_.substr(0, 15);
  }

  ESP_LOGI(tag_, "Beginning central...");

  // IMPORTANT:
  // Do NOT call NimBLEDevice::init() here.
  //
  // The caller (or S3XYButton, if also in use) must have already
  // initialized the global BLE stack.

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
    connectToDevice();
    return;
  }

  if (millis() < nextScanTime_)
    return;

  // Backoff timer
  if (!connectToDevice()) {
    ESP_LOGI(tag_, "Not found. Will retry in 5 seconds...");
    nextScanTime_ = millis() + 5000;
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

void S3XYDevice::onNotify(NotifyCallback cb) {
  notifyCallback_ = cb;
}
