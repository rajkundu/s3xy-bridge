#pragma once

#include "S3XYDevice.h"
#include "S3XYDeviceProfile.h"
#include "driver/gpio.h"
#include <cstdint>

/// Manages dynamic discovery, pairing, and lifecycle of S3XY BLE devices.
///
/// On boot, restores previously paired devices from NVS.
/// A configurable GPIO button triggers pairing mode (scan for new devices)
/// or unpair-all (longer hold).
class S3XYDeviceManager {
  // Grant PairingLocatorCallbacks access to private members
  friend class PairingLocatorCallbacks;

 public:
  static constexpr size_t MAX_DEVICES = 3;

  /// @param profiles      Pointer to a static array of known device profiles.
  /// @param profileCount  Number of entries in the profiles array.
  S3XYDeviceManager(const S3XYDeviceProfile* profiles, size_t profileCount);
  ~S3XYDeviceManager();

  /// Initialize: configure pair button GPIO, load saved devices from NVS.
  /// @param pairPin           GPIO for pairing button (default: GPIO_NUM_9)
  /// @param pressDurationMs   Hold to enter pairing mode (default: 5000)
  /// @param pairingWindowMs   How long pairing mode stays active (default: 30000)
  /// @param unpairDurationMs  Hold to unpair all devices (default: 15000)
  void begin(gpio_num_t pairPin = GPIO_NUM_9,
             uint32_t pressDurationMs = 5000,
             uint32_t pairingWindowMs = 30000,
             uint32_t unpairDurationMs = 15000);

  /// Call every main loop iteration.
  void loop();

  // ----- State -----
  size_t deviceCount() const { return deviceCount_; }
  S3XYDevice* device(size_t index) const;
  bool isPairing() const { return pairingMode_; }
  bool anyReady() const;
  bool allReady() const;

  // ----- Unpairing -----
  void unpairAll();
  void unpair(size_t index);

 private:
  // Profiles
  const S3XYDeviceProfile* profiles_;
  size_t profileCount_;

  // Owned devices (heap-allocated)
  S3XYDevice* devices_[MAX_DEVICES] = {};
  uint8_t profileIndices_[MAX_DEVICES] = {};
  size_t deviceCount_ = 0;
  // Heap-allocated strings for tag/name that outlive devices
  char* tags_[MAX_DEVICES] = {};
  char* names_[MAX_DEVICES] = {};

  // Pair button
  gpio_num_t pairPin_ = GPIO_NUM_9;
  uint32_t pressDurationMs_ = 5000;
  uint32_t unpairDurationMs_ = 15000;
  uint32_t buttonPressedAt_ = 0;
  bool buttonWasPressed_ = false;
  bool pairActionTaken_ = false;

  // Pairing mode
  bool pairingMode_ = false;
  uint32_t pairingWindowMs_ = 30000;
  uint32_t pairingEndTime_ = 0;

  // Button input
  void checkPairButton();

  // Pairing
  void enterPairingMode();
  void exitPairingMode();
  void pairingScan();

  // Matching
  const S3XYDeviceProfile* matchProfile(const char* bleName,
                                         size_t* outIndex) const;
  bool isDeviceKnown(const std::string& mac) const;

  // Device management
  S3XYDevice* addDevice(const char* mac, const char* bleName,
                         size_t profileIndex);
  void freeDevice(size_t index);

  // NVS persistence
  void loadDevices();
  void saveDevices();
};
