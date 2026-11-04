#pragma once

// Timing script for emulating the wired remote (AOT30L/ART30RL bus). Pure logic, no hardware: the
// caller asks `due()` what to send, transmits it, then calls `sent()`. Offsets are from captured
// logic-analyser runs summarized in docs/aot30-protocol.md. tests/aot30_scheduler_test.cpp checks the script.
//
// The remote owns the bus: it polls about every 247 ms and the unit only replies. A change is sent in
// place of a poll, twice (rounds `03` and `13`), with a poll flagged `0A` after each.

#include <stddef.h>
#include <stdint.h>

#include "Aot30Cells.h"

namespace fuji_aot30 {

struct Action {
  enum class Kind : uint8_t { NONE, POLL, SETTING, OFF };
  Kind kind = Kind::NONE;
  bool running = false;  // POLL: run flag (poll byte 2 = 17 when set, 13 otherwise)
  bool pending = false;  // POLL: pending flag (poll byte 3 = 0A when set, 08 otherwise)
  Mode mode = Mode::HEAT;
  Fan fan = Fan::FAN_AUTO;
  uint8_t temp = 20;
  bool eco = false;
  bool repeat = false;  // SETTING: second round
};

class Scheduler {
 public:
  static constexpr uint32_t kPollPeriodMs = 247;  // median 246.6 ms, range 238-249 ms
  // The real remote's first setting round follows its last normal poll by 397 ms in 22 of 24 captured changes (247 ms in
  // the other two), i.e. 150 ms later than the next poll would have been.
  static constexpr uint32_t kSettingExtraDelayMs = 150;

  void start(uint32_t now_ms) {
    active_ = true;
    running_ = false;
    awaiting_confirm_ = false;
    script_len_ = 0;
    script_pos_ = 0;
    next_poll_ms_ = now_ms;
  }
  void stop() {
    active_ = false;
    script_len_ = 0;
    script_pos_ = 0;
  }
  // The remote's poll run flag mirrors the unit's last status reply (1,592 of the 1,600 captured polls that
  // followed a reply); the exceptions are the polls right after the remote itself commanded on. So idle polls
  // follow the unit, and after a queued on/off change the commanded value is kept until the unit's reply agrees.
  void observe_unit_running(bool running) {
    if (awaiting_confirm_) {
      if (running == running_) {
        awaiting_confirm_ = false;
      }
      return;
    }
    running_ = running;
  }

  bool active() const { return active_; }
  bool running() const { return running_; }
  bool busy() const { return script_pos_ < script_len_; }

  // Queue a change (also used to turn on). Rejected while another change is being sent.
  bool queue_setting(Mode mode, Fan fan, uint8_t temp, bool eco) {
    if (!active_ || busy() || !valid_fan(fan) || (mode != Mode::FAN && !temp_in_range(mode, temp))) {
      return false;
    }
    Action s;
    s.kind = Action::Kind::SETTING;
    s.mode = mode;
    s.fan = fan;
    s.temp = temp;
    s.eco = eco;
    Action pending_poll;
    pending_poll.kind = Action::Kind::POLL;
    pending_poll.running = true;
    pending_poll.pending = true;
    Action normal_poll;
    normal_poll.kind = Action::Kind::POLL;
    normal_poll.running = true;
    Action second = s;
    second.repeat = true;
    script_[0] = {0, s};
    script_[1] = {405, pending_poll};
    script_[2] = {803, second};
    script_[3] = {1210, pending_poll};
    script_[4] = {1516, normal_poll};
    script_len_ = 5;
    script_pos_ = 0;
    t0_set_ = false;
    next_poll_ms_ += kSettingExtraDelayMs;
    after_running_ = true;
    awaiting_confirm_ = true;
    return true;
  }

  bool queue_off() {
    if (!active_ || busy()) {
      return false;
    }
    Action off;
    off.kind = Action::Kind::OFF;
    Action still_running_poll;
    still_running_poll.kind = Action::Kind::POLL;
    still_running_poll.running = true;
    Action idle_poll;
    idle_poll.kind = Action::Kind::POLL;
    script_[0] = {0, off};
    script_[1] = {167, still_running_poll};
    script_[2] = {405, idle_poll};
    script_len_ = 3;
    script_pos_ = 0;
    t0_set_ = false;
    after_running_ = false;
    awaiting_confirm_ = true;
    return true;
  }

  // The action that should go out now, if any. Does not advance; call sent() after transmitting it.
  bool due(uint32_t now_ms, Action *out) const {
    if (!active_) {
      return false;
    }
    if (busy()) {
      const uint32_t when = script_pos_ == 0 || !t0_set_ ? next_poll_ms_ : t0_ms_ + script_[script_pos_].offset_ms;
      if (!reached_(now_ms, when)) {
        return false;
      }
      *out = script_[script_pos_].action;
      return true;
    }
    if (!reached_(now_ms, next_poll_ms_)) {
      return false;
    }
    Action poll;
    poll.kind = Action::Kind::POLL;
    poll.running = running_;
    *out = poll;
    return true;
  }

  // Record that the due action went out at now_ms.
  void sent(uint32_t now_ms) {
    if (!active_) {
      return;
    }
    if (busy()) {
      if (script_pos_ == 0) {
        t0_ms_ = now_ms;
        t0_set_ = true;
      }
      script_pos_++;
      if (!busy()) {
        running_ = after_running_;
        next_poll_ms_ = t0_ms_ + script_[script_len_ - 1].offset_ms + kPollPeriodMs;
      }
    } else {
      next_poll_ms_ = now_ms + kPollPeriodMs;
    }
  }

 private:
  struct Step {
    uint32_t offset_ms;
    Action action;
  };

  static bool reached_(uint32_t now_ms, uint32_t target_ms) {
    return static_cast<int32_t>(now_ms - target_ms) >= 0;
  }

  bool active_ = false;
  bool running_ = false;
  bool after_running_ = false;
  bool awaiting_confirm_ = false;
  bool t0_set_ = false;
  uint32_t t0_ms_ = 0;
  uint32_t next_poll_ms_ = 0;
  Step script_[5];
  size_t script_len_ = 0;
  size_t script_pos_ = 0;
};

}  // namespace fuji_aot30
