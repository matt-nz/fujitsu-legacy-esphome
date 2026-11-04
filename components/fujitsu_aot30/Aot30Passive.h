#pragma once

// ---------------------------------------------------------------------------
// Passive parser for the message stream seen on the AOT30L bus (31 Oct 2026 captures).
//
// After inverting every byte (XOR 0xFF) the bus carries variable-length messages that
// each start with 0x7F -- NOT the 8-byte frames the vendored upstream engine assumes.
// Shapes observed across captures (see docs/aot30-protocol.md):
//
//   7F 28 1x 08|0A         status poll, remote -> unit   (1x: 13 = unit idle, 17 = running)
//   7F 18 1x 48 80          status reply, unit -> remote
//   7F 22|12 0x|1x vv 02 5F 7E   remote setting change (22) and its echo (12); vv = state byte
//   7F 24|14 03             power off (24) and its echo (14)
//
// The high nibble of byte 1 (2 = remote->unit, 1 = unit->remote) and the low nibble as a
// message class (8 status, 2 setting, 4 power) are a working hypothesis, not a validated
// specification. Anything that does not match a known shape exactly is reported as UNKNOWN
// and never interpreted. This parser only reads; nothing here transmits.
//
// No Arduino/ESPHome dependencies, so it can be unit-tested on a desktop compiler.
// ---------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

namespace fuji_passive {

constexpr uint8_t kStartByte = 0x7F;
constexpr size_t kMaxMessageLen = 16;

// Numeric values are the ones the legacy fujitsu_climate component uses for FujiMode / FujiFanMode. The byte-view
// decode below was written against them, so they are kept.
constexpr uint8_t kModeFan = 1;
constexpr uint8_t kModeCool = 3;
constexpr uint8_t kModeHeat = 4;
constexpr uint8_t kModeAuto = 5;
constexpr uint8_t kFanAuto = 0;
constexpr uint8_t kFanLow = 2;
constexpr uint8_t kFanMedium = 3;
constexpr uint8_t kFanHigh = 4;

enum class Kind : uint8_t { UNKNOWN, STATUS, SETTING, POWER_OFF };
enum class Direction : uint8_t { UNKNOWN, REMOTE_TO_UNIT, UNIT_TO_REMOTE };

struct Message {
  Kind kind = Kind::UNKNOWN;
  Direction direction = Direction::UNKNOWN;
  bool running = false;  // STATUS only: poll/reply byte 2 bit 2
  uint8_t value = 0;     // SETTING only: state byte (byte 3)
  uint8_t len = 0;
  uint8_t data[kMaxMessageLen] = {0};
};

struct Setting {
  uint8_t mode = 0;
  bool fan_valid = false;
  uint8_t fan = 0;
  bool temperature_valid = false;
  uint8_t temperature = 0;
};

// Maps a SETTING state byte to what the captures labeled it as. Values the captures have
// not labeled return false and must be ignored rather than guessed.
inline bool decode_setting(uint8_t value, Setting *out) {
  *out = Setting();
  auto with_fan = [out](uint8_t mode, uint8_t fan, bool has_temp, uint8_t temp) {
    out->mode = mode;
    out->fan_valid = true;
    out->fan = fan;
    out->temperature_valid = has_temp;
    out->temperature = temp;
    return true;
  };

  // COOL with AUTO fan: the labeled 18-30 C sweep produced 0x95, 0x9D, ... 0xF5.
  if (value >= 0x95 && value <= 0xF5 && ((value - 0x95) % 0x08) == 0) {
    return with_fan(kModeCool, kFanAuto, true, static_cast<uint8_t>(18 + ((value - 0x95) / 0x08)));
  }

  switch (value) {
    case 0x03: return with_fan(kModeFan, kFanHigh, false, 0);
    case 0x05: return with_fan(kModeFan, kFanMedium, false, 0);
    case 0x09: return with_fan(kModeFan, kFanLow, false, 0);
    case 0x0B: return with_fan(kModeFan, kFanAuto, false, 0);

    case 0x73: return with_fan(kModeAuto, kFanHigh, true, 23);
    case 0x75: return with_fan(kModeAuto, kFanMedium, true, 23);
    case 0x79: return with_fan(kModeAuto, kFanLow, true, 23);
    case 0x7B: return with_fan(kModeAuto, kFanAuto, true, 23);

    case 0xB9: return with_fan(kModeCool, kFanHigh, true, 23);
    case 0xBA: return with_fan(kModeCool, kFanMedium, true, 23);
    case 0xBC: return with_fan(kModeCool, kFanLow, true, 23);

    case 0xE6: return with_fan(kModeHeat, kFanHigh, true, 23);
    case 0xEA: return with_fan(kModeHeat, kFanMedium, true, 23);
    case 0xF2: return with_fan(kModeHeat, kFanLow, true, 23);
    case 0xF6: return with_fan(kModeHeat, kFanAuto, true, 23);

    // 24 C in COOL was captured once and its fan setting is not mapped.
    case 0xC4:
      out->mode = kModeCool;
      out->temperature_valid = true;
      out->temperature = 24;
      return true;

    // The labeled HEAT setpoint sweep held fan LOW: 18-22 C, plus the 23 C fan sweep below.
    case 0x52:
    case 0x72:
    case 0x92:
    case 0xB2:
    case 0xD2:
      return with_fan(kModeHeat, kFanLow, true, static_cast<uint8_t>(18 + ((value - 0x52) / 0x20)));
    default:
      return false;
  }
}

class Parser {
 public:
  // Feed one inverted bus byte. Returns true and fills *out when a message has just been
  // completed. A message completes when the next start byte arrives, so the last message
  // of a burst is reported with the following one.
  bool feed(uint8_t byte, Message *out) {
    bool completed = false;
    if (byte == kStartByte) {
      if (len_ > 0) {
        classify_(out);
        completed = true;
      }
      len_ = 1;
      buf_[0] = byte;
      return completed;
    }
    if (len_ == 0) {
      return false;  // not synchronised yet
    }
    if (len_ >= kMaxMessageLen) {
      len_ = 0;  // runaway message; wait for the next start byte
      return false;
    }
    buf_[len_++] = byte;
    return false;
  }

