#include "S3XYDeviceManager.h"

#include "compat.h"
#include "NimBLEDevice.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <cstring>
#include <strings.h>
#include <cstdio>

static const char* TAG = "manager";

static inline uint8_t inferAddressType(const std::string& mac) {
  if (mac.length() >= 2) {
    char hexStr[3] = { mac[0], mac[1], '\0' };
    unsigned long firstByte = strtoul(hexStr, nullptr, 16);
    if ((firstByte & 0xC0) == 0xC0) {
      return BLE_ADDR_RANDOM;
    }
  }
  return BLE_ADDR_PUBLIC;
}

// ============================================================
// Whitelist Initiator Connection Callbacks
// ============================================================
class ManagerInitiatorCallbacks : public NimBLEClientCallbacks {
 public:
  explicit ManagerInitiatorCallbacks(S3XYDeviceManager* mgr) : mgr_(mgr) {}

  void onConnect(NimBLEClient* pClient) override {
    NimBLEAddress peer = pClient->getPeerAddress();
    std::string mac = peer.toString();
    ESP_LOGI(TAG, "Whitelist initiator connected to %s", mac.c_str());

    S3XYDevice* targetDev = mgr_->findDeviceByMac(mac);

    if (targetDev && !targetDev->connected()) {
      mgr_->detachInitiatorClient();
      targetDev->attachClient(pClient);
      NimBLEDevice::whiteListRemove(peer);
      mgr_->fallbackToScan_ = false;
    } else {
      ESP_LOGW(TAG, "Whitelist connected to unexpected/already-connected peer %s, disconnecting", mac.c_str());
      pClient->disconnect();
      mgr_->detachInitiatorClient();
      NimBLEDevice::deleteClient(pClient);
    }
  }

  void onConnectFail(NimBLEClient* pClient, int reason) override {
    ESP_LOGI(TAG, "Whitelist initiator attempt ended/timed out (reason: %d)", reason);
    bool wasActive = (mgr_->initiatorClient_ == pClient);
    mgr_->initiatorClient_ = nullptr;
    mgr_->whitelistInitiatorActive_ = false;
    if (wasActive) {
      mgr_->onInitiatorExpired();
    }
  }

 private:
  S3XYDeviceManager* mgr_;
};

// ============================================================
// Shared Non-Blocking Reconnect Scan Callback
// ============================================================
// Listens for ALL disconnected devices in a single scan pass.

class ReconnectScanCallbacks : public NimBLEScanCallbacks {
 public:
  explicit ReconnectScanCallbacks(S3XYDeviceManager* mgr) : mgr_(mgr) {}

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    std::string mac = advertisedDevice->getAddress().toString();

    for (size_t i = 0; i < mgr_->deviceCount(); ++i) {
      S3XYDevice* dev = mgr_->device(i);
      if (!dev || dev->connected() || dev->connecting() || dev->hasTargetDevice()) {
        continue;
      }

      if (dev->mac().empty()) {
        continue;
      }

      if (strcasecmp(mac.c_str(), dev->mac().c_str()) == 0) {
        ESP_LOGI(TAG, "Scan matched saved device [%u] (%s)",
                 (unsigned)i, dev->mac().c_str());
        dev->setTargetDevice(*advertisedDevice);
        // Stop scan immediately so main loop can initiate connection right away
        NimBLEDevice::getScan()->stop();
        return;
      }
    }
  }

 private:
  S3XYDeviceManager* mgr_;
};

// Static instance so its lifetime persists across non-blocking scans
static ReconnectScanCallbacks* s_reconnectCallbacks = nullptr;

// ============================================================
// Pairing scan callback — discovers any ENH_* device
// ============================================================

