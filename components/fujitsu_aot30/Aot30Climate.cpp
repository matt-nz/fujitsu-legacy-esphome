#include "Aot30Climate.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace fujitsu_aot30 {

static const char *const TAG = "aot30.climate";

#ifdef ESP32
// Dedicated bus task: transmissions busy-wait for up to about 165 ms and must not be interrupted by the main loop.
static void aot30_bus_task(void *pv) {
  auto *climate = static_cast<Aot30Climate *>(pv);
  for (;;) {
    climate->bus_.pump();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
#endif

void Aot30Climate::setup() {
  this->target_temperature = NAN;
  this->mode = climate::CLIMATE_MODE_OFF;
  this->fan_mode = climate::CLIMATE_FAN_AUTO;
  this->preset = climate::CLIMATE_PRESET_NONE;
  this->action = climate::CLIMATE_ACTION_OFF;

#ifdef ESP32
  // Pull-up on the RX pin: a floating pin crashes the ESP32 when it is powered but not wired to the bus.
  pinMode(this->rx_pin_, INPUT_PULLUP);
  // Hold the transceiver's TXD recessive (high) from here on. A floating or reconfiguring TX pin can pull the bus low
  // for a moment while the ESP32 starts. An external pull-up on the transceiver's TXD covers the time before this line.
  pinMode(this->tx_pin_, OUTPUT);
  digitalWrite(this->tx_pin_, HIGH);

  this->bus_.set_debug(this->debug_);
  this->bus_.set_remote_relay(this->relay_pin_, this->relay_inverted_);  // connect() drives it to "remote connected"
  this->bus_.connect(&Serial2, this->rx_pin_, this->tx_pin_);
  ESP_LOGI(TAG, "AOT30 bus on Serial2, rx=GPIO%d tx=GPIO%d%s", this->rx_pin_, this->tx_pin_,
           this->relay_pin_ >= 0 ? " (remote relay configured: takeover mode available)" : "");

  xTaskCreatePinnedToCore(aot30_bus_task, "Aot30Bus", 10000, (void *) this, configMAX_PRIORITIES - 1,
                          &(this->task_handle_), 1);
#else
  ESP_LOGE(TAG, "fujitsu_aot30 requires an ESP32 (FreeRTOS task + Serial2)");
#endif
}

void Aot30Climate::loop() { this->update_state_(); }

void Aot30Climate::dump_config() {
  ESP_LOGCONFIG(TAG, "Fujitsu AOT30L/ART30RL climate:");
  ESP_LOGCONFIG(TAG, "  RX pin: GPIO%d  TX pin: GPIO%d", this->rx_pin_, this->tx_pin_);
  if (this->relay_pin_ >= 0) {
    ESP_LOGCONFIG(TAG, "  Remote relay: GPIO%d%s (cuts the remote's +12 V and LIN during a takeover)", this->relay_pin_,
                  this->relay_inverted_ ? " (inverted)" : "");
  }
  ESP_LOGCONFIG(TAG, "  Debug (RX raw log lines): %s", this->debug_ ? "yes" : "no");
}

bool Aot30Climate::bus_alive() const {
  const uint32_t last = this->bus_.last_message_ms();
  return last != 0 && (millis() - last) < BUS_TIMEOUT_MS;
}

std::string Aot30Climate::bus_status() const {
  if (this->bus_alive()) {
    return "Bus OK";
  }
  return this->bus_.last_message_ms() != 0 ? "No Signal" : "Never Seen";
}

void Aot30Climate::set_master_controller(bool enabled) {
  if (enabled) {
    if (!this->bus_.start_master()) {
      ESP_LOGW(TAG, "Master controller is already running");
    }
  } else {
    this->bus_.stop_master();
  }
}

// Publishes the observed state (what the bus showed, or what the master controller commanded) when it changes.
void Aot30Climate::update_state_() {
  const uint32_t revision = this->bus_.observed_revision();
  if (this->state_initialized_ && revision == this->seen_revision_) {
    return;
  }
  this->seen_revision_ = revision;
  const fuji_aot30::Observed o = this->bus_.observed();
  if (!o.valid) {
    return;
  }
  this->state_initialized_ = true;

  if (!o.on) {
    this->mode = climate::CLIMATE_MODE_OFF;
    this->action = climate::CLIMATE_ACTION_OFF;
  } else {
    this->action = climate::CLIMATE_ACTION_IDLE;
    if (o.mode_valid) {
      switch (o.mode) {
        case fuji_passive::kModeHeat: this->mode = climate::CLIMATE_MODE_HEAT; break;
        case fuji_passive::kModeCool: this->mode = climate::CLIMATE_MODE_COOL; break;
        case fuji_passive::kModeFan: this->mode = climate::CLIMATE_MODE_FAN_ONLY; break;
        case fuji_passive::kModeAuto: this->mode = climate::CLIMATE_MODE_AUTO; break;
        default: break;
      }
    }
    if (o.fan_valid) {
      switch (o.fan) {
        case fuji_passive::kFanLow: this->fan_mode = climate::CLIMATE_FAN_LOW; break;
        case fuji_passive::kFanMedium: this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
        case fuji_passive::kFanHigh: this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
        default: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
      }
    }
    if (o.eco_valid) {
      this->preset = o.eco ? climate::CLIMATE_PRESET_ECO : climate::CLIMATE_PRESET_NONE;
    }
    if (o.temp_valid) {
      this->target_temperature = static_cast<float>(o.temp);
    } else if (o.mode_valid && o.mode == fuji_passive::kModeFan) {
      this->target_temperature = NAN;
    }
  }
  this->publish_state();
}

climate::ClimateTraits Aot30Climate::traits() {
  auto traits = climate::ClimateTraits();
  // This unit (ART30RL ducted) has no Dry mode, no Quiet fan speed and no vanes.
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_AUTO,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });
  traits.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
  });
  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_ECO,
  });
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(18.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(1.0f);
  return traits;
}

