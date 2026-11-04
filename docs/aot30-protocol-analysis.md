# Legacy wired-remote protocol: analysis and evidence

The wired-remote bus of the older ducted Fujitsu units: indoor ART30RL, outdoor AOT30L, wired remote part 9364911014, three wires (LIN, power,
ground). This is the detailed analysis behind the [protocol field reference](aot30-protocol.md). It is reverse engineered from logic-analyser and ESP32 captures, not from a vendor
specification, and every statement carries its evidence level (see [Evidence](#evidence)). The firmware that uses this is described in
[aot30-master-controller.md](aot30-master-controller.md).

This bus is **not** the separate 8-byte frame protocol used by newer units.
The model names here identify the reference installation, not an exhaustive compatibility list.
Other models, including AOT20, remain unverified; see the
[compatibility summary](../README.md#compatibility-and-identifying-the-protocol).

## 1. Physical layer

- Single-wire LIN-style bus, 12 V, **idle high (recessive)**. The reference ESP32 connects through an MCP2021-330 LIN transceiver; RX and TX are plain 3.3 V UART
  levels on its other side.
- **The line is inverted:** a bus bit `1` is the line low. Everything below is in bus bits (line level inverted) unless it says "line".
- Two transmitters with their own clocks (measured from falling edge to falling edge of plain bytes):

  | Sender | Bit time | Rate |
  |---|---|---|
  | Wired remote | **1980.8 µs** (steady to ±0.03 %) | 504.8 baud |
  | Indoor unit | **2055.6 µs** (looser, ±0.5 %) | 486.5 baud |

  A receiver must tolerate that 3.8 % difference. The unit rejects a *remote* message sent only 20 % faster (speed test, section 7).
- **Wiring facts (measured):** with the remote fully disconnected (all three wires) the line idles at 12 V. A remote that is attached but
  unpowered clamps the line low (about 2.6 V). With the remote connected a multimeter reads 8.2-9.3 V on the line, which is probably the average of a
  pulsing line, not a logic level.
- Message bytes are **11 cells**: start (line low), eight data cells least significant first, then two more cells. The "ninth cell" is **not**
  parity (it depends only on the byte's position in the message), so a standard 8E1 UART decodes only part of the stream correctly; see section 5.

## 2. Participants and roles

- The **wired remote is the bus master**. It polls the unit about every 247 ms. The **unit only replies** (and echoes the settings it accepts).
- Nothing else has been seen on the bus: no room temperature, no fault code, no second device. The second byte's high nibble identifies the
  sender: `2` remote, `1` unit.
- **The remote is the only stateful controller.** The unit's reply carries no settings: it is constant (`7F 18 <run> 48 80`) apart from the run flag, so there is nothing from which a remote could learn the unit's mode or setpoint. The remote shows the settings it holds itself (the last ones it sent). Nuance:
  the remote does copy the unit's run flag into its polls (1,592 of 1,600), so it reads that one bit back, but nothing else. The unit echoes the settings it accepts, which the remote may use as an acknowledgement, not as state. Consequences: a Home Assistant change never shows on the
  remote's display, the remote re-sends its own stored settings when it re-links or is power-cycled (see the master controller doc), and the truth about the unit's settings lives only in the last setting message sent by whoever was master.
- The unit does not switch off the moment polling stops: with the compressor **not running** it shut down **1 minute after the last poll** (timed by the
  operator). No off message is sent; it looks like a communication-loss timeout. After the compressor has been running for a while the delay may be
  different (for example a longer protection or run-down sequence), and has not been measured. So a polling gap shorter than a minute (an ESP32 reboot, a
  WiFi drop) should not turn the unit off, but an OTA update plus reboot can come close. A master must still poll continuously.

## 3. Messages

All values are bus bytes (line inverted). Start byte is always `7F`. Only the message shapes below have been seen.

| Message | Bytes | Cells | Duration | Sender |
|---|---|---|---|---|
| Poll | `7F 28 <run> <pend>` | 44 | 87 ms | remote |
| Status reply | `7F 18 <run> 48 80` | | | unit |
| Setting | `7F 22 <round> <v> 02 5F 7E` | 84 | 166 ms | remote |
| Setting echo | `7F 12 <round> <v> 02 5F 7E` | 84 | | unit |
| Power off | `7F 24 03` | 33 | 65 ms | remote |
| Power-off echo | `7F 14 03` | 33 | | unit |
| Start-up one-shot | `7F 12 01` | | | unit |

- `<run>`: `13` idle, `17` running. In a poll it is **the remote's own on/off state**; it normally matches the unit's last reply (1,592 of 1,600 captured polls) because the remote commanded that state, but a power-cycled remote polls `13` while the unit keeps replying `17` (3 Nov: 358 polls
  in a row, `esphome-web-e4b13c-logs-9.txt`), and the unit ignores it. In a reply it is the unit's own state; it goes `13`->`17` about 1-1.5 s after a setting turns the unit on and
  `17`->`13` right after an off message. It follows on/off commands, **not** a thermostat or the compressor. Only the unit's reply tells whether the unit is running.
- `<pend>`: `08` normal, `0A` ("pending") in the polls sent right after each setting round.
- `<round>`: `03` for the first transmission of a change, `13` for the repeat.
- `<v>`: the byte view of the setting; it is **not** a stable value (section 5). Use the cell layout below.
- The unit's reply to the *first* poll after power-up is `7F 12 01` instead of `7F 18 ...`. Its meaning is unknown.

### 3.1 Setting message: cell layout

84 cells counted from the message's first falling edge (cell width = the sender's bit time), bus bits, fields **least significant bit first**.
Comparing 149 captured setting packets (50 remote, 99 unit), exactly these cells ever vary:

| Cells | Field | Values |
|---|---|---|
| 16-17 | sender (second byte high nibble) | remote `0 1`, unit `1 0` |
| 27 | round | 0 = first (`03`), 1 = repeat (`13`) |
| 33-35 | mode | 1 COOL, 2 HEAT, 4 FAN, 5 AUTO |
| 37-39 | fan | 1 high, 2 medium, 4 low, 5 auto |
| 40-44 | setpoint, **plain binary degrees C** (5 bits, 16-31) | see below |
| 54 | ECO | 0 off, 1 on |

Every cell outside the fields above is constant in the captured settings; the first 33 cells include the sender and round fields and are not all constant.
No checksum has been found. The unit's echo is cell-for-cell identical apart from
cells 16-17, so the unit repeats what it was sent. The reference encoder is `tools/fuji_packet.py` (`--golden` writes 677 vectors that
`tests/aot30_cells_test.cpp` checks the firmware encoder against). The constant base pattern is in `components/fujitsu_aot30/Aot30Cells.h`.

**Temperature and ranges.** The setpoint is the Celsius value in binary (20 °C = `10100`), one degree steps. In FAN mode the field reads 16 and carries no
setpoint. Ranges the remote allows: **HEAT 16-30, COOL 18-30, AUTO 18-30**; COOL and AUTO stay at 30 when pressed past it. There is no DRY mode, no
quiet fan speed and no vane control on this ducted unit.

### 3.2 Poll and off messages

Poll (44 cells) is constant except cell 25 (run flag) and cell 35 (pending flag): 1,607 captured polls agree. Power off (33 cells) is constant in
every capture. The unit echoes off `7F 14 03` about 78 ms later; it is not repeated.

## 4. Exchange patterns and timing

**Normal polling (unit idle or running):** poll every ~247 ms (median 246.6 ms, 238-249 ms, steady over minutes). The reply starts 1.75-6.1 ms
(median 3.9 ms) after the poll ends. Busy time per cycle is about 208 ms, leaving **30-58 ms free**.

**A change** (from the real remote, `heat.sr`, `fan-mode.sr`, `eco.sr`):

| Offset | Message |
|---|---|
| 0 | setting, round `03`. In 22 of 24 captured changes this is 397 ms after the last normal poll (247 ms in the other two). |
| ~190 ms after its start | unit echoes the setting |
| +405 ms | poll `7F 28 17 0A` |
| ~+90 ms later | unit echoes the setting again |
| +803 ms | setting, round `13` (the repeat) |
| +1210 ms | poll `0A`, then the unit echoes again |
| +1516 ms | normal poll `08`, normal 247 ms polling resumes |

The repeat round was missing from the unit's echo 3 times in 50 on the real remote (reason unknown). Power off is a single message, no repeat.

**A setting cannot be fitted into the free gap** next to a live remote: it needs 166 ms against 30-58 ms, and the unit refused every setting sent
faster (section 7). There must therefore be only one active master: the remote or a replacement ESP.
The indoor unit also transmits its replies; "one master" does not mean only one physical transmitter.

**Start-up.** With the real remote, from power-up: about 3.3 s of silence, then the first poll, then `7F 12 01` from the unit. With the ESP32 as
master the unit stayed silent for 9.3 s until our first poll, answered it with `7F 12 01`, skipped the reply to the second poll, and then replied
normally. There is no handshake, enrolment or second-remote step; the unit waits to be polled.

**Reply anomalies** (rare, only while running): one reply `7F 18 07 48 80`, one stalled reply (`7F 18 17`, 1 s of silence, then a lone `04`),
and occasional missing replies (3 of 52 polls in one ESP-master capture). A master must tolerate a missing reply.

## 5. The byte view and why it aliases

A UART at 500 baud 8E1 on the inverted stream decodes `7F`, the second byte and the poll/reply bytes correctly, but for a setting message the
"value byte" is 8 cells starting at the next falling edge. Where that falls depends on the mode bits, so the byte mixes mode, fan and setpoint bits
differently per mode, and the setpoint's top bits arrive as parity/framing information. Consequences:

- The aliasing is a property of the UART view, **not of the unit**: the unit reads the cells, and the setpoint's top bits (cells 43-44) are on the wire in
  every message. Our encoder sends all 84 cells, and the master reads each one back from the bus. So the unit always gets an unambiguous setpoint;
  only a UART-based *monitor* cannot tell the aliased values apart.
- In HEAT, setpoints 8 apart give the same byte (16 and 24, 17 and 25, ... 22 and 30; for example 18 and 26). COOL and AUTO sweeps are unambiguous within the
  mode, but some bytes are shared across modes. Example seen: HEAT 20 fan auto reads `96`.
- Reading the remote's state through a UART is therefore ambiguous. Unambiguous decoding needs the cells (GPIO edge timestamps).
- The firmware's passive monitor (`Aot30Passive.h`) only maps byte values labelled in the captures and ignores the rest. In master mode it is not used
  for setting fields: the climate state follows the sent target, rolls back if no echo is seen, and uses the unit's own reply for its run flag.

## 6. Replacing the remote (what a master must do)

1. The bus must be silent and idle high before starting; with the real remote still attached (even unpowered) it is not.
2. Poll every 247 ms with the run flag following the unit's reply (the first poll has the run flag off).
3. To change state send the setting twice with the offsets in section 4 (and `0A` polls between), or the off message once.
4. Keep polling for as long as the unit should run.
5. Read every transmitted cell back from the bus; abort on repeated mismatches. Never transmit while the unit's reply is arriving.

`Aot30Scheduler.h` implements this script; `Aot30Bus.cpp` the bit-banged transmit and the safety checks.

## 7. Experiments that shaped this

- **Speed test (ladder):** HEAT 20-26 sent at 100, 80, 60, 50, 40, 30 and 25 % of the remote's cell time. Only 100 % was accepted. The rejected
  messages did no harm (the unit kept answering polls and replied with its old setting to the next `0A` poll).
- **Room temperature:** the polls and replies never vary except for the flags: 5,205 polls and replies in 21.5 min of unit-off logging, and
  several minutes of running. The unit most likely uses its own return-air sensor (or an optional remote sensor wired to the indoor board, not the
  bus). A slowly changing or rare temperature message cannot be ruled out.
- **Cold start under ESP control, and shutdown on loss of polling:** `full-run.sr`.

## Evidence

| Level | What |
|---|---|
| Real remote, cell level (logic analyser) | message shapes, cell layout, timing, start-up, HEAT 18-23, COOL 23, AUTO 18, FAN, fans, ECO, off |
| Real remote, byte level (ESP logs) through the model | HEAT 16-30, COOL 18-30, AUTO 18-30 (all 43 distinct byte pairs reproduced) |
| ESP as master on the real unit | Initial cold start with HEAT 20 / auto fan / ECO off; later 3 Nov test reports ESP-commanded HEAT / high fan remaining active after remote hand-back |
| Remote return on the real unit (3 Nov) | Memory-enabled remote resumes polling without pushing stored settings; a button sends stored FAN / low; off is sent and echoed |
| Host tests, not hardware evidence | 677 cell-model vectors, scheduler and bus controller with simulated time, GPIO and UART |
| Still lacking a documented ESP-driven hardware sweep | All setpoints/modes/fans, ECO on, and an off message sent by the ESP rather than the physical remote |

Raw captures and the old capture-analysis utilities were removed during repository cleanup.
The counts and named recordings above are retained historical observations, not files that can
be replayed from this checkout. The retained golden vectors check model/encoder agreement;
they are not independent proof of hardware compatibility.

## Questions already answered

These conclusions reconcile the early investigation notes with the later 3 Nov test results
and the current implementation. They apply to the reference installation, not all Fujitsu models.

| Earlier question | Answer / evidence |
|---|---|
| Is the setting temperature an arbitrary lookup or missing high bits? | It is a five-bit Celsius value at cells 40-44. The ambiguity is introduced by UART sampling, not by the transmitted command. |
| Can a faster setting fit beside the remote's polls? | Not in the tested speed ladder; only the original cell time was accepted. |
| Can LIN-only isolation preserve an HA change after reconnect? | No in the reported 3 Nov test: the still-powered remote immediately re-sent its settings. |
| Does restoring remote power overwrite an ESP change? | Not in the memory-enabled remote-return test: the unit stayed in ESP-commanded HEAT / high while the remote held FAN / low. The stored FAN / low was sent only on a button press. |
| Does the remote poll while showing `EE:EE`? | Yes: 358 idle polls were recorded after reconnect while the unit continued replying running. The continued polling explains why it ran beyond the no-poll timeout. |
| Does an idle poll mean the unit is off? | No. Remote polls `7F 28 13 08` coexisted with running replies `7F 18 17 48 80`. Only the unit's reply supplies its run flag. |
| Is `EE:EE` caused by reconnect order? | The operator identified it as the remote's power-failure indication, independent of reconnect order. It did not prevent polling; a button cleared it. |
| Must the remote return to COOL 23 after every takeover? | No. That was the earlier no-memory test. With memory enabled, the first button sends the remote's stored settings instead. |
| Must power and LIN be switched independently? | No for the tested hand-back: simultaneous switching with a double-pole relay is sufficient. Both legs must be isolated because an unpowered remote clamps LIN low. |
| Does stopping the ESP master immediately stop the unit? | No. It stops polling, releases the relay if configured, and preserves the shown state. The unit can continue under remote polling; without polls, about one minute to shutdown was measured with the compressor off. |
| Has relay takeover been implemented? | Yes: optional `remote_relay_pin`, merge/user-idle waits, bus checks, command/echo wait and relay release exist and are host-tested. Automated relay operation still needs hardware validation. |
| Does a command that failed to send stay displayed as successful? | The current code publishes after the final command transmission and rolls back if no setting/off echo is seen. This is a host-tested fix, not proof of every hardware command combination. |

### Acknowledgement is limited evidence

The bus controller counts recognised unit setting/off echoes during a command's acknowledgement
window. It does **not** compare every echoed cell with the target. `Master Last Command`
therefore reports that an echo was observed, not an independently decoded readback of mode,
temperature, fan and ECO. Local transmit read-back checks the driven line; that too is distinct
from verifying the indoor unit's resulting operation.

## Remaining unknowns and validation work

| Item | What is still unknown or unverified |
|---|---|
| Other models | AOT20 and other related systems need their own protocol/compatibility evidence. |
| Hardware command coverage | Complete the ESP-driven mode/setpoint/fan/ECO/off sweep; do not treat golden vectors or echo presence as that sweep. |
| Start-up message | The trigger for `7F 12 01` is observed, but its exact meaning is not known. |
| Missing / anomalous replies | Why repeat echoes are sometimes absent, and the causes of the rare malformed replies, remain unknown. |
| Constant cells | No checksum was found; whether apparently constant cells encode other features is not established. |
| Temperature limits | The remote's limits are known (HEAT 16-30, COOL/AUTO 18-30). Whether the unit accepts values outside them is not established and is not supported by the encoder. |
| Room temperature | None identified in the observed traffic. Rare messages and the exact indoor sensor/regulation behaviour remain unverified. |
| Other features | Defrost/de-icing and test-run messages have not been identified. |
| Poll loss / restart | Idle-flag polls while running are proven harmless in the remote-return test. Actual ESP reboot/OTA recovery and shutdown timing with a running compressor still need tests. The firmware's 65 s silence-to-off display rule is an inference, not another measured timeout. |
| Relay takeover | Manual isolation/hand-back was tested; automatic relay timing, electrical behaviour/current draw and repeated power-cycle durability still need validation. |
| Exact passive state | A cell-level receiver is still unimplemented; the UART lookup cannot unambiguously recover all settings. |
| Two-transceiver bridge | Still a design proposal. Synthetic-reply acceptance, remote-side start-up, response deadlines and state/display synchronisation are untested. |

For current operation and the implemented relay sequence, see the
[controller guide](aot30-master-controller.md). A separate bridge would require a unit-side
master, a remote-side reply emulator, a cell-level receiver and hardware-timed transmission
to service both buses. Power-cycle takeover does not implement that bridge.