class PairingLocatorCallbacks : public NimBLEScanCallbacks {
 public:
  explicit PairingLocatorCallbacks(S3XYDeviceManager* mgr) : mgr_(mgr) {}

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!advertisedDevice->haveName()) return;

    std::string mac = advertisedDevice->getAddress().toString();
    std::string name = advertisedDevice->getName();
    if (name.rfind("ENH_", 0) != 0) return;

    bool isNewAddr = mgr_->pairingLoggedAddresses_.insert(mac).second;

    std::string services;
    for (uint8_t i = 0; i < advertisedDevice->getServiceUUIDCount(); ++i) {
      if (!services.empty()) services += ",";
      services += advertisedDevice->getServiceUUID(i).toString();
    }
    if (services.empty()) services = "<none>";

    if (isNewAddr) {
      ESP_LOGI(TAG,
               "Pairing advertisement: name=%s address=%s rssi=%d services=%s "
               "connectable=%d scannable=%d",
               name.c_str(), mac.c_str(), advertisedDevice->getRSSI(),
               services.c_str(), advertisedDevice->isConnectable(),
               advertisedDevice->isScannable());
    }

    if (mgr_->isDeviceKnown(mac)) {
      if (isNewAddr) {
        ESP_LOGI(TAG, "Rejected advertisement %s: device already paired", mac.c_str());
      }
      return;
    }

    size_t profIdx;
    const S3XYDeviceProfile* profile =
        mgr_->matchProfile(advertisedDevice, &profIdx);
    if (!profile) {
      if (isNewAddr) {
        ESP_LOGW(TAG, "Rejected ENH_* device %s: no name/service profile matched",
                 mac.c_str());
      }
      return;
    }

    if (found_) return;

    ESP_LOGI(TAG, "*** Pairing candidate: %s (%s), profile=%s ***",
             name.c_str(), mac.c_str(), profile->typeName);

    delete foundDevice_;
    foundDevice_ = new NimBLEAdvertisedDevice(*advertisedDevice);
    foundMac_ = mac;
    foundName_ = name;
    matchedProfileIndex_ = profIdx;
    found_ = true;
    NimBLEDevice::getScan()->stop();
  }

  ~PairingLocatorCallbacks() override {
    delete foundDevice_;
    foundDevice_ = nullptr;
  }

  bool found() const { return found_; }
  const NimBLEAdvertisedDevice* foundDevice() const { return foundDevice_; }
  const std::string& foundMac() const { return foundMac_; }
  const std::string& foundName() const { return foundName_; }
  size_t matchedProfileIndex() const { return matchedProfileIndex_; }

 private:
  S3XYDeviceManager* mgr_;
  NimBLEAdvertisedDevice* foundDevice_ = nullptr;
  std::string foundMac_;
  std::string foundName_;
  size_t matchedProfileIndex_ = 0;
  bool found_ = false;
};

// ============================================================
// Construction / destruction
// ============================================================

static S3XYDeviceManager* s_instance = nullptr;

S3XYDeviceManager* S3XYDeviceManager::instance() {
  return s_instance;
}

S3XYDeviceManager::S3XYDeviceManager(const S3XYDeviceProfile* profiles,
                                     size_t profileCount)
    : profiles_(profiles), profileCount_(profileCount) {
  s_instance = this;
  if (!s_reconnectCallbacks) {
    s_reconnectCallbacks = new ReconnectScanCallbacks(this);
  }
  initiatorCallbacks_ = new ManagerInitiatorCallbacks(this);
}

S3XYDeviceManager::~S3XYDeviceManager() {
  stopWhitelistInitiator();
  delete initiatorCallbacks_;
  initiatorCallbacks_ = nullptr;
  if (s_instance == this) s_instance = nullptr;
  for (size_t i = 0; i < deviceCount_; ++i) {
    freeDevice(i);
  }
}

void S3XYDeviceManager::detachInitiatorClient() {
  initiatorClient_ = nullptr;
  whitelistInitiatorActive_ = false;
}

void S3XYDeviceManager::onInitiatorExpired() {
  ESP_LOGI(TAG, "Whitelist initiator window expired -> falling back to background scan");
  initiatorClient_ = nullptr;
  whitelistInitiatorActive_ = false;
  fallbackToScan_ = true;
  nextReconnectScanTime_ = millis();
}

