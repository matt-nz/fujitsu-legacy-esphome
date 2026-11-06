# Legacy wired-remote protocol: field reference

Quick reference for the protocol captured on ART30RL / AOT30L with wired remote
9364911014. Other models remain unverified. This is reverse engineered, not a
vendor specification. See [analysis and evidence](aot30-protocol-analysis.md)
for timing, UART aliasing, experiments and remaining unknowns, and the
[controller guide](aot30-master-controller.md) for operation.

## Conventions

- Hex values below are **inverted bus bytes** (received UART byte XOR `0xFF`).
- A **cell** is one transmitted bit time. Cell numbers are zero-based from
  the first falling edge of a message. Bus bit `1` means line low.
- Setting fields are packed **least significant bit first** into an 84-cell
  message. Their hex values are field values, **not standalone UART bytes**.
  In particular, do not patch a UART "value byte" to change temperature.
- Physical bus: nominal 12 V, idle high, approximately 500 baud. Remote cell
  time is 1980.8 microseconds; unit replies use about 2055.6 microseconds.

## MODE

Three-bit field at **cells 33-35**:

| Mode | Hex field value |
|---|---|
| Cool | `0x01` |
| Heat | `0x02` |
| Fan only | `0x04` |
| Auto | `0x05` |

Off is a separate command, not another MODE value. No Dry mode is implemented.

## TEMP

Five-bit field at **cells 40-44**: the temperature in whole degrees Celsius,
with **no offset**.

| Temperature | Hex field value |
|---|---|
| 16 C | `0x10` |
| 18 C | `0x12` |
| 20 C | `0x14` |
| 23 C | `0x17` |
| 24 C | `0x18` |
| 30 C | `0x1E` |

Supported ranges: **Heat 16-30 C; Cool / Auto 18-30 C**, in one-degree steps.
In Fan-only mode, send `0x10` (16); it does not represent an active setpoint.
The five-bit width is not evidence that every value from 0 to 31 is supported.

## FAN speed

Three-bit field at **cells 37-39**:

| Fan speed | Hex field value |
|---|---|
| High | `0x01` |
| Medium | `0x02` |
| Low | `0x04` |
| Auto | `0x05` |

Quiet is not implemented. These are wire-field values, not the internal
numeric identifiers used by the passive parser / climate adapter.

## ECO / Economy

Single bit at **cell 54**: `0x00` = off, `0x01` = on.

## Setting round and sender

| Field | Cells | Value |
|---|---|---|
| Repeat round | 27 | `0` first transmission, `1` repeat; UART round byte appears as `03` / `13` |
| Sender | 16-17 | Remote: cells `0 1`; unit echo: cells `1 0` (listed in transmission order) |

The remaining setting cells come from the fixed base pattern in
[`Aot30Cells.h`](../components/fujitsu_aot30/Aot30Cells.h).
[`fuji_packet.py`](../tools/fuji_packet.py) is the corresponding reference model.
For example, `python3 tools/fuji_packet.py heat 20 auto` builds a setting with
MODE `0x02`, TEMP `0x14`, FAN `0x05`, ECO `0x00`.

## Power, polls and replies

| Function | Inverted UART byte view | Meaning |
|---|---|---|
| Poll | `7F 28 <run> <pending>` | Remote/master asks the unit for status |
| Status reply | `7F 18 <run> 48 80` | Unit reports its run flag |
| Setting | `7F 22 <round> <value> 02 5F 7E` | Starts/changes operation; encode the cells, not this aliased byte view |
| Setting echo | `7F 12 <round> <value> 02 5F 7E` | Unit echoes a setting |
| Off | `7F 24 03` | Remote/master commands off |
| Off echo | `7F 14 03` | Unit echoes off |
| Start-up reply | `7F 12 01` | Observed after the first power-up poll; exact meaning unknown |

- `<run>`: `0x13` idle/off, `0x17` running. **Only the unit reply reports
  the unit's state.** A remote can poll idle while the unit continues running.
  This flag does not report compressor activity or thermostat demand, and an
  idle reply does not change the selected climate mode to Off.
- `<pending>`: `0x08` normal, `0x0A` after each setting round.
- `<round>`: `0x03` first, `0x13` repeat.
- `<value>`: UART sampling mixes mode/fan/temperature differently by mode;
  it is **not a reliable packed register**.

There is no separate power-on message: send the desired setting to start.
Off is sent once; a setting is sent in two rounds with intervening pending polls.
Normal polls are about 247 ms apart and must continue while controlling the unit.
The full change sequence takes about 1.5 seconds; use the
[measured schedule](aot30-protocol-analysis.md#4-exchange-patterns-and-timing),
not back-to-back UART writes.

## Feedback limits

Normal replies do not contain mode, setpoint, fan, ECO or an identified room
temperature. The unit echoes settings, but the firmware's acknowledgement
currently detects echo presence rather than comparing all returned fields.
There is no implemented secondary-controller mode: only one master may poll
the unit at a time.
