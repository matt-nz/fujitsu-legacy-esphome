#pragma once

// ESPHome climate entity for the AOT30L/ART30RL bus (see Aot30Bus.h and docs/aot30-master-controller.md).
//
// The climate entity shows what the bus tells us while a real wired remote is connected (best effort), and is the
// control surface of the master controller: while it runs, climate commands become remote messages. Bus Alive,
// Bus Status, Unit Running and the master controller switch/status are exposed as accessors for template entities
// in the YAML, so this component needs no sub-platforms.

#include "esphome/components/climate/climate.h"
#include "esphome/core/component.h"

#include <string>

#include "Aot30Bus.h"

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome {
namespace fujitsu_aot30 {

class Aot30Climate : public climate::Climate, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Start the bus task before WiFi association: the ESP32 and the unit boot together on a shared 12 V rail.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

  void set_rx_pin(int pin) { rx_pin_ = pin; }
  void set_tx_pin(int pin) { tx_pin_ = pin; }
  void set_debug(bool debug) { debug_ = debug; }
  // Optional relay output that cuts the wired remote's +12 V and LIN while the ESP is in control (see Aot30Bus.h).
  void set_remote_relay_pin(int pin) { relay_pin_ = pin; }
  void set_remote_relay_inverted(bool inverted) { relay_inverted_ = inverted; }

  // The ESP takes over the wired remote's role (continuous polling; climate commands become remote messages).
  // The wired remote must be fully disconnected.
  void set_master_controller(bool enabled);
  bool master_controller_active() const { return bus_.master_active(); }
  std::string master_controller_status() const {
    const fuji_aot30::MasterState state = bus_.master_state();
    if (state == fuji_aot30::MasterState::OFF && bus_.takeover_available()) {
      return "Listening (the remote is in charge)";
    }
    return fuji_aot30::Bus::master_state_text(state);
  }
  // Did the unit echo the last setting / power-off we sent? Echoing is how it acknowledges what it accepts.
  std::string master_last_command() const {
    switch (bus_.last_ack()) {
      case 1: return "Acknowledged by the unit";
      case 2: return "Not acknowledged by the unit";
      default: return "None yet";
    }
  }
  // Run flag from the unit's replies: -1 unknown, 0 idle, 1 running.
  int unit_run_state() const { return bus_.unit_run_state(); }
  // True while recognised messages are still arriving.
  bool bus_alive() const;
  std::string bus_status() const;

  fuji_aot30::Bus bus_;  // public for the bus task

 private:
  void update_state_();
  fuji_aot30::Target current_target_() const;

  static constexpr uint32_t BUS_TIMEOUT_MS = 4000;
  int rx_pin_ = 16;
  int tx_pin_ = 17;
  int relay_pin_ = -1;
  bool relay_inverted_ = false;
  bool debug_ = false;
  uint32_t seen_revision_ = 0;
  bool state_initialized_ = false;
#ifdef ESP32
  TaskHandle_t task_handle_ = nullptr;
#endif
};

}  // namespace fujitsu_aot30
}  // namespace esphome