void S3XYDeviceManager::onDeviceDisconnected(S3XYDevice* dev) {
  ESP_LOGI(TAG, "Device [%s] disconnected -> refreshing reconnection", dev ? dev->tag() : "?");
  fallbackToScan_ = false;
  if (whitelistInitiatorActive_) {
    stopWhitelistInitiator();
  }
}

// ============================================================
// Public API
// ============================================================

void S3XYDeviceManager::begin(gpio_num_t pairPin,
                               uint32_t pressDurationMs,
                               uint32_t pairingWindowMs,
                               uint32_t unpairDurationMs) {
  pairPin_ = pairPin;
  pressDurationMs_ = pressDurationMs;
  pairingWindowMs_ = pairingWindowMs;
  unpairDurationMs_ = unpairDurationMs;

  // Configure pair button GPIO
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << pairPin_);
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&io_conf);

  loadDevices();

  if (deviceCount_ == 0) {
    ESP_LOGI(TAG, "No saved devices. Hold pair button (%d) for %lu ms to enter pairing mode.",
             pairPin_, (unsigned long)pressDurationMs_);
  }
}

bool S3XYDeviceManager::startWhitelistInitiator() {
  if (whitelistInitiatorActive_) return true;

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    scan->stop();
  }

  // Remove any already-connected or currently-connecting devices from whitelist
  for (size_t i = 0; i < deviceCount_; ++i) {
    S3XYDevice* dev = devices_[i];
    if (dev && (dev->connected() || dev->connecting()) && !dev->mac().empty()) {
      uint8_t addrType = inferAddressType(dev->mac());
      NimBLEAddress addr(dev->mac(), addrType);
      if (NimBLEDevice::onWhiteList(addr)) {
        NimBLEDevice::whiteListRemove(addr);
      }
    }
  }

  // Ensure all disconnected saved devices are in the whitelist
  size_t count = 0;
  for (size_t i = 0; i < deviceCount_; ++i) {
    S3XYDevice* dev = devices_[i];
    if (!dev || dev->connected() || dev->connecting() || dev->mac().empty()) {
      continue;
    }
    uint8_t addrType = inferAddressType(dev->mac());
    NimBLEAddress addr(dev->mac(), addrType);
    if (NimBLEDevice::onWhiteList(addr) || NimBLEDevice::whiteListAdd(addr)) {
      ESP_LOGI(TAG, "Whitelist active for [%s] %s (type %u)", dev->tag(), dev->mac().c_str(), addrType);
      count++;
    } else {
      ESP_LOGW(TAG, "Failed adding [%s] %s to whitelist", dev->tag(), dev->mac().c_str());
    }
  }

  if (count == 0) {
    return false;
  }

  initiatorClient_ = NimBLEDevice::createClient();
  if (!initiatorClient_) {
    ESP_LOGE(TAG, "Failed to create BLE initiator client");
    return false;
  }

  // 15 ms initial connection interval (12 * 1.25ms) for fast, reliable connection establishment
  // and rapid GATT discovery (80 ms) without 0x3e connection aborts.
  // scan_itvl=24 (15 ms), window=22 (13.75 ms) gives 91.7% duty cycle for maximum probability
  // of catching the S3XY button's 1.5 ms burst on its very first 2.56s cycle.
  initiatorClient_->setConnectionParams(12, 12, 0, 200, 24, 22);
  initiatorClient_->setConnectTimeout(8000);
  initiatorClient_->setSelfDelete(false, true);
  initiatorClient_->setClientCallbacks(initiatorCallbacks_, false);

  initiatorStartTime_ = millis();
  whitelistInitiatorActive_ = true;
  ESP_LOGI(TAG, "Starting whitelist initiator for %u device(s) (timeout=8000 ms, 91.7%% duty cycle)", (unsigned)count);

  bool ok = initiatorClient_->connect(NimBLEAddress(), true, true, false);
  if (!ok) {
    ESP_LOGW(TAG, "Whitelist initiator connect request failed");
    whitelistInitiatorActive_ = false;
    NimBLEDevice::deleteClient(initiatorClient_);
    initiatorClient_ = nullptr;
    return false;
  }

  return true;
}

