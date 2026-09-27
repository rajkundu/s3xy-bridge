#pragma once

#include <cstdint>
#include <cstddef>
#include "compat.h"

// Forward declaration
class S3XYDevice;

// ============================================================
// Stalk Protocol Definitions
// ============================================================

enum class StalkSwitch : uint8_t {
  EndCap,
  Down,
  Up,
  Unknown
};

enum class StalkEvent : uint8_t {
  Press,
  Release,
  Action_Single,
  Action_Double,
  Action_Long,
  Unknown
};

struct DecodedStalkPacket {
  StalkSwitch sw = StalkSwitch::Unknown;
  StalkEvent event = StalkEvent::Unknown;
  uint32_t timestamp = 0;      // millis() captured at arrival
  const uint8_t* rawData = nullptr;
  size_t rawLen = 0;
};

const char* stalkSwitchToString(StalkSwitch sw);
const char* stalkEventToString(StalkEvent ev);
bool decodeStalkPacket(const uint8_t* data, size_t len, DecodedStalkPacket* out);

// ============================================================
// Button Protocol Definitions
// ============================================================

enum class ButtonEvent : uint8_t {
  Press,
  Release,
  Action_Single,
  Action_Double,
  Action_Long,
  Unknown
};

struct DecodedButtonPacket {
  ButtonEvent event = ButtonEvent::Unknown;
  uint32_t timestamp = 0;      // millis() captured at arrival
  const uint8_t* rawData = nullptr;
  size_t rawLen = 0;
};

const char* buttonEventToString(ButtonEvent ev);
bool decodeButtonPacket(const uint8_t* data, size_t len, DecodedButtonPacket* out);