// The state to fold a climate call into when the ESP is not the continuous master: what the entity currently shows (the
// last commanded state, or what a connected remote last sent), so a change made on the remote is not reverted by an
// unrelated Home Assistant command.
fuji_aot30::Target Aot30Climate::current_target_() const {
  using fuji_aot30::Fan;
  using fuji_aot30::Mode;
  fuji_aot30::Target t = this->bus_.last_target();  // keeps what the entity does not show (for example the last mode while off)
  t.on = this->mode != climate::CLIMATE_MODE_OFF;
  switch (this->mode) {
    case climate::CLIMATE_MODE_HEAT: t.mode = Mode::HEAT; break;
    case climate::CLIMATE_MODE_COOL: t.mode = Mode::COOL; break;
    case climate::CLIMATE_MODE_FAN_ONLY: t.mode = Mode::FAN; break;
    case climate::CLIMATE_MODE_HEAT_COOL:
    case climate::CLIMATE_MODE_AUTO: t.mode = Mode::AUTO; break;
    default: break;
  }
  if (!std::isnan(this->target_temperature)) {
    t.temp = static_cast<uint8_t>(lroundf(this->target_temperature));
  }
  if (this->fan_mode.has_value()) {
    switch (this->fan_mode.value()) {
      case climate::CLIMATE_FAN_HIGH: t.fan = Fan::FAN_HIGH; break;
      case climate::CLIMATE_FAN_MEDIUM: t.fan = Fan::FAN_MEDIUM; break;
      case climate::CLIMATE_FAN_LOW: t.fan = Fan::FAN_LOW; break;
      default: t.fan = Fan::FAN_AUTO; break;
    }
  }
  t.eco = this->preset.has_value() && this->preset.value() == climate::CLIMATE_PRESET_ECO;
  return t;
}

// Folds the climate call into the last requested target and hands it to the bus task.
void Aot30Climate::control(const climate::ClimateCall &call) {
  using fuji_aot30::Fan;
  using fuji_aot30::Mode;
  const bool master = this->bus_.master_active();
  if (!master && !this->bus_.takeover_available()) {
    ESP_LOGW(TAG, "Climate command ignored: turn on the Master Controller (wired remote disconnected) first");
    return;
  }
  fuji_aot30::Target target = master ? this->bus_.last_target() : this->current_target_();

  if (call.get_mode().has_value()) {
    switch (call.get_mode().value()) {
      case climate::CLIMATE_MODE_OFF: target.on = false; break;
      case climate::CLIMATE_MODE_HEAT: target.on = true; target.mode = Mode::HEAT; break;
      case climate::CLIMATE_MODE_COOL: target.on = true; target.mode = Mode::COOL; break;
      case climate::CLIMATE_MODE_HEAT_COOL:
      case climate::CLIMATE_MODE_AUTO: target.on = true; target.mode = Mode::AUTO; break;
      case climate::CLIMATE_MODE_FAN_ONLY: target.on = true; target.mode = Mode::FAN; break;
      default:
        ESP_LOGW(TAG, "This unit has no such mode, ignoring the command");
        return;
    }
  }
  if (call.get_target_temperature().has_value()) {
    target.temp = static_cast<uint8_t>(lroundf(call.get_target_temperature().value()));
  }
  if (call.get_fan_mode().has_value()) {
    switch (call.get_fan_mode().value()) {
      case climate::CLIMATE_FAN_HIGH: target.fan = Fan::FAN_HIGH; break;
      case climate::CLIMATE_FAN_MEDIUM: target.fan = Fan::FAN_MEDIUM; break;
      case climate::CLIMATE_FAN_LOW:
      case climate::CLIMATE_FAN_QUIET: target.fan = Fan::FAN_LOW; break;
      default: target.fan = Fan::FAN_AUTO; break;
    }
  }
  if (call.get_preset().has_value()) {
    target.eco = call.get_preset().value() == climate::CLIMATE_PRESET_ECO;
  }
  if (target.on && target.mode != Mode::FAN && !fuji_aot30::temp_in_range(target.mode, target.temp)) {
    // HEAT 16-30, COOL and AUTO 18-30 (measured); clamp rather than drop the command.
    const uint8_t low = target.mode == Mode::HEAT ? 16 : 18;
    target.temp = target.temp < low ? low : 30;
  }
  const bool sent = master ? this->bus_.submit_target(target) : this->bus_.request_takeover(target);
  if (!sent) {
    ESP_LOGW(TAG, "Command not sent (the master controller stopped or no relay is configured)");
  }
}

}  // namespace fujitsu_aot30
}  // namespace esphome