void S3XYDeviceManager::stopWhitelistInitiator() {
  if (whitelistInitiatorActive_ && initiatorClient_) {
    ESP_LOGI(TAG, "Stopping active whitelist initiator");
    whitelistInitiatorActive_ = false;
    NimBLEClient* client = initiatorClient_;
    initiatorClient_ = nullptr;
    client->cancelConnect();
  }
  whitelistInitiatorActive_ = false;
  initiatorClient_ = nullptr;
}

void S3XYDeviceManager::updateBackgroundScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();

  bool anyInitiating = false;
  bool anyTargetReady = false;
  bool anyDisconnected = false;
  bool anyConnected = false;

  for (size_t i = 0; i < deviceCount_; ++i) {
    S3XYDevice* dev = devices_[i];
    if (!dev) continue;

    if (dev->connected()) {
      anyConnected = true;
    } else {
      anyDisconnected = true;
      if (dev->connecting()) {
        anyInitiating = true;
      }
      if (dev->hasTargetDevice()) {
        anyTargetReady = true;
      }
    }
  }

  if (!anyDisconnected || anyInitiating || anyTargetReady || !fallbackToScan_) {
    if (scan->isScanning()) {
      scan->stop();
    }
    return;
  }

  if (!scan->isScanning() && millis() >= nextReconnectScanTime_) {
    scan->clearResults();
    scan->setScanCallbacks(s_reconnectCallbacks, true);
    scan->setActiveScan(false);

    if (anyConnected) {
      // Gentle 30% duty cycle (window 48 * 0.625ms = 30ms, interval 160 * 0.625ms = 100ms)
      // for 1500 ms, followed by a 3500 ms quiet period.
      // This leaves >90% average radio airtime for active connections and virtual button
      // advertising, completely preventing LL response timeouts (reason: 546).
      scan->setInterval(160);
      scan->setWindow(48);
      scan->start(1500, false, false);
      nextReconnectScanTime_ = millis() + 5000;
    } else {
      // No devices connected yet: scan with 75% duty cycle for 2000 ms, 500 ms quiet
      scan->setInterval(160);
      scan->setWindow(120);
      scan->start(2000, false, false);
      nextReconnectScanTime_ = millis() + 2500;
    }
  }
}

void S3XYDeviceManager::updateReconnection() {
  if (pairingMode_) return;

  size_t disconnectedKnownCount = 0;
  bool anyInitiating = false;
  bool anyHasTarget = false;

  for (size_t i = 0; i < deviceCount_; ++i) {
    S3XYDevice* dev = devices_[i];
    if (!dev) continue;

    if (!dev->connected()) {
      if (!dev->mac().empty() && !dev->connecting() && !dev->hasTargetDevice()) {
        disconnectedKnownCount++;
      }
      if (dev->initiating()) {
        anyInitiating = true;
      }
      if (dev->hasTargetDevice()) {
        anyHasTarget = true;
      }
    }
  }

  // If all known devices are connected:
  if (disconnectedKnownCount == 0 && !anyInitiating && !anyHasTarget) {
    stopWhitelistInitiator();
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) {
      scan->stop();
    }
    fallbackToScan_ = false;
    return;
  }

  // If any device has a target from scan or is initiating direct connect:
  if (anyHasTarget || anyInitiating) {
    if (whitelistInitiatorActive_) {
      stopWhitelistInitiator();
    }
    return;
  }

  // Fallback scan mode when whitelist initiator has timed out for remaining devices:
  if (fallbackToScan_) {
    updateBackgroundScan();
    return;
  }

  // Whitelist initiator mode:
  if (whitelistInitiatorActive_) {
    // NimBLE does not enforce connectTimeout for asynchronous connections.
    // Enforce an 8-second software timeout so missing devices (e.g. dev_0) do not
    // lock the radio in the high-duty-cycle Initiating State indefinitely.
    if (millis() - initiatorStartTime_ >= 8000) {
      ESP_LOGI(TAG, "Whitelist initiator window expired (8000 ms) -> switching to background scan");
      stopWhitelistInitiator();
      onInitiatorExpired();
      return;
    }
  }

  if (!whitelistInitiatorActive_ && disconnectedKnownCount > 0) {
    if (!startWhitelistInitiator()) {
      fallbackToScan_ = true;
    }
  }
}

