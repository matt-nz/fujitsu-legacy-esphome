# Fujitsu Legacy Wired Remote for ESPHome

ESPHome external component for the older Fujitsu ducted **wired-remote protocol**
reverse engineered on an **ART30RL indoor unit / AOT30L outdoor unit**, with wired remote
part number **9364911014**.

**Experimental.** Compatibility depends on the controller and protocol, not the outdoor
unit's model or capacity.

## Compatibility and identifying the protocol

| Hardware | Compatibility evidence |
|---|---|
| ART30RL indoor / AOT30L outdoor, remote 9364911014 | Reference installation used for protocol captures and control experiments; the component remains experimental |
| AOT20 and other related models | Plausible candidates, **not verified**; neither the model prefix nor capacity establishes compatibility |
| Systems using the separate Fujitsu 8-byte protocol | Not supported by this component |

"Legacy wired remote" is a descriptive name, not an official Fujitsu series.
The identifier `fujitsu_aot30` reflects the reference hardware, not a model restriction.

### How this differs from the 8-byte protocol

Similar wiring does **not** make these protocols interchangeable:

| Characteristic | Legacy wired-remote protocol (this project) | Separate 8-byte protocol (original project) |
|---|---|---|
| Wiring and speed | Three wires: nominal 12 V, ground and LIN-style data; roughly 500 baud | Also uses a LIN-style interface at roughly 500 baud; wiring/speed alone do not identify the protocol |
| Message format | Variable-length messages beginning with `7F` in the inverted byte view: polls, replies, settings and off messages | Fixed 8-byte frames |
| Bit framing | 11 cells per byte; the extra cell is position-dependent, not ordinary UART parity. Settings are encoded at cell level | Uses the original project's separate UART/frame implementation |
| Controller role | Wired remote is the master and polls about every 247 ms; the indoor unit replies | Original integration joins as a secondary controller alongside the wired remote |
| Dual-remote / secondary-controller operation | **Not supported by this implementation; no second-controller addressing or arbitration has been identified** | Secondary-controller operation is supported by the original integration |
| Settings feedback | Normal unit replies report a run flag, not a full settings snapshot; accepted commands are echoed | Original integration decodes settings from its 8-byte frame exchange |
| Available features | Heat, Cool, Auto, Fan, fan speed and Economy on the reference installation; no room temperature has been identified on this bus | Different field layout and feature set; its decoder cannot be reused here |

**Only one active master is supported.** Disconnect the wired remote completely before
enabling the ESP master, or use relay takeover to isolate its power and LIN temporarily.
Takeover is time-sharing, **not dual-remote support**; the remote does not mirror HA settings
and a button press can restore its own stored settings.

Continuous polling is required: the reference unit stopped about a minute after polling
ceased with the compressor off. Other operating conditions remain unverified.

For another model, compare **receive-only captures** with the
[protocol analysis](docs/aot30-protocol-analysis.md) before transmitting.

## Quick start

1. Follow the [circuit guide](docs/aot30-circuit.md) for the ESP32 and LIN transceiver.
2. Use [`aot30.yaml`](aot30.yaml) with the local [`components/`](components/) directory.
3. Configure the template below, validate it in ESPHome, then follow the
   [controller guide](docs/aot30-master-controller.md) to start safely.

## Basic configuration

Start with the complete [`aot30.yaml`](aot30.yaml) template rather than only the
component block: it also defines the Master Controller switch and web controls.

| Setting | Template default / action |
|---|---|
| Device | `aircon`, ESP32 `esp32dev`, Arduino framework; adjust for your board |
| Bus pins | `rx_pin: 16` from transceiver RXD; `tx_pin: 17` to transceiver TXD |
| Setpoint range | Home Assistant climate controls use 18-30°C in 1°C steps; target temperature and fan speed can be staged while Off, then applied when a compatible mode is selected |
| Master Controller | `RESTORE_DEFAULT_OFF`: first boot is passive; once enabled, restores On across reboots. An intentional Off is also saved |
| Relay takeover | Leave unconfigured during passive bring-up. Later set `remote_relay_pin` and `remote_relay_inverted` to match your driver; HA commands can then trigger transmission even with the master switch Off |
| Logging | `logger.level: INFO`, climate `debug: false`; commands, acknowledgements, warnings and errors remain enabled |
| Network outages | WiFi/API `reboot_timeout: 0s` keeps polling without connectivity-triggered reboots |
| Status LED | Optional GPIO2 warning/error indicator; remove if unsuitable for your board |

Master On starts polling, **not heating/cooling or restoration of the previous
target**. Use the climate controls to select operation; climate Off sends an off
command, while disabling the master only stops polling.

### Local secrets

Create `secrets.yaml` next to your device YAML, replacing every placeholder:

```yaml
wifi_ssid: "YOUR_WIFI_NAME"
wifi_password: "YOUR_WIFI_PASSWORD"
api_encryption_key: "YOUR_BASE64_ENCODED_32_BYTE_KEY"
fallback_ap_password: "YOUR_UNIQUE_AP_PASSWORD"
web_username: "YOUR_WEB_USERNAME"
web_password: "YOUR_UNIQUE_WEB_PASSWORD"
```

Generate the API key locally with `openssl rand -base64 32`; Home Assistant needs
the same key. Use strong, unique passwords (8-63 characters for the fallback AP).
The local secrets file is gitignored; do not commit credentials.

The template uses encrypted API access for OTA authentication and an
authenticated web UI. The web UI is still **unencrypted HTTP**: keep it on a
trusted LAN and do not expose it to the internet. OTA safe-mode recovery and the
fallback AP/captive portal remain enabled.

See [configuration details](docs/aot30-master-controller.md#template-secrets-and-network-behaviour)
for network-recovery trade-offs and operating modes.

### First-time bring-up: listen before controlling

1. **Validate and flash.** Set your board, pins and secrets, then validate the
   complete YAML in ESPHome. Keep Master Controller Off and leave the relay option
   unconfigured. A previously used device may restore a saved On: check its saved
   state with the bus disconnected before attaching it to a live remote.
2. **Check wiring.** Follow the circuit guide, check supply and GPIO voltages,
   then connect the transceiver with the original remote still in charge.
   Never connect the nominal 12 V LIN signal directly to an ESP GPIO.
3. **Confirm reception.** Expect `Bus Alive` on and `Bus Status` = `Bus OK`.
   Operate the physical remote and check `Unit Running` follows the unit's on/off
   replies. Mode/temperature/fan monitoring is best effort; UART aliasing means
   some settings are ambiguous or unmapped, not necessarily a wiring fault.
4. **Capture raw traffic if needed.** Change the climate component's
   `debug: false` to `debug: true`, reflash and open ESPHome's device logs
   (wireless or USB serial at 115200 baud). `RX raw` lines are logged at INFO and
   contain bytes **before inversion**. For additional unmapped-state messages,
   also change `logger.level` to `DEBUG`. The web UI's log stream is disabled
   (`web_server.log: false`). Restore `debug: false` and INFO after investigation.
5. **Take control deliberately.** Without a relay, disconnect the wired remote
   completely before enabling Master Controller in HA or the web UI. For relay
   operation, first verify that the contacts isolate both remote power and LIN.
   Wait for status `Running`; do not bypass refusals or bus faults.
6. **Test a command and recovery.** Start with a simple Fan command, check
   `Master Last Command` and actual unit behaviour, then test climate Off.
   An echo alone is not full state verification. Once settings are saved,
   verify a reboot restores Master On and polling with the remote still isolated.
   It does not restore the previous heating/cooling target.

No recognised traffic? Check transceiver power, common ground, RX/TX wiring,
normal-mode enable and pin assignments before attempting control. For another
model, compare captures with the protocol reference first.

## Validation

Run host-side tests from the repository root:

```sh
sh tests/run_all.sh
```

Success prints `ALL HOST TESTS PASSED`. These are host tests, not hardware validation.

## Core docs

- [Protocol field reference](docs/aot30-protocol.md) - mode, temperature, fan and command values
- [Protocol analysis](docs/aot30-protocol-analysis.md) - timing, framing, evidence and open questions
- [Controller guide](docs/aot30-master-controller.md) - operation, relay takeover and limitations
- [Circuit guide](docs/aot30-circuit.md) - wiring and relay hardware
- [Touchscreen design](docs/aot30-touchscreen-controller.md) - proposed local controller

## Acknowledgements

This work was inspired and guided by earlier Fujitsu integrations and their
reverse-engineering and hardware-interface documentation:

- [endofthewurst/Retro-Fujitsu-ESPHome](https://github.com/endofthewurst/Retro-Fujitsu-ESPHome) -
  the original project this work started from, including ESPHome integration and
  TJA1021 / prebuilt LIN-board wiring guidance. This project's reference circuit
  instead uses an MCP2021-330.
- [unreality/FujiHeatPump](https://github.com/unreality/FujiHeatPump) -
  Arduino control library and protocol work used by the earlier integration.
- [jaroslawprzybylowicz/fuji-iot](https://github.com/jaroslawprzybylowicz/fuji-iot) -
  Fujitsu / Fuji Electric interface and Raspberry Pi implementation reference.
- [FujiHeatPump/esphome-fujitsu](https://github.com/FujiHeatPump/esphome-fujitsu) -
  earlier ESPHome custom-component implementation.
- [Reverse-engineering a Fujitsu air conditioner](https://hackaday.io/project/19473-reverse-engineering-a-fujitsu-air-conditioner-unit) -
  foundational protocol and hardware-interface investigation by Frederic Germain and Myles Eftos.

These are historical influences, not claims that their protocols or circuits are
interchangeable with this unit. The legacy message format here was derived from
the reference installation's captures. The circuit guide distinguishes the
reported MCP2021-330 wiring from suggested additions and datasheet caveats.
