# AOT30L / ART30RL bus protocol

The wired-remote bus of the older ducted Fujitsu units: indoor ART30RL, outdoor AOT30L, wired remote part 9364911014, three wires (LIN, power,
ground). This is the single reference for that bus. It is reverse engineered from logic-analyser and ESP32 captures, not from a vendor
specification, and every statement carries its evidence level (see [Evidence](#evidence)). The firmware that uses this is described in
[aot30-master-controller.md](aot30-master-controller.md).

This bus is **not** the separate 8-byte frame protocol used by newer units.

## 1. Physical layer

- Single-wire LIN-style bus, 12 V, **idle high (recessive)**. The ESP32 connects through a LIN transceiver (TJA1021); RX and TX are plain 3.3 V UART
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

Every other cell is constant, including the first 33 and last 29; no checksum has been found. The unit's echo is cell-for-cell identical apart from
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
faster (section 7). The bus therefore has exactly one transmitter at a time: the remote, or a replacement master.

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
  for state: the climate state is what was commanded.

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
| ESP as master on the real unit | polling, cold start, HEAT 20 with auto fan and ECO off accepted and echoed, run flag, shutdown when polling stops |
| Not yet tried on the unit | any other setpoint, COOL, AUTO, FAN, other fan speeds, ECO on, the off message from the master |

## Open items

- Verify the encoder on the unit for the combinations above. The firmware now reports `Master Last Command` (acknowledged or not, from the unit's
  echo), so each HA command checks itself.
- Meaning of `7F 12 01`, why the repeat round is sometimes absent, and the cause of the reply anomalies.
- Whether any cell in the constant stretches carries information (no checksum found), and whether the lower limits of COOL and AUTO below 18 exist.
- A cell-level monitor (GPIO edge timestamps) to read a real remote's exact state without aliasing; only needed for monitor-only use with the
  remote connected.
- Remote features that were never captured: defrost / de-icing (the unit may report it some other way, or only the outdoor unit does it), and the
  test run mode listed in the remote's manual. Neither seems important for control.
- What the unit does if a master restarts while it is running: our first poll carries the run flag "idle" (as after power-up) although the unit is
  running. Untested; the 1 minute shutdown timeout (measured with the compressor off) should make it harmless for short restarts.
- A bridge (ESP between unit and remote, second LIN transceiver) to keep the physical remote: not built, design sketch in aot30-master-controller.md.