void S3XYDeviceManager::loop() {
  checkPairButton();

  if (pairingMode_) {
    if (pairingJustEntered_) {
      pairingJustEntered_ = false;
      return;
    }

    bool deviceBusy = false;
    for (size_t i = 0; i < deviceCount_; ++i) {
      if (devices_[i] && devices_[i]->connecting()) {
        deviceBusy = true;
        break;
      }
    }

    if (millis() >= pairingEndTime_) {
      exitPairingMode();
    } else if (!deviceBusy) {
      pairingScan();
    }

    for (size_t i = 0; i < deviceCount_; ++i) {
      if (devices_[i] && (devices_[i]->connected() || devices_[i]->connecting() ||
                          devices_[i]->hasTargetDevice())) {
        devices_[i]->loop();
      }
    }

    if (pairingCandidateIndex_ < deviceCount_ &&
        devices_[pairingCandidateIndex_] &&
        devices_[pairingCandidateIndex_]->ready()) {
      ESP_LOGI(TAG, "Pairing successful: %s is connected and subscribed",
               names_[pairingCandidateIndex_]);
      pairingCandidateIndex_ = MAX_DEVICES;
      exitPairingMode();
    }
    return;
  }

  // Normal mode: central-managed whitelist reconnection
  updateReconnection();

  // Tick all devices so they can run GATT discovery and monitor connection status
  for (size_t i = 0; i < deviceCount_; ++i) {
    if (devices_[i]) {
      devices_[i]->loop();
    }
  }
}

S3XYDevice* S3XYDeviceManager::device(size_t index) const {
  if (index >= deviceCount_) return nullptr;
  return devices_[index];
}

S3XYDevice* S3XYDeviceManager::findDeviceByMac(const std::string& mac) const {
  for (size_t i = 0; i < deviceCount_; ++i) {
    if (devices_[i] && strcasecmp(devices_[i]->mac().c_str(), mac.c_str()) == 0) {
      return devices_[i];
    }
  }
  return nullptr;
}

bool S3XYDeviceManager::anyReady() const {
  for (size_t i = 0; i < deviceCount_; ++i) {
    if (devices_[i] && devices_[i]->ready()) return true;
  }
  return false;
}

bool S3XYDeviceManager::allReady() const {
  // returns true even if deviceCount_ is 0
  return numReady() == deviceCount_;
}

size_t S3XYDeviceManager::numReady() const {
  size_t count = 0;
  for (size_t i = 0; i < deviceCount_; ++i) {
    if (devices_[i] && devices_[i]->ready()) {
      count++;
    }
  }
  return count;
}

// ============================================================
// Button handling
// ============================================================

void S3XYDeviceManager::checkPairButton() {
  bool pressed = (gpio_get_level(pairPin_) == 0);

  if (pressed && !buttonWasPressed_) {
    buttonPressedAt_ = millis();
    pairActionTaken_ = false;
  }

  if (pressed && !pairActionTaken_) {
    uint32_t held = millis() - buttonPressedAt_;

    if (held >= unpairDurationMs_) {
      ESP_LOGW(TAG, "*** UNPAIR ALL — long press detected ***");
      unpairAll();
      pairActionTaken_ = true;
    } else if (held >= pressDurationMs_ && !pairingMode_) {
      enterPairingMode();
      pairActionTaken_ = true;
    }
  }

  buttonWasPressed_ = pressed;
}

