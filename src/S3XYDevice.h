#pragma once

#include "NimBLEDevice.h"
#include <string>

/// Generalized BLE central that connects to a S3XY device (stalk or button).
///
/// Each instance independently manages its own BLE client, scan cycle,
/// reconnect timer, and bonded MAC (persisted to NVS).
///
/// IMPORTANT: Call NimBLEDevice::init() once before calling begin() on any
/// S3XYDevice.  The existing virtual-button code (S3XYButton) already does
/// this, so no extra init is needed when both are used together.
class S3XYDevice {
 public:
  using NotifyCallback = void (*)(S3XYDevice& device, const uint8_t* data, size_t len);

  /// @param tag       Unique label for this device instance.  Used as the NVS
  ///                  key (with a "mac_" prefix) and as the log prefix.
  ///                  Must be ≤ 11 characters so the full key stays within the
  ///                  15-char NVS limit.
  /// @param bleName   BLE local name to scan for (e.g. "ENH_STLK_L").
  /// @param serviceUUID   Primary service UUID.
  /// @param notifyUUID    Notify characteristic UUID.
  /// @param idUUID        ID characteristic UUID.
  S3XYDevice(const char* tag,
             const char* bleName,
             const char* serviceUUID,
             const char* notifyUUID,
             const char* idUUID);

  ~S3XYDevice();

  // Not copyable or movable — BLE callbacks hold `this`.
  S3XYDevice(const S3XYDevice&) = delete;
  S3XYDevice& operator=(const S3XYDevice&) = delete;

  // ----- Lifecycle -----
  void begin();
  void loop();

  // ----- State -----

  /// True after the full setup sequence completes (GATT connect, service
  /// discovery, notification subscription, ID characteristic validation).
  bool connected() const;

  /// True when connected AND notifications are subscribed.
  /// Currently equivalent to connected(), kept separate for future use.
  bool ready() const;
    bool connecting() const { return connectionPending_; }

  // ----- Callbacks -----
  void onNotify(NotifyCallback cb);

  // ----- Accessors -----
  const char* tag() const { return tag_; }
  const char* name() const { return bleName_; }
  const std::string& mac() const { return savedMac_; }

  // ----- NVS -----
  void clearSavedMac();

  // ----- Reconnection -----
  void setTargetDevice(const NimBLEAdvertisedDevice& device);
  bool hasTargetDevice() const { return targetDevice_ != nullptr || directConnectPending_; }
  bool initiating() const { return connectionPending_ && !linkEstablished_; }
  bool canAttemptConnect() const;

 private:
  // Identity (set at construction, never change)
  const char* tag_;
  const char* bleName_;
  const char* serviceUUID_;
  const char* notifyUUID_;
  const char* idUUID_;

  // NVS key derived from tag_ in begin()
  std::string prefsKey_;

  // BLE client state (per-instance)
  NimBLEClient* client_ = nullptr;
  NimBLERemoteService* service_ = nullptr;
  NimBLERemoteCharacteristic* notifyChar_ = nullptr;
  NimBLERemoteCharacteristic* idChar_ = nullptr;

  bool connected_ = false;
  bool notificationsSubscribed_ = false;
    bool connectionPending_ = false;
    bool linkEstablished_ = false;
  uint32_t nextScanTime_ = 0;

  NotifyCallback notifyCallback_ = nullptr;
  std::string savedMac_;

  // Internal
  void cleanupClient();
  void loadSavedMac();
  void saveMac(const NimBLEAddress& address);

  NimBLEAdvertisedDevice* targetDevice_ = nullptr;
  bool directConnectPending_ = false;
  uint8_t directConnectRetries_ = 0;
  bool connectToDevice();

  // BLE callback helpers need access to private members
  friend class DeviceClientCallbacks;
  friend class DeviceLocatorCallbacks;

  // Static trampoline dispatches NimBLE notify callbacks to the correct
  // instance via a global characteristic → device map.
  static void notifyTrampoline(
      NimBLERemoteCharacteristic* characteristic,
      uint8_t* data,
      size_t length,
      bool isNotify);
};