  void reset() { len_ = 0; }

 private:
  void classify_(Message *out) {
    *out = Message();
    out->len = static_cast<uint8_t>(len_);
    for (size_t i = 0; i < len_; i++) {
      out->data[i] = buf_[i];
    }
    if (len_ < 3) {
      return;
    }

    const uint8_t dir = buf_[1] >> 4;
    const uint8_t cls = buf_[1] & 0x0F;
    if (dir == 2) {
      out->direction = Direction::REMOTE_TO_UNIT;
    } else if (dir == 1) {
      out->direction = Direction::UNIT_TO_REMOTE;
    } else {
      return;
    }

    if (cls == 8 && (buf_[2] & 0xFB) == 0x13) {
      const bool poll = len_ == 4 && dir == 2 && (buf_[3] == 0x08 || buf_[3] == 0x0A);
      const bool reply = len_ == 5 && dir == 1 && buf_[3] == 0x48 && buf_[4] == 0x80;
      if (poll || reply) {
        out->kind = Kind::STATUS;
        out->running = (buf_[2] & 0x04) != 0;
      }
    } else if (cls == 2 && len_ == 7 && (buf_[2] & 0xEF) == 0x03 && buf_[4] == 0x02 && buf_[5] == 0x5F &&
               buf_[6] == 0x7E) {
      out->kind = Kind::SETTING;
      out->value = buf_[3];
    } else if (cls == 4 && len_ == 3 && buf_[2] == 0x03) {
      out->kind = Kind::POWER_OFF;
    }
  }

  uint8_t buf_[kMaxMessageLen] = {0};
  size_t len_ = 0;
};

}  // namespace fuji_passive