// ============================================================
// Pairing mode
// ============================================================

void S3XYDeviceManager::enterPairingMode() {
  stopWhitelistInitiator();
  if (NimBLEDevice::getScan()->isScanning()) {
    NimBLEDevice::getScan()->stop();
  }
  pairingMode_ = true;
  pairingCandidateIndex_ = MAX_DEVICES;
  pairingJustEntered_ = true;
  pairingLoggedAddresses_.clear();
  pairingEndTime_ = millis() + pairingWindowMs_;
  ESP_LOGI(TAG, "Entering pairing mode for %lu ms", (unsigned long)pairingWindowMs_);
}

void S3XYDeviceManager::exitPairingMode() {
  pairingMode_ = false;
  nextReconnectScanTime_ = 0;
  fallbackToScan_ = false;
  ESP_LOGI(TAG, "Exiting pairing mode");
}

void S3XYDeviceManager::pairingScan() {
  if (deviceCount_ >= MAX_DEVICES) {
    ESP_LOGW(TAG, "Max devices (%u) reached, exiting pairing mode",
             (unsigned)MAX_DEVICES);
    exitPairingMode();
    return;
  }

  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    scan->stop();
  }

  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(80);

  ESP_LOGI(TAG, "Pairing scan started (active, 5 seconds)");

  PairingLocatorCallbacks callbacks(this);
  scan->setScanCallbacks(&callbacks);
  scan->getResults(5000, false);
  scan->setScanCallbacks(nullptr);
  scan->clearResults();

  if (!callbacks.found()) {
    ESP_LOGI(TAG, "Pairing scan finished: no new matching ENH_* device found");
    return;
  }

  ESP_LOGI(TAG, "Pairing selected %s (%s), creating central client",
           callbacks.foundName().c_str(), callbacks.foundMac().c_str());
  S3XYDevice* device = addDevice(callbacks.foundMac().c_str(),
                                 callbacks.foundName().c_str(),
                                 callbacks.matchedProfileIndex());
  if (!device) {
    ESP_LOGE(TAG, "Pairing failed: could not add %s", callbacks.foundName().c_str());
  } else {
    ESP_LOGI(TAG, "Pairing candidate stored; attempting connection now");
    pairingCandidateIndex_ = deviceCount_ - 1;
    if (callbacks.foundDevice()) {
      device->setTargetDevice(*callbacks.foundDevice());
    }
    device->loop();
  }
}

// ============================================================
// Profile matching
// ============================================================

const S3XYDeviceProfile* S3XYDeviceManager::matchProfile(
    const NimBLEAdvertisedDevice* advertisedDevice, size_t* outIndex) const {
  const std::string bleName = advertisedDevice->getName();
  for (size_t i = 0; i < profileCount_; ++i) {
    size_t prefixLen = strlen(profiles_[i].namePrefix);
    if (bleName.compare(0, prefixLen, profiles_[i].namePrefix) != 0) continue;

    bool serviceMatches = advertisedDevice->haveServiceUUID() &&
        advertisedDevice->isAdvertisingService(
            NimBLEUUID(profiles_[i].serviceUUID));
    ESP_LOGI(TAG, "Profile check: name=%s profile=%s service=%s",
             bleName.c_str(), profiles_[i].typeName,
             serviceMatches ? "MATCH" : "MISMATCH");
    if (!serviceMatches) continue;

    if (outIndex) *outIndex = i;
    return &profiles_[i];
  }
  return nullptr;
}

bool S3XYDeviceManager::isDeviceKnown(const std::string& mac) const {
  for (size_t i = 0; i < deviceCount_; ++i) {
    if (devices_[i] && devices_[i]->mac() == mac) {
      return true;
    }
  }
  return false;
}

// ============================================================
// Device management
// ============================================================

