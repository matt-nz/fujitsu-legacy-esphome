#include "Aot30Bus.h"

#include "esphome/core/log.h"

#include <cstdio>

#ifdef ESP32
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#endif

namespace fuji_aot30 {

namespace {
const char *const TAG = "aot30.bus";

constexpr uint32_t kCmdStart = 1;
constexpr uint32_t kCmdStop = 2;
// The unit's echo of a setting or power-off message should have arrived this long after we finished sending it
// (it starts about 190 ms after the start of the message and is repeated after the next poll).
constexpr uint32_t kAckWaitMs = 450;
// The bus must have been silent this long before starting; a connected remote polls every ~247 ms.
constexpr uint32_t kQuietMs = 1500;
// Polling time before the first command, so the unit sees a normal cycle first.
constexpr uint32_t kLeadMs = 2000;
// Our own transmission comes back on RX and takes up to this long to finish arriving. The unit never replies
// sooner than 30 ms after the end of a message.
constexpr uint32_t kOwnEchoMs = 25;
// Wired-remote messages heard while running before the master controller gives the bus back.
constexpr uint32_t kRemoteMessagesToStop = 2;
// Do not start a transmission within this long of the last received byte (the unit's reply is still coming).
constexpr uint32_t kQuietBeforeTxMs = 12;
// Abort a transmission after this many cells whose read-back differs from what was driven.
constexpr uint32_t kMaxMismatches = 3;

// Remote relay and takeover ----------------------------------------------------------------------
// Silence on the bus for this long means nobody polls and the unit has shut down (it stops about a minute after the last poll).
constexpr uint32_t kSilenceMeansOffMs = 65000;
// Time for the relay contacts to open and the remote to drop off the bus.
constexpr uint32_t kRelaySettleMs = 50;
// A manual relay test must not leave the remote isolated if its switch is forgotten.
constexpr uint32_t kRelayTestTimeoutMs = 15000;
// Takeover: the bus is isolated, so this is only a sanity check that nothing else is talking.
constexpr uint32_t kTakeoverQuietMs = 300;
// Takeover: polls before the setting, so the unit sees a normal cycle first. Untested whether it is needed; adjust by measuring.
constexpr uint32_t kTakeoverLeadMs = 1000;
// Commands arriving within this long of the first one are merged into one takeover (and spare the remote extra power cycles).
constexpr uint32_t kTakeoverMergeMs = 2000;
// A takeover waits until the remote has not sent a setting or power-off for this long (someone may be using it).
constexpr uint32_t kTakeoverUserIdleMs = 15000;
// The cut is made within this long of the end of a unit reply (the 30-58 ms gap before the next poll)...
constexpr uint32_t kTakeoverGapWindowMs = 20;
// ...but only waited for this long.
constexpr uint32_t kTakeoverGapWaitMs = 1000;
// The whole takeover is abandoned after this long.
constexpr uint32_t kTakeoverMaxMs = 15000;

uint8_t mode_code(Mode m) {
  switch (m) {
    case Mode::COOL: return fuji_passive::kModeCool;
    case Mode::HEAT: return fuji_passive::kModeHeat;
    case Mode::FAN: return fuji_passive::kModeFan;
    case Mode::AUTO: return fuji_passive::kModeAuto;
  }
  return fuji_passive::kModeAuto;
}

uint8_t fan_code(Fan f) {
  switch (f) {
    case Fan::FAN_HIGH: return fuji_passive::kFanHigh;
    case Fan::FAN_MEDIUM: return fuji_passive::kFanMedium;
    case Fan::FAN_LOW: return fuji_passive::kFanLow;
    case Fan::FAN_AUTO: return fuji_passive::kFanAuto;
  }
  return fuji_passive::kFanAuto;
}

Observed observed_from_target(const Target &t) {
  Observed o;
  o.valid = true;
  o.on = t.on;
  if (t.on) {
    o.mode_valid = true;
    o.mode = mode_code(t.mode);
    o.fan_valid = true;
    o.fan = fan_code(t.fan);
    o.temp_valid = t.mode != Mode::FAN;
    o.temp = t.temp;
    o.eco_valid = true;
    o.eco = t.eco;
  }
  return o;
}
}  // namespace

// ---- setup -----------------------------------------------------------------------------------

void Bus::connect(HardwareSerial *serial, int rx_pin, int tx_pin) {
  serial_ = serial;
  rx_pin_ = rx_pin;
  tx_pin_ = tx_pin;
  last_rx_byte_ms_ = 0;
  last_remote_message_ms_ = 0;
#ifdef ESP32
  serial_->begin(500, SERIAL_8E1, rx_pin, tx_pin);
#else
  serial_->begin(500, SERIAL_8E1);
#endif
  if (relay_pin_ >= 0) {
    pinMode(relay_pin_, OUTPUT);
    relay_set(false);  // remote connected
  }
}

void Bus::relay_set(bool cut) {
  relay_cut_.store(cut && relay_pin_ >= 0);
  if (relay_pin_ >= 0) {
    digitalWrite(relay_pin_, (cut != relay_inverted_) ? HIGH : LOW);
  }
}

void Bus::pump() {
  drain_rx();
  master_step();
  if (relay_test_active_.load() && millis() - relay_test_started_ms_.load() >= kRelayTestTimeoutMs &&
      relay_test_active_.exchange(false)) {
    relay_set(false);
    ESP_LOGW(TAG, "Relay test timed out; wired remote reconnected");
  }
  if (!master_enabled_.load()) {
    takeover_step();
    // The unit shuts down about a minute after the last poll. If nothing recognisable has been heard for longer than that
    // (nobody is polling), it has stopped, so show it off. While the remote or the master polls, replies keep arriving.
    const uint32_t last = last_message_ms_.load();
    if (last != 0 && millis() - last > kSilenceMeansOffMs) {
      observe_off();
    }
  }
}

// ---- reading ---------------------------------------------------------------------------------

void Bus::drain_rx() {
  if (serial_ == nullptr) {
    return;
  }
  uint8_t chunk[8];
  size_t chunk_len = 0;
  auto flush_chunk = [&]() {
    if (debug_ && chunk_len > 0) {
      // Same line format as the earlier firmware so tools/decode_timeline.py reads these logs too.
      char raw[25] = {};
      for (size_t i = 0; i < chunk_len; i++) {
        snprintf(raw + i * 3, sizeof(raw) - i * 3, " %02X", static_cast<unsigned>(chunk[i]));
      }
      ESP_LOGI(TAG, "RX raw (%u/8, before XOR):%s", static_cast<unsigned>(chunk_len), raw);
    }
    chunk_len = 0;
  };
  while (serial_->available() > 0) {
    const int value = serial_->read();
    if (value < 0) {
      break;
    }
    last_rx_byte_ms_ = millis();
    if (static_cast<int32_t>(last_rx_byte_ms_ - ignore_rx_until_ms_) < 0) {
      continue;  // our own transmission, read back
    }
    chunk[chunk_len++] = static_cast<uint8_t>(value);
    on_byte(static_cast<uint8_t>(value) ^ 0xFF);
    if (chunk_len == sizeof(chunk)) {
      flush_chunk();
    }
  }
  flush_chunk();
}

void Bus::publish_observed(const Observed &state) {
  if (!(state == observed())) {
    observed_word_.store(state.pack());
    observed_revision_.fetch_add(1);
  }
}

void Bus::observe_off() {
  Observed o = observed();
  if (o.valid && !o.on) {
    return;
  }
  Observed off;
  off.valid = true;
  publish_observed(off);
}

void Bus::on_byte(uint8_t value) {
  // The end of a unit reply (bus bytes `.. 48 80`), used to cut the remote off inside the gap before the next poll.
  if (value == 0x80 && prev_byte_ == 0x48) {
    unit_reply_end_ms_ = millis();
  }
  prev_byte_ = value;
  fuji_passive::Message message;
  if (!parser_.feed(value, &message)) {
    return;
  }
  const uint32_t now = millis();
  if (message.direction == fuji_passive::Direction::REMOTE_TO_UNIT) {
    last_remote_message_ms_ = now;
    remote_message_count_++;
    if (message.kind == fuji_passive::Kind::SETTING || message.kind == fuji_passive::Kind::POWER_OFF) {
      last_remote_user_ms_ = now;  // someone is using the remote
    }
  }
  if (message.kind != fuji_passive::Kind::UNKNOWN) {
    last_message_ms_.store(now);
  }

  switch (message.kind) {
    case fuji_passive::Kind::SETTING: {
      if (message.direction == fuji_passive::Direction::UNIT_TO_REMOTE) {
        unit_echo_count_++;
      }
      if (master_enabled_.load()) {
        return;  // the byte view aliases settings; the master controller publishes the state it commanded itself
      }
      fuji_passive::Setting setting;
      if (!fuji_passive::decode_setting(message.value, &setting)) {
        if (debug_) {
          ESP_LOGD(TAG, "Unmapped controller state byte: 0x%02X", message.value);
        }
        return;
      }
      Observed o;
      o.valid = true;
      o.on = true;
      o.mode_valid = true;
      o.mode = setting.mode;
      o.fan_valid = setting.fan_valid;
      o.fan = setting.fan;
      o.temp_valid = setting.temperature_valid;
      o.temp = setting.temperature;
      // The remote's message and the unit's echo carry the same state; publish once.
      publish_observed(o);
      break;
    }
    case fuji_passive::Kind::POWER_OFF:
      if (message.direction == fuji_passive::Direction::UNIT_TO_REMOTE) {
        unit_echo_count_++;
      }
      observe_off();
      break;
    case fuji_passive::Kind::STATUS:
      // Only the unit's own reply says whether the unit is running. The remote's poll carries the remote's own idea
      // (a remote that was just power-cycled polls "idle" while the unit keeps running), so it must not turn the state off.
      if (message.direction == fuji_passive::Direction::UNIT_TO_REMOTE) {
        unit_running_known_ = true;
        unit_running_ = message.running;
        unit_run_state_.store(message.running ? 1 : 0);
        // The run flag can become idle while the selected climate mode remains active (for example Fan Only).
        // Treat the explicit power-off message, not an idle status reply, as the mode change to Off.
      }
      break;
    default:
      break;
  }
}

// ---- master controller: main thread API ------------------------------------------------------

const char *Bus::master_state_text(MasterState state) {
  switch (state) {
    case MasterState::OFF: return "Off";
    case MasterState::CHECKING_BUS: return "Checking bus";
    case MasterState::RUNNING: return "Running";
    case MasterState::REFUSED_REMOTE_PRESENT: return "Refused: wired remote is on the bus";
    case MasterState::REFUSED_BUS_LOW: return "Refused: bus held low";
    case MasterState::STOPPED_REMOTE_PRESENT: return "Stopped: wired remote detected";
    case MasterState::STOPPED_TX_FAILED: return "Stopped: bus fault while sending";
    case MasterState::TAKEOVER_PENDING: return "Takeover pending: waiting for a gap in the remote's polling";
    case MasterState::TAKEOVER: return "Takeover: the remote is cut off while the change is sent";
    case MasterState::TAKEOVER_TIMEOUT: return "Takeover timed out";
  }
  return "Unknown";
}

bool Bus::request_takeover(const Target &target) {
  if (relay_test_active_.load()) {
    ESP_LOGW(TAG, "Climate command ignored while the relay test is disconnecting the wired remote");
    return false;
  }
  if (master_enabled_.load()) {
    return submit_target(target);  // already in control: either a continuous master or a takeover in progress
  }
  if (relay_pin_ < 0) {
    return false;
  }
  target_word_.store(target.pack());
  target_dirty_.store(true);
  if (!takeover_requested_.exchange(true)) {
    master_state_.store(static_cast<uint8_t>(MasterState::TAKEOVER_PENDING));
  }
  return true;
}

bool Bus::start_master() {
  if (relay_test_active_.load()) {
    ESP_LOGW(TAG, "Master controller cannot start while the relay test is active");
    return false;
  }
  if (master_enabled_.exchange(true)) {
    return false;
  }
  master_state_.store(static_cast<uint8_t>(MasterState::CHECKING_BUS));
  command_.store(kCmdStart);
  return true;
}

void Bus::stop_master() {
  if (master_enabled_.load()) {
    command_.store(kCmdStop);
  }
}

bool Bus::set_relay_test(bool cut) {
  if (cut) {
    if (relay_pin_ < 0) {
      ESP_LOGW(TAG, "Relay test unavailable: no remote relay pin is configured");
      return false;
    }
    if (master_enabled_.load() || takeover_requested_.load()) {
      ESP_LOGW(TAG, "Relay test unavailable while a master or takeover is active");
      return false;
    }
    relay_test_started_ms_.store(millis());
    relay_test_active_.store(true);
    relay_set(true);
    ESP_LOGW(TAG, "Relay test active: wired remote power and LIN should be disconnected");
    return true;
  }
  if (master_enabled_.load()) {
    ESP_LOGW(TAG, "Relay test cannot release the relay while the master is active");
    return false;
  }
  relay_test_active_.store(false);
  relay_test_started_ms_.store(0);
  relay_set(false);
  ESP_LOGI(TAG, "Relay test ended: wired remote reconnected");
  return true;
}

bool Bus::submit_target(const Target &target) {
  if (!master_enabled_.load()) {
    return false;
  }
  target_word_.store(target.pack());
  target_dirty_.store(true);
  return true;
}

// ---- master controller: bus task side --------------------------------------------------------

// Turns the latest submitted target into a scripted change once the previous one has been sent.
void Bus::apply_target() {
  if (scheduler_.busy() || !target_dirty_.exchange(false)) {
    return;
  }
  const Target target = Target::unpack(target_word_.load());
  if (target.on) {
    if (target == applied_) {
      return;
    }
    if (!scheduler_.queue_setting(target.mode, target.fan, target.temp, target.eco)) {
      ESP_LOGW(TAG, "Master: refused target mode=%u temp=%u", static_cast<unsigned>(target.mode),
               static_cast<unsigned>(target.temp));
      return;
    }
    ESP_LOGW(TAG, "Master: sending mode=%u fan=%u temp=%u eco=%u", static_cast<unsigned>(target.mode),
             static_cast<unsigned>(target.fan), static_cast<unsigned>(target.temp), static_cast<unsigned>(target.eco));
  } else {
    if (!applied_.on && !(unit_running_known_ && unit_running_)) {
      return;  // already off
    }
    scheduler_.queue_off();
    ESP_LOGW(TAG, "Master: sending power-off");
  }
  applied_ = target;
  // Not shown yet: the commanded state is published only when the setting has actually been sent (see master_step()).
  previous_observed_ = observed();
  pending_observed_ = observed_from_target(target);
  pending_valid_ = true;
}

void Bus::finish_master(MasterState state, bool keep_state) {
  scheduler_.stop();
  stage_ = Stage::IDLE;
  ack_pending_ = false;
  pending_valid_ = false;
  target_dirty_.store(false);
  master_state_.store(static_cast<uint8_t>(state));
  master_enabled_.store(false);
  takeover_ = false;
  takeover_requested_.store(false);
  takeover_armed_ = false;
  relay_set(false);  // hand the bus back to the remote (a no-op without a relay)
  // The state is not forced off: if nobody polls, the unit shuts down about a minute later and pump() then shows it off
  // after the bus has been silent that long; if the remote (or anything else) takes over polling, the unit carries on and
  // the state stays.
  ESP_LOGW(TAG, "Master controller: %s. The bus is released%s.", master_state_text(state),
           (keep_state || relay_pin_ >= 0) ? "" : "; the unit stops about a minute after the last poll unless the remote takes over");
}

bool Bus::transmit(const uint8_t *cells, size_t count, const char *what) {
  const unsigned long duration_us = static_cast<unsigned long>(count * kRemoteCellTenthsUs / 10);
  if (dry_run_.load()) {
    ESP_LOGW(TAG, "Master DRY RUN would send %s (%u cells, %lu us)", what, static_cast<unsigned>(count), duration_us);
    return true;
  }
#ifdef ESP32
  if (rx_pin_ < 0 || tx_pin_ < 0) {
    ESP_LOGE(TAG, "Master: TX/RX pins unknown");
    return false;
  }
  const gpio_num_t tx = static_cast<gpio_num_t>(tx_pin_);
  const gpio_num_t rx = static_cast<gpio_num_t>(rx_pin_);

  // Take the TX pin away from the UART and drive it as a plain GPIO, idle high (recessive).
  esp_rom_gpio_connect_out_signal(tx, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction(tx, GPIO_MODE_OUTPUT);
  gpio_set_level(tx, 1);

  uint32_t mismatches = 0;
  int first_bad = -1;
  int last_level = 1;
  const int64_t t0 = esp_timer_get_time();
  for (size_t i = 0; i < count; i++) {
    const int64_t cell_start = t0 + static_cast<int64_t>(i * kRemoteCellTenthsUs / 10);
    const int64_t cell_mid = cell_start + (kRemoteCellTenthsUs / 20);
    const int level = cells[i] ? 0 : 1;  // line level is the inverse of the bus bit
    while (esp_timer_get_time() < cell_start) {
    }
    if (level != last_level) {
      gpio_set_level(tx, level);
      last_level = level;
    }
    while (esp_timer_get_time() < cell_mid) {
    }
    if (gpio_get_level(rx) != level) {
      if (first_bad < 0) {
        first_bad = static_cast<int>(i);
      }
      if (++mismatches >= kMaxMismatches) {
        break;
      }
    }
  }
  if (mismatches < kMaxMismatches) {
    const int64_t end = t0 + static_cast<int64_t>(count * kRemoteCellTenthsUs / 10);
    while (esp_timer_get_time() < end) {
    }
  }
  gpio_set_level(tx, 1);

  // Give the pin back to the UART.
  uart_set_pin(static_cast<uart_port_t>(2), tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  if (mismatches >= kMaxMismatches) {
    ESP_LOGE(TAG, "Master: %s aborted, %u read-back mismatches (first at cell %d); line released", what,
             static_cast<unsigned>(mismatches), first_bad);
    return false;
  }
  if (first_bad >= 0) {
    ESP_LOGW(TAG, "Master: %s sent with %u read-back mismatch (cell %d)", what, static_cast<unsigned>(mismatches),
             first_bad);
  }
  return true;
#else
  (void) cells;
  ESP_LOGE(TAG, "Master controller needs an ESP32");
  return false;
#endif
}

// Starts the master controller (continuous, or a takeover that ends by itself). With a relay configured the remote is cut
// off first, in both cases.
void Bus::begin_master(bool takeover) {
  const uint32_t now = millis();
  applied_ = Target();
  ignore_rx_until_ms_ = now;
  if (!takeover) {
    unit_running_known_ = false;  // learn it from the unit's replies once polling starts
    unit_run_state_.store(-1);
  }  // a takeover keeps what the unit's replies just showed, so its first poll carries the right run flag
  ack_pending_ = false;
  pending_valid_ = false;
  last_ack_.store(0);
  scheduler_.stop();
  parser_.reset();  // drop a half-received message of the remote that was cut off
  takeover_ = takeover;
  takeover_sent_ = false;
  takeover_started_ms_ = now;
  stage_start_ms_ = now;
  if (relay_pin_ >= 0) {
    relay_set(true);
    stage_ = Stage::RELAY_SETTLE;
    ESP_LOGW(TAG, "Master%s: the remote is cut off (relay), checking that the bus is silent.",
             takeover ? " takeover" : "");
  } else {
    stage_ = Stage::WAIT_QUIET;
    ESP_LOGW(TAG, "Master%s: checking that the bus is silent for %u ms. The wired remote must be disconnected.",
             dry_run_.load() ? " (DRY RUN)" : "", static_cast<unsigned>(kQuietMs));
  }
}

// Listening mode with a relay: waits for the right moment to take over for a requested change.
void Bus::takeover_step() {
  if (!takeover_requested_.load() || relay_pin_ < 0) {
    return;
  }
  const uint32_t now = millis();
  if (!takeover_armed_) {
    takeover_armed_ = true;
    takeover_not_before_ms_ = now + kTakeoverMergeMs;  // merge commands that arrive together
    takeover_wait_since_ms_ = 0;
    return;
  }
  if (static_cast<int32_t>(now - takeover_not_before_ms_) < 0) {
    return;
  }
  // Not while someone is using the remote.
  if (last_remote_user_ms_ != 0 && now - last_remote_user_ms_ < kTakeoverUserIdleMs) {
    return;
  }
  // Cut the remote off inside the gap right after a unit reply, so the unit sees no broken message; do not wait long.
  if (takeover_wait_since_ms_ == 0) {
    takeover_wait_since_ms_ = now;
  }
  const bool in_gap = unit_reply_end_ms_ != 0 && now - unit_reply_end_ms_ < kTakeoverGapWindowMs;
  if (!in_gap && now - takeover_wait_since_ms_ < kTakeoverGapWaitMs) {
    return;
  }
  takeover_requested_.store(false);
  takeover_armed_ = false;
  master_enabled_.store(true);
  master_state_.store(static_cast<uint8_t>(MasterState::TAKEOVER));
  begin_master(true);
}

void Bus::master_step() {
  uint32_t now = millis();

  if (ack_pending_ && static_cast<int32_t>(now - ack_at_ms_) >= 0) {
    ack_pending_ = false;
    const bool acked = unit_echo_count_ != ack_baseline_;
    last_ack_.store(acked ? 1 : 2);
    if (acked) {
      ESP_LOGI(TAG, "Master: the unit acknowledged the %s (echoed it)", ack_what_);
    } else {
      ESP_LOGW(TAG, "Master: the unit did NOT acknowledge the %s (no echo seen); the shown state goes back to what it was",
               ack_what_);
      if (pending_valid_) {
        publish_observed(previous_observed_);
      }
    }
    pending_valid_ = false;
  }

  const uint32_t command = command_.exchange(0);
  if (command == kCmdStop) {
    if (master_enabled_.load()) {
      finish_master(MasterState::OFF);
    }
    return;
  }
  if (command == kCmdStart) {
    begin_master(false);
  }

  switch (stage_) {
    case Stage::IDLE:
      return;
    case Stage::RELAY_SETTLE:
      if (now - stage_start_ms_ >= kRelaySettleMs) {
        stage_ = Stage::WAIT_QUIET;
        stage_start_ms_ = now;
      }
      return;
    case Stage::WAIT_QUIET: {
      // Only messages from the wired remote count: the unit is seen sending a stray byte by itself about 5 s after
      // power-up with no remote connected, which must not block the start. When the relay has just cut the remote off,
      // its messages from before the cut are expected, so only messages after the cut count.
      const uint32_t quiet_ms = takeover_ ? kTakeoverQuietMs : kQuietMs;
      const bool traffic_before = !relay_cut_.load() && last_remote_message_ms_ != 0 &&
                                  static_cast<uint32_t>(stage_start_ms_ - last_remote_message_ms_) < quiet_ms;
      const bool traffic_since =
          last_remote_message_ms_ != 0 && static_cast<int32_t>(last_remote_message_ms_ - stage_start_ms_) > 0;
      if (traffic_before || traffic_since) {
        ESP_LOGE(TAG, "Master refused: wired remote messages seen within %u ms of starting. %s",
                 static_cast<unsigned>(quiet_ms),
                 relay_cut_.load() ? "The relay did not cut the remote off: check the relay wiring."
                                   : "Fully disconnect the wired remote (all three wires) and start again.");
        finish_master(MasterState::REFUSED_REMOTE_PRESENT);
        return;
      }
      if (now - stage_start_ms_ >= quiet_ms) {
#ifdef ESP32
        // A bus that never idles high cannot carry a message (an unpowered remote left attached clamps the line low).
        if (!dry_run_.load() && rx_pin_ >= 0 && gpio_get_level(static_cast<gpio_num_t>(rx_pin_)) == 0) {
          ESP_LOGE(TAG, "Master refused: the bus (GPIO%d) is held low with no traffic, so it does not idle high. "
                        "Disconnect the wired remote completely (all three wires) and check the LIN wire reads 12 V.",
                   rx_pin_);
          finish_master(MasterState::REFUSED_BUS_LOW);
          return;
        }
#endif
        scheduler_.start(now);
        remote_baseline_ = remote_message_count_;
        stage_ = Stage::LEAD;
        stage_start_ms_ = now;
        ESP_LOGW(TAG, "Master: bus silent, polling started (first poll has the run flag off, then it follows the "
                      "unit)");
      }
      return;
    }
    case Stage::LEAD:
      if (now - stage_start_ms_ >= (takeover_ ? kTakeoverLeadMs : kLeadMs)) {
        stage_ = Stage::RUN;
        master_state_.store(static_cast<uint8_t>(takeover_ ? MasterState::TAKEOVER : MasterState::RUNNING));
        if (!takeover_) {
          // Show off only if the unit is known to be idle (or nothing is known yet): when it is running we do not know
          // its settings, and "off" would be wrong.
          if (!observed().valid || unit_run_state_.load() == 0) {
            publish_observed(observed_from_target(applied_));
          }
          ESP_LOGW(TAG, "Master: running, polling continuously. Waiting for a command.");
        }
      }
      break;
    case Stage::RUN:
      apply_target();
      if (takeover_) {
        // A takeover ends by itself: once the change was sent and its echo checked, or if there was nothing to send.
        const bool idle = !scheduler_.busy() && !target_dirty_.load() && !ack_pending_;
        if (scheduler_.busy()) {
          takeover_sent_ = true;
        }
        if (idle && takeover_sent_ && last_ack_.load() != 0) {
          finish_master(MasterState::OFF, true);
          return;
        }
        if (idle && !takeover_sent_ && now - stage_start_ms_ >= kTakeoverMergeMs) {
          finish_master(MasterState::OFF, true);  // nothing needed sending (the unit was already in that state)
          return;
        }
      }
      break;
  }
  if (takeover_ && now - takeover_started_ms_ >= kTakeoverMaxMs) {
    ESP_LOGE(TAG, "Master: the takeover took too long, handing the bus back.");
    finish_master(MasterState::TAKEOVER_TIMEOUT, true);
    return;
  }
  if (remote_message_count_ - remote_baseline_ >= kRemoteMessagesToStop) {
    ESP_LOGE(TAG, "Master: a wired remote is talking on the bus, giving it back.");
    finish_master(MasterState::STOPPED_REMOTE_PRESENT);
    return;
  }

  if (unit_running_known_) {
    scheduler_.observe_unit_running(unit_running_);
  }
  Action action;
  if (!scheduler_.due(now, &action)) {
    return;
  }
  if (serial_->available() > 0 || (last_rx_byte_ms_ != 0 && now - last_rx_byte_ms_ < kQuietBeforeTxMs)) {
    return;  // the unit is still talking, try again next time round
  }

  uint8_t cells[kSettingCells];
  size_t count = 0;
  const char *what = "poll";
  switch (action.kind) {
    case Action::Kind::POLL:
      build_poll(action.running, action.pending, cells);
      count = kPollCells;
      what = action.pending ? "poll (pending)" : (action.running ? "poll (running)" : "poll (idle)");
      break;
    case Action::Kind::SETTING:
      if (!build_setting(action.mode, action.fan, action.temp, action.eco, action.repeat, cells)) {
        ESP_LOGE(TAG, "Master: invalid setting, stopping");
        finish_master(MasterState::STOPPED_TX_FAILED);
        return;
      }
      count = kSettingCells;
      what = action.repeat ? "setting (round 2)" : "setting (round 1)";
      break;
    case Action::Kind::OFF:
      build_off(cells);
      count = kOffCells;
      what = "power-off";
      break;
    default:
      return;
  }

  now = millis();
  if (!transmit(cells, count, what)) {
    finish_master(MasterState::STOPPED_TX_FAILED);
    return;
  }
  scheduler_.sent(now);
  ignore_rx_until_ms_ = millis() + kOwnEchoMs;
  if (action.kind != Action::Kind::POLL) {
    ESP_LOGW(TAG, "Master: sent %s", what);
  }
  // The echo is looked for from the first message of a change on, so a missing echo of round 2 alone does not count as a failure.
  if (action.kind == Action::Kind::OFF || (action.kind == Action::Kind::SETTING && !action.repeat)) {
    ack_baseline_ = unit_echo_count_;
  }
  // The final message of a change (round 2, or the power-off) has been sent: show the new state and start waiting for the echo.
  if (action.kind == Action::Kind::OFF || (action.kind == Action::Kind::SETTING && action.repeat)) {
    if (pending_valid_) {
      publish_observed(pending_observed_);
    }
    ack_at_ms_ = millis() + kAckWaitMs;
    ack_pending_ = true;
    ack_what_ = action.kind == Action::Kind::OFF ? "power-off" : "setting";
  }
}

}  // namespace fuji_aot30
