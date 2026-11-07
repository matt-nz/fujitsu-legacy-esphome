#pragma once

// ---------------------------------------------------------------------------------------------
// Bus side of the AOT30L/ART30RL support (old ducted Fujitsu units with a 3-wire LIN wired remote).
//
// On this bus the wired remote is the master: it polls the unit about every 247 ms and the unit only
// replies (see docs/aot30-master-controller.md). This class does two jobs:
//
//  * Always: read the bus without blocking, split it into messages (Aot30Passive.h) and keep track of bus
//    activity, the unit's run flag and, while a real remote is on the bus, a best-effort view of its state.
//  * On request ("master controller"): take over the remote's role. Check that the bus is silent and idles
//    high, poll continuously, and turn the targets submitted by the main thread into the captured setting and
//    off messages (Aot30Cells.h, Aot30Scheduler.h), bit-banged on the TX pin with a read-back of every cell.
//    The wired remote must be disconnected (all three wires). The controller stops itself if a remote is heard
//    or a transmission does not read back correctly.
//
// pump() runs on one dedicated task. Everything else is thread safe by construction: the main thread only uses
// atomics, and the state it reads is packed into single words.
// ---------------------------------------------------------------------------------------------

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <Arduino.h>
#include <HardwareSerial.h>

#include "Aot30Cells.h"
#include "Aot30Passive.h"
#include "Aot30Scheduler.h"

namespace fuji_aot30 {

// Best-effort state seen on the bus while a real remote is connected, or the state the master controller
// commanded. Mode and fan use the fuji_passive::kMode* / kFan* codes.
struct Observed {
  bool valid = false;
  bool on = false;
  bool mode_valid = false;
  uint8_t mode = 0;
  bool fan_valid = false;
  uint8_t fan = 0;
  bool temp_valid = false;
  uint8_t temp = 0;
  bool eco_valid = false;
  bool eco = false;

  bool operator==(const Observed &o) const { return pack() == o.pack(); }
  uint32_t pack() const {
    return (valid ? 1u : 0u) | (on ? 2u : 0u) | (mode_valid ? 4u : 0u) | (static_cast<uint32_t>(mode & 7) << 3) |
           (fan_valid ? 64u : 0u) | (static_cast<uint32_t>(fan & 7) << 7) | (temp_valid ? 1024u : 0u) |
           (static_cast<uint32_t>(temp & 31) << 11) | (eco_valid ? 65536u : 0u) | (eco ? 131072u : 0u);
  }
  static Observed unpack(uint32_t w) {
    Observed o;
    o.valid = (w & 1u) != 0;
    o.on = (w & 2u) != 0;
    o.mode_valid = (w & 4u) != 0;
    o.mode = static_cast<uint8_t>((w >> 3) & 7u);
    o.fan_valid = (w & 64u) != 0;
    o.fan = static_cast<uint8_t>((w >> 7) & 7u);
    o.temp_valid = (w & 1024u) != 0;
    o.temp = static_cast<uint8_t>((w >> 11) & 31u);
    o.eco_valid = (w & 65536u) != 0;
    o.eco = (w & 131072u) != 0;
    return o;
  }
};

enum class MasterState : uint8_t {
  OFF,
  CHECKING_BUS,
  RUNNING,
  REFUSED_REMOTE_PRESENT,
  REFUSED_BUS_LOW,
  STOPPED_REMOTE_PRESENT,
  STOPPED_TX_FAILED,
  TAKEOVER_PENDING,  // a Home Assistant change is waiting for a gap in the remote's polling
  TAKEOVER,          // the remote is cut off and the ESP is sending the change
  TAKEOVER_TIMEOUT,
};

class Bus {
 public:
  // Task side ------------------------------------------------------------------------------------
  void connect(HardwareSerial *serial, int rx_pin, int tx_pin);
  void set_debug(bool debug) { debug_ = debug; }
  // One non-blocking pass: read what has arrived and, if the master controller is on, send what is due.
  void pump();