S3XYDevice* S3XYDeviceManager::addDevice(const char* mac,
                                          const char* bleName,
                                          size_t profileIndex) {
  if (deviceCount_ >= MAX_DEVICES) return nullptr;

  size_t idx = deviceCount_;
  const auto& prof = profiles_[profileIndex];

  // Generate tag: "dev_0", "dev_1", etc.
  char tagBuf[8];
  snprintf(tagBuf, sizeof(tagBuf), "dev_%u", (unsigned)idx);
  tags_[idx] = strdup(tagBuf);
  names_[idx] = strdup(bleName);

  // Pre-write MAC to NVS using S3XYDevice's key format ("mac_<tag>")
  // so that begin() -> loadSavedMac() finds it immediately.
  {
    nvs_handle_t handle;
    if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
      char key[16];
      snprintf(key, sizeof(key), "mac_%s", tags_[idx]);
      nvs_set_str(handle, key, mac);
      nvs_commit(handle);
      nvs_close(handle);
    }
  }

  // Create device with profile UUIDs
  auto* dev = new S3XYDevice(
      tags_[idx],
      names_[idx],
      prof.serviceUUID,
      prof.notifyUUID,
      prof.idUUID);

  dev->onNotify(prof.notifyCallback);
  dev->begin();  // loads the pre-written MAC

  devices_[idx] = dev;
  profileIndices_[idx] = profileIndex;
  deviceCount_++;

  saveDevices();

  ESP_LOGI(TAG, "Added device [%s] %s (%s) as %s",
           tags_[idx], bleName, mac, prof.typeName);
  return dev;
}

void S3XYDeviceManager::freeDevice(size_t index) {
  if (index >= deviceCount_) return;

  delete devices_[index];
  devices_[index] = nullptr;

  free(tags_[index]);
  tags_[index] = nullptr;

  free(names_[index]);
  names_[index] = nullptr;
}

void S3XYDeviceManager::unpairAll() {
  ESP_LOGW(TAG, "Unpairing all %u device(s) and clearing BLE bonds", (unsigned)deviceCount_);

  stopWhitelistInitiator();
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) {
    scan->stop();
  }
  while (NimBLEDevice::getWhiteListCount() > 0) {
    NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
  }
  fallbackToScan_ = false;

  // Clear all stored BLE bonds from NimBLE NVS
  NimBLEDevice::deleteAllBonds();

  for (size_t i = 0; i < deviceCount_; ++i) {
    // Clear the device's own NVS MAC
    if (devices_[i]) {
      devices_[i]->clearSavedMac();
    }
    freeDevice(i);
  }
  deviceCount_ = 0;

  // Clear manager's NVS
  nvs_handle_t handle;
  if (nvs_open("s3xy_mgr", NVS_READWRITE, &handle) == ESP_OK) {
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
  }

  ESP_LOGI(TAG, "All devices unpaired");
}

void S3XYDeviceManager::unpair(size_t index) {
  if (index >= deviceCount_) return;

  ESP_LOGI(TAG, "Unpairing device %u [%s]", (unsigned)index,
           tags_[index] ? tags_[index] : "?");

  stopWhitelistInitiator();
  fallbackToScan_ = false;

  if (devices_[index]) {
    devices_[index]->clearSavedMac();
  }
  freeDevice(index);

  // Compact arrays: shift everything after `index` down by one
  for (size_t i = index; i + 1 < deviceCount_; ++i) {
    devices_[i] = devices_[i + 1];
    profileIndices_[i] = profileIndices_[i + 1];
    tags_[i] = tags_[i + 1];
    names_[i] = names_[i + 1];
  }
  deviceCount_--;
  devices_[deviceCount_] = nullptr;
  tags_[deviceCount_] = nullptr;
  names_[deviceCount_] = nullptr;

  // Re-save MACs and update tags to match their new indices in NVS
  for (size_t i = index; i < deviceCount_; ++i) {
    if (devices_[i] && !devices_[i]->mac().empty()) {
      char newTag[8];
      snprintf(newTag, sizeof(newTag), "dev_%u", (unsigned)i);
      free(tags_[i]);
      tags_[i] = strdup(newTag);
      nvs_handle_t handle;
      if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "mac_%s", tags_[i]);
        nvs_set_str(handle, key, devices_[i]->mac().c_str());
        nvs_commit(handle);
        nvs_close(handle);
      }
    }
  }
  // Clear the old last slot's MAC from NVS
  {
    nvs_handle_t handle;
    if (nvs_open("s3xy", NVS_READWRITE, &handle) == ESP_OK) {
      char key[16];
      snprintf(key, sizeof(key), "mac_dev_%u", (unsigned)deviceCount_);
      nvs_erase_key(handle, key);
      nvs_commit(handle);
      nvs_close(handle);
    }
  }

  saveDevices();
}

