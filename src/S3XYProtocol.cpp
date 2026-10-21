#include "S3XYProtocol.h"

// ============================================================
// Stalk Protocol Implementation
// ============================================================

const char* stalkSwitchToString(StalkSwitch sw) {
  switch (sw) {
    case StalkSwitch::EndCap: return "EndCap";
    case StalkSwitch::Down:   return "Down";
    case StalkSwitch::Up:     return "Up";
    default:                  return "Unknown";
  }
}

const char* stalkEventToString(StalkEvent ev) {
  switch (ev) {
    case StalkEvent::Press:         return "Press";
    case StalkEvent::Release:       return "Release";
    case StalkEvent::Action_Single: return "Action_Single";
    case StalkEvent::Action_Double: return "Action_Double";
    case StalkEvent::Action_Long:   return "Action_Long";
    default:                        return "Unknown";
  }
}

bool decodeStalkPacket(const uint8_t* data, size_t len, DecodedStalkPacket* out) {
  if (!data || len < 2 || !out) {
    return false;
  }

  out->rawData = data;
  out->rawLen = len;
  out->timestamp = millis();
  out->sw = StalkSwitch::Unknown;
  out->event = StalkEvent::Unknown;

  // Pack the whole message (2-byte raw press/release or 3-byte interpreted action)
  uint32_t msg = (len == 2)
      ? (((uint32_t)data[0] << 8) | data[1])
      : (((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2]);

  switch (msg) {
    // EndCap
    case 0xA101:   out->sw = StalkSwitch::EndCap; out->event = StalkEvent::Press;         return true;
    case 0xA100:   out->sw = StalkSwitch::EndCap; out->event = StalkEvent::Release;       return true;
    case 0xA1C101: out->sw = StalkSwitch::EndCap; out->event = StalkEvent::Action_Single; return true;
    case 0xA1C102: out->sw = StalkSwitch::EndCap; out->event = StalkEvent::Action_Double; return true;
    case 0xA1C301: out->sw = StalkSwitch::EndCap; out->event = StalkEvent::Action_Long;   return true;

    // Down
    case 0xA201:   out->sw = StalkSwitch::Down;   out->event = StalkEvent::Press;         return true;
    case 0xA200:   out->sw = StalkSwitch::Down;   out->event = StalkEvent::Release;       return true;
    case 0xA2C101: out->sw = StalkSwitch::Down;   out->event = StalkEvent::Action_Single; return true;
    case 0xA2C102: out->sw = StalkSwitch::Down;   out->event = StalkEvent::Action_Double; return true;
    case 0xA2C301: out->sw = StalkSwitch::Down;   out->event = StalkEvent::Action_Long;   return true;

    // Up
    case 0xA301:   out->sw = StalkSwitch::Up;     out->event = StalkEvent::Press;         return true;
    case 0xA300:   out->sw = StalkSwitch::Up;     out->event = StalkEvent::Release;       return true;
    case 0xA3C101: out->sw = StalkSwitch::Up;     out->event = StalkEvent::Action_Single; return true;
    case 0xA3C102: out->sw = StalkSwitch::Up;     out->event = StalkEvent::Action_Double; return true;
    case 0xA3C301: out->sw = StalkSwitch::Up;     out->event = StalkEvent::Action_Long;   return true;

    default:
      return false;
  }
}

// ============================================================
// Button Protocol Implementation
// ============================================================

const char* buttonEventToString(ButtonEvent ev) {
  switch (ev) {
    case ButtonEvent::Press:         return "Press";
    case ButtonEvent::Release:       return "Release";
    case ButtonEvent::Action_Single: return "Action_Single";
    case ButtonEvent::Action_Double: return "Action_Double";
    case ButtonEvent::Action_Long:   return "Action_Long";
    default:                         return "Unknown";
  }
}

bool decodeButtonPacket(const uint8_t* data, size_t len, DecodedButtonPacket* out) {
  if (!data || len == 0 || !out) {
    return false;
  }

  out->rawData = data;
  out->rawLen = len;
  out->timestamp = millis();
  out->event = ButtonEvent::Unknown;

  // 1-byte raw press / release
  if (len == 1) {
    switch (data[0]) {
      case 0x01: out->event = ButtonEvent::Press;   return true;
      case 0x00: out->event = ButtonEvent::Release; return true;
      default:   return false;
    }
  }

  // 2-byte interpreted actions (0xC101: Single, 0xC102: Double, 0xC301: Long)
  if (len >= 2) {
    uint16_t msg = ((uint16_t)data[0] << 8) | data[1];
    switch (msg) {
      case 0xC101: out->event = ButtonEvent::Action_Single; return true;
      case 0xC102: out->event = ButtonEvent::Action_Double; return true;
      case 0xC301: out->event = ButtonEvent::Action_Long;   return true;
      default:     return false;
    }
  }

  return false;
}