  // Main thread ----------------------------------------------------------------------------------
  // False if the master controller is already running or starting. The bus task checks the bus and may still
  // refuse; master_state() says what happened.
  bool start_master();
  void stop_master();
  bool master_active() const { return master_enabled_.load(); }
  MasterState master_state() const { return static_cast<MasterState>(master_state_.load()); }
  static const char *master_state_text(MasterState state);
  // Hold the relay in its remote-disconnected position for wiring tests while no master/takeover is active.
  bool set_relay_test(bool cut);
  bool relay_test_active() const { return relay_test_active_.load(); }
  // Ask for a new state; false if the master controller is not running.
  bool submit_target(const Target &target);
  Target last_target() const { return Target::unpack(target_word_.load()); }
  // Run flag in the unit's last reply: -1 unknown, 0 idle, 1 running.
  int unit_run_state() const { return unit_run_state_.load(); }
  Observed observed() const { return Observed::unpack(observed_word_.load()); }
  uint32_t observed_revision() const { return observed_revision_.load(); }
  // Whether the unit echoed the last setting or off message the master sent (the unit repeats what it accepts):
  // 0 none yet, 1 acknowledged, 2 not acknowledged.
  int last_ack() const { return last_ack_.load(); }
  // millis() of the last recognised message on the bus, 0 if none yet.
  uint32_t last_message_ms() const { return last_message_ms_.load(); }
  // Optional relay that cuts the wired remote's +12 V and LIN (normally-closed contacts: coil off = remote connected, so
  // a dead or booting ESP leaves the remote working). `inverted` flips the pin polarity. Set before connect().
  void set_remote_relay(int pin, bool inverted) {
    relay_pin_ = pin;
    relay_inverted_ = inverted;
  }
  bool takeover_available() const { return relay_pin_ >= 0; }
  // With a relay configured and the master controller off: ask for one change to be made by briefly taking over from
  // the remote (cut it off, poll, send the setting, check the echo, hand it back). Commands arriving in the next few
  // seconds are merged. If the master controller is already running, this is submit_target(). False without a relay.
  bool request_takeover(const Target &target);
  // True while the relay holds the remote cut off.
  bool remote_cut() const { return relay_cut_.load(); }
  // Test hook for the host tests; the firmware never turns dry run on.
  void set_dry_run(bool dry_run) { dry_run_.store(dry_run); }

 private:
  enum class Stage : uint8_t { IDLE, RELAY_SETTLE, WAIT_QUIET, LEAD, RUN };

  void drain_rx();
  void on_byte(uint8_t value);
  void publish_observed(const Observed &state);
  void observe_off();
  void master_step();
  void takeover_step();
  void begin_master(bool takeover);
  void apply_target();
  void finish_master(MasterState state, bool keep_state = false);
  void relay_set(bool cut);
  bool transmit(const uint8_t *cells, size_t count, const char *what);

  HardwareSerial *serial_ = nullptr;
  int rx_pin_ = -1;
  int tx_pin_ = -1;
  bool debug_ = false;
  std::atomic<bool> dry_run_{false};

  // Reading
  fuji_passive::Parser parser_;
  uint32_t last_rx_byte_ms_ = 0;
  uint32_t last_remote_message_ms_ = 0;  // wired remote (sender nibble 2) only; ignores stray unit bytes
  uint32_t remote_message_count_ = 0;     // our own transmissions are discarded before parsing
  uint32_t ignore_rx_until_ms_ = 0;
  bool unit_running_known_ = false;
  bool unit_running_ = false;
  uint32_t unit_echo_count_ = 0;  // setting and power-off messages sent by the unit (its echoes)
  uint32_t ack_baseline_ = 0;
  uint32_t ack_at_ms_ = 0;
  bool ack_pending_ = false;
  const char *ack_what_ = "";
  std::atomic<int8_t> last_ack_{0};
  std::atomic<uint32_t> last_message_ms_{0};
  std::atomic<int8_t> unit_run_state_{-1};
  std::atomic<uint32_t> observed_word_{0};
  std::atomic<uint32_t> observed_revision_{0};

  // Master controller
  Scheduler scheduler_;
  std::atomic<bool> master_enabled_{false};
  std::atomic<uint32_t> command_{0};
  std::atomic<uint8_t> master_state_{0};
  std::atomic<uint32_t> target_word_{0};  // unpack() turns an unset (zero) word into the default target
  std::atomic<bool> target_dirty_{false};
  Target applied_;  // bus task: what the unit was last told
  // The state shown in Home Assistant follows a command only once it has actually been sent, and is taken back if the unit does
  // not echo it. pending_observed_ is what the command would show, previous_observed_ what to go back to.
  Observed pending_observed_;
  Observed previous_observed_;
  bool pending_valid_ = false;
  Stage stage_ = Stage::IDLE;
  uint32_t stage_start_ms_ = 0;
  uint32_t remote_baseline_ = 0;

  // Remote relay and takeover (only with a relay pin)
  int relay_pin_ = -1;
  bool relay_inverted_ = false;
  std::atomic<bool> relay_cut_{false};
  std::atomic<bool> takeover_requested_{false};
  std::atomic<bool> relay_test_active_{false};
  std::atomic<uint32_t> relay_test_started_ms_{0};
  bool takeover_ = false;                  // bus task: the current master run is a takeover and ends by itself
  bool takeover_sent_ = false;             // a change was queued during this takeover
  bool takeover_armed_ = false;            // bus task: the merge window of a pending request has started
  uint32_t takeover_not_before_ms_ = 0;    // end of the merge window
  uint32_t takeover_wait_since_ms_ = 0;
  uint32_t takeover_started_ms_ = 0;
  uint8_t prev_byte_ = 0;
  uint32_t unit_reply_end_ms_ = 0;         // when the last unit reply ended (its last bytes were seen)
  uint32_t last_remote_user_ms_ = 0;       // last setting or power-off message from the remote (someone using it)
};

}  // namespace fuji_aot30
