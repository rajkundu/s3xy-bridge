#pragma once

#include "S3XYDevice.h"

/// Describes a class of S3XY BLE device for pairing-mode discovery.
/// Profiles are pure data — no methods, no inheritance.
struct S3XYDeviceProfile {
  const char* namePrefix;      ///< BLE name prefix to match (e.g. "ENH_STLK")
  const char* serviceUUID;     ///< Expected primary service UUID
  const char* notifyUUID;      ///< Notify characteristic UUID
  const char* idUUID;          ///< ID characteristic UUID
  const char* typeName;        ///< Human-readable label (e.g. "stalk", "button")
  S3XYDevice::NotifyCallback notifyCallback;  ///< Auto-assigned to new devices
};
