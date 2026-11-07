# Legacy wired-remote master controller

Status: **experimental**. Protocol captures and manual ESP/remote hand-back tests
were performed on ART30RL / AOT30L hardware on 2-3 Nov 2026. The current component
has host tests; this does not establish that the latest firmware or automatic
relay takeover has been validated on hardware.

The `fujitsu_aot30` identifier names the reference hardware, not a verified
compatibility boundary. See the [compatibility comparison](../README.md#compatibility-and-identifying-the-protocol),
[protocol field reference](aot30-protocol.md) and
[in-depth analysis](aot30-protocol-analysis.md).

## What it does

The ESP replaces the wired remote as bus master: it polls the unit continuously
and sends settings or off commands. It cannot join as a second active remote.
There are two operating arrangements:

- **Permanent replacement:** disconnect the wired remote completely (LIN, power
  and ground) before starting the ESP master.
- **Optional relay takeover:** normally leave the remote connected and the ESP
  listening. Isolate the remote's power and LIN while the ESP controls the unit,
  then reconnect it. Only one master is active at a time.

An unpowered but LIN-connected remote clamps the bus low (about 2.6 V measured).
Cutting power alone is therefore not sufficient. See the
[circuit guide](aot30-circuit.md) before wiring.

## Installing and starting

Use [`aot30.yaml`](../aot30.yaml) with
[`components/fujitsu_aot30/`](../components/fujitsu_aot30/) next to it.
RX defaults to GPIO16 and TX to GPIO17 through a LIN transceiver.
The example uses INFO logging with raw bus logging disabled (`debug: false`).
Command, acknowledgement, warning and error messages remain enabled. Set
`debug: true` temporarily to capture raw received bytes; use logger level DEBUG
as well if unmapped-state diagnostics are needed.

The example uses ESPHome's native status LED for warning/error indication instead
of custom WiFi/API light effects. It omits heap telemetry, repository/LED-pattern
entities and periodic status-log summaries; WiFi signal, uptime and bus/controller
diagnostics remain available.

Start with the remote in charge, Master Controller Off and no relay pin
configured. Follow the [passive bring-up checklist](../README.md#first-time-bring-up-listen-before-controlling)
to verify reception and capture raw logs before attempting transmission.
With a relay configured, HA commands can start a takeover even while the master
switch is Off; Off alone is therefore not a receive-only guarantee in that setup.

After passive checks, without a relay:

1. Disconnect the wired remote completely and power the unit.
2. Enable **Master Controller** in HA or the web UI. It defaults to off when no
   saved switch state exists and otherwise restores its saved state. It checks for 1.5 s of bus silence and idle-high
   line level, then polls for 2 s before normal operation.
3. Use the climate entity; a setting takes about 1.5 s to transmit in two rounds.
4. Disable **Master Controller** to stop polling. This does **not** send an off
   command. Use the climate entity's Off command while the master is active to
   request an immediate commanded shutdown.

The unit stopped about one minute after the last poll in the test with the
compressor not running. Timing in other operating conditions is not established.
If a remote resumes polling, the unit can keep running instead. The firmware
infers off for the displayed climate state after 65 s of bus silence; that is
not an additional hardware measurement. WiFi loss is not itself loss of polling,
but a reboot or OTA update can interrupt it.

## Template secrets and network behaviour

The example is a template, not a deployable configuration with shared passwords.
Create a local `secrets.yaml` next to your ESPHome YAML with these entries, replacing
every placeholder before validating or flashing:

```yaml
wifi_ssid: "YOUR_WIFI_NAME"
wifi_password: "YOUR_WIFI_PASSWORD"
api_encryption_key: "YOUR_BASE64_ENCODED_32_BYTE_KEY"
fallback_ap_password: "YOUR_UNIQUE_AP_PASSWORD"
web_username: "YOUR_WEB_USERNAME"
web_password: "YOUR_UNIQUE_WEB_PASSWORD"
```

Generate a fresh API key locally with `openssl rand -base64 32` and supply the
same key when adding the device to Home Assistant. Use unique, strong passwords;
the fallback AP password must be 8-63 characters. Do not commit the secrets file.

API traffic is encrypted; OTA updates use the API encryption key for
authentication. The web UI requires authentication but still uses unencrypted
HTTP, so restrict it to a trusted LAN and do not expose it to the internet. The
fallback AP and captive portal remain
available for WiFi recovery.

WiFi and API `reboot_timeout` are disabled so loss of WiFi or Home Assistant does
not by itself reboot the bus controller and interrupt polling. The trade-off is
that these connectivity watchdogs will not reboot it to recover a stuck network
connection. OTA's automatic safe-mode support remains enabled with its defaults;
the redundant explicit safe-mode block has been removed.

The native GPIO2 status LED is optional: remove `status_led` if your board has no
suitable LED or GPIO2 is needed elsewhere.

The template uses `RESTORE_DEFAULT_OFF` for **Master Controller**: a new device
starts listening only. Once enabled and saved, On is restored across reboots;
it does not revert to Off merely because it rebooted. An intentional Off is
also saved. Allow ESPHome's preference write interval before removing power
(normally up to one minute); an immediate power cut may lose a recent change.
Faults/refusals still stop the master and the synchronised switch can save that
Off state; inspect and resolve the fault before re-enabling it. There is no
automatic retry that overrides the bus safety checks. Master On starts polling;
it does not itself restore a previous heating/cooling target.

## Home Assistant entities

| Entity | Purpose |
|---|---|
| **Master Controller** | Enables continuous ESP master operation; restored across reboots. With a relay, keeps the remote isolated until disabled or the master stops. |
| **Master Controller Status** | Running, listening, takeover progress, or a refusal/stop reason. |
| **Master Last Command** | Whether a recognised unit setting/off echo was observed during the acknowledgement window; not a full field-by-field state readback. |
| **Unit Running** | Run flag from the unit's own replies; unknown until a reply has been received. Not compressor activity. |
| **Bus Alive / Bus Status** | Recognised bus traffic is arriving, including while listening to the physical remote. |
| **Fujitsu Heat Pump** | Climate control: Off, Auto, Cool, Heat, Fan; fan Auto/Low/Medium/High; Economy preset. |
| **Mode / Set Temperature / Fan Speed / Economy Mode** | Additional controls for the ESPHome web UI and HA Auto-mode temperature selection. |

The climate entity starts with a 23 C target and retains the selected target
while Off or in Fan Only so it can be adjusted before choosing an operating
mode. Fan speed is also staged while Off. The staged temperature applies when
Heat, Cool or Auto is selected; fan speed is applied when any operating mode is
selected. In Fan Only mode, fan-speed changes apply immediately; Fan mode
itself has no active temperature setpoint. Supported setpoints: Heat 16-30 C,
Cool/Auto 18-30 C, whole degrees; out-of-range setpoints are clamped by the
climate adapter.
Dry, Quiet and vane control are not offered on the reference installation.
No room-temperature field has been identified on this bus.

Without an active master or a configured takeover relay, HA commands are
rejected with a warning. While listening, remote settings are a best-effort
UART decode; they are not an exact readback of every possible setting.

## Relay takeover: implemented, hardware validation outstanding

Configure `remote_relay_pin` and, for an active-low driver,
`remote_relay_inverted` on the climate platform. Use normally-closed contacts
in the remote's power and LIN legs so the unenergised relay connects the remote.
The firmware drives the relay to "remote connected" at setup and on hand-back.
This does not guarantee recovery from every firmware crash that leaves the GPIO
asserted.

With **Master Controller off**, an HA command starts a temporary takeover:

1. Merge commands for 2 s and wait until the remote has not sent a setting/off
   message for 15 s.
2. Wait up to 1 s for a gap after a unit reply, then isolate the remote.
3. Allow 50 ms for relay settling and check for 300 ms of silence.
4. Poll for 1 s, carrying the last observed unit run flag.
5. Send the setting/off command and check for a unit echo.
6. Stop transmitting and reconnect the remote.

Once isolation starts, the active takeover has a 15 s deadline; the earlier
merge/user-idle wait is separate. Timings are constants in
[`Aot30Bus.cpp`](../components/fujitsu_aot30/Aot30Bus.cpp), not hardware-certified
values. Status reports `Takeover pending`, `Takeover` or
`Listening (the remote is in charge)`.

With **Master Controller on**, the relay stays isolated and the ESP is the
permanent master. Turning the switch off releases the relay without forcing
the climate display to off.

### Remote return: established behaviour

Manual tests on 3 Nov resolved the earlier hand-back questions:

- **LIN-only interruption is unsuitable:** reconnecting a still-powered remote
  made it re-send its settings and overwrite the unit's state.
- **Power-and-LIN interruption worked in the memory-enabled test:** the ESP
  left the unit in Heat/high while the remote held Fan/low. On reconnect the
  remote sent only idle polls and the unit continued running. A button then
  sent the remote's stored Fan/low setting.
- **Enable the remote's memory / restore-after-power-failure function** for the
  tested retained-setting behaviour. The earlier COOL 23 default was observed
  without that retention; it is not an unavoidable takeover result.
- The remote shows **`EE:EE`**, identified by the operator as its power-failure
  indication. It continues polling; a button clears the display. Reconnect order
  did not remove the indication.
- The remote does not learn HA's mode or setpoint. Its first button action can
  therefore replace HA's target with its own stored settings.
- Button presses while the remote is unpowered cannot be captured.

These are manual test results, not proof of the complete automatic relay
sequence or long-term power-cycle durability.

## Safety checks and state reporting

| Status / behaviour | Meaning |
|---|---|
| `Refused: wired remote is on the bus` | Remote messages were heard during the start check. |
| `Refused: bus held low` | The line did not idle high. Check wiring and remote isolation. |
| `Stopped: wired remote detected` | Two remote messages were heard while running; the ESP stops and releases the relay. |
| `Stopped: bus fault while sending` | Three cell read-back mismatches caused transmission to abort and the line to be released. |

The ESP avoids transmitting within 12 ms of the last received byte. Each
transmitted cell is read back, but this checks the line, not the unit's resulting
mode or temperature.

The climate setting is published after the final transmission and rolled back
if no recognised echo is seen. Echo presence is not an independent decode of the
returned fields. The run flag comes from the unit, not the remote's polls.
Stopping the master does not itself force off; the remote can continue polling.

## Tests and remaining work

Run `sh tests/run_all.sh` from the repository root. It checks 677 encoder vectors,
the scheduler and bus-controller behaviour using fake time, GPIO and UART.
It does not build ESPHome firmware or exercise a real relay/heat pump.

The consolidated [remaining-questions table](aot30-protocol-analysis.md#remaining-unknowns-and-validation-work)
distinguishes unresolved protocol questions from missing hardware validation.
It also records [questions already answered](aot30-protocol-analysis.md#questions-already-answered),
replacing the superseded "test this next" proposals from the early investigation.

### Future options, not implemented

- **Exact passive monitor:** GPIO edge timestamps or RMT reception to decode the
  setting cells without UART aliasing.
- **Two-transceiver bridge:** ESP master towards the unit, reply emulator towards
  the remote, cell-level reception, hardware-timed transmission and arbitration.
  Synthetic replies and display synchronisation remain untested. This is not
  what relay takeover implements.
- **Local touchscreen master:** use the current climate interface for an LVGL UI.
  Hardware/UI and transmit scheduling need validation; see the
  [touchscreen design](aot30-touchscreen-controller.md).
- **Virtual remote button presses:** an alternative requiring modification of the
  remote, key-contact isolation and accurate feedback; not implemented.