// ============================================================
// NVS persistence
// ============================================================

void S3XYDeviceManager::saveDevices() {
  nvs_handle_t handle;
  if (nvs_open("s3xy_mgr", NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for save");
    return;
  }

  nvs_set_u8(handle, "dev_cnt", (uint8_t)deviceCount_);

  for (size_t i = 0; i < deviceCount_; ++i) {
    char profKey[12], nameKey[12];
    snprintf(profKey, sizeof(profKey), "d%u_prof", (unsigned)i);
    snprintf(nameKey, sizeof(nameKey), "d%u_name", (unsigned)i);

    nvs_set_u8(handle, profKey, profileIndices_[i]);
    nvs_set_str(handle, nameKey, names_[i] ? names_[i] : "");
  }

  nvs_commit(handle);
  nvs_close(handle);

  ESP_LOGD(TAG, "Saved %u device(s) to NVS", (unsigned)deviceCount_);
}

void S3XYDeviceManager::loadDevices() {
  nvs_handle_t handle;
  if (nvs_open("s3xy_mgr", NVS_READONLY, &handle) != ESP_OK) {
    ESP_LOGI(TAG, "No saved device data in NVS");
    return;
  }

  uint8_t count = 0;
  nvs_get_u8(handle, "dev_cnt", &count);

  for (size_t i = 0; i < count && i < MAX_DEVICES; ++i) {
    char profKey[12], nameKey[12];
    snprintf(profKey, sizeof(profKey), "d%u_prof", (unsigned)i);
    snprintf(nameKey, sizeof(nameKey), "d%u_name", (unsigned)i);

    uint8_t profIdx = 0xFF;
    nvs_get_u8(handle, profKey, &profIdx);

    size_t nameLen = 0;
    char* nameBuf = nullptr;
    if (nvs_get_str(handle, nameKey, nullptr, &nameLen) == ESP_OK && nameLen > 0) {
      nameBuf = static_cast<char*>(malloc(nameLen));
      if (nameBuf) {
        nvs_get_str(handle, nameKey, nameBuf, &nameLen);
      }
    }

    if (profIdx >= profileCount_ || !nameBuf || nameLen == 0) {
      free(nameBuf);
      continue;
    }

    const auto& prof = profiles_[profIdx];

    // Generate tag
    char tagBuf[8];
    snprintf(tagBuf, sizeof(tagBuf), "dev_%u", (unsigned)i);
    tags_[i] = strdup(tagBuf);
    names_[i] = nameBuf;  // take ownership

    auto* dev = new S3XYDevice(
        tags_[i],
        names_[i],
        prof.serviceUUID,
        prof.notifyUUID,
        prof.idUUID);

    dev->onNotify(prof.notifyCallback);
    dev->begin();  // loads MAC from "mac_dev_N"

    devices_[i] = dev;
    profileIndices_[i] = profIdx;
    deviceCount_++;

    ESP_LOGI(TAG, "Restored device [%s] %s as %s",
             tags_[i], names_[i], prof.typeName);
  }

  nvs_close(handle);
}
