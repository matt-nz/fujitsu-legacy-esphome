#!/usr/bin/env python3
"""Encode and decode the wired remote's setting packet (AOT30L/ART30RL bus) as cells.

This is a model of observed behaviour, not a validated command format. Nothing here transmits.
Layout and field positions align with the protocol notes in docs/aot30-protocol.md.

    python3 tools/fuji_packet.py heat 22 auto            # cells and pulse list for one setting
    python3 tools/fuji_packet.py cool 24 high --eco --repeat
    python3 tools/fuji_packet.py fan --                  # FAN carries no setpoint
    python3 tools/fuji_packet.py --golden tests/golden_cells.txt   # regenerate the C++ test vectors

Cells are bus bits (the line level inverted), cell 0 being the first start cell. A cell lasts
REMOTE_CELL_US (1980.8 us) when the remote sends and UNIT_CELL_US (2055.6 us) for the unit's echo.
"""

import argparse
import sys

REMOTE_CELL_US = 1980.8
UNIT_CELL_US = 2055.6

# Remote packet with every variable field cleared; all other cells are constant in 149 captured
# packets (heat, fan-mode, eco and start-up captures).
BASE = "111111110101010001001011100000001000100000000001010000001011111101000000101111110100"
FIELDS = {"round": (27, 1), "mode": (33, 3), "fan": (37, 3), "temp": (40, 5), "eco": (54, 1)}

# Remote poll `7F 28 13|17 08|0A` (44 cells) with the two variable cells cleared: cell 25 is the run flag
# (13 idle / 17 running) and cell 35 the pending flag (08 / 0A, seen right after each setting message).
# All other cells are constant in 1,607 captured remote polls.
POLL_BASE = "11111111010100010100101110010000010001000000"
POLL_FIELDS = {"running": 25, "pending": 35}
# Remote power-off `7F 24 03` (33 cells), constant in all captured remote messages; the unit's echo
# `7F 14 03` differs only in the sender cells 16 and 17.
OFF_BASE = "111111110101001001001011100000000"
SENDER_CELLS = (16, 17)  # remote: 0 1 (second byte 0x22), unit: 1 0 (0x12)

MODES = {"cool": 1, "heat": 2, "fan": 4, "auto": 5}
FANS = {"high": 1, "medium": 2, "low": 4, "auto": 5}
FAN_MODE_TEMP = 16  # what the remote sends in the temperature field in FAN mode
# Setpoint ranges on the remote (operator-confirmed, consistent with the captures): HEAT 16-30, COOL 18-30,
# AUTO 18-30. COOL and AUTO clamp at 30 on the bus; the lower limits come from the operator because the
# captured sweeps started at 18 and only went up.
TEMP_RANGES = {"heat": (16, 30), "cool": (18, 30), "auto": (18, 30)}


def encode(mode, fan, temp=None, eco=False, repeat=False, sender="remote"):
    """Return the 84 bus-bit cells for one setting message."""
    if mode not in MODES or fan not in FANS:
        raise ValueError("mode must be one of %s and fan one of %s" % (sorted(MODES), sorted(FANS)))
    if mode == "fan":
        temp = FAN_MODE_TEMP
    low, high = TEMP_RANGES.get(mode, (16, 31))
    if temp is None or not low <= temp <= high:
        raise ValueError("%s setpoint must be %d..%d (got %r)" % (mode, low, high, temp))
    cells = [int(c) for c in BASE]
    values = {"round": int(repeat), "mode": MODES[mode], "fan": FANS[fan], "temp": temp, "eco": int(eco)}
    for name, (first, width) in FIELDS.items():
        for i in range(width):
            cells[first + i] = (values[name] >> i) & 1
    if sender == "unit":
        cells[SENDER_CELLS[0]], cells[SENDER_CELLS[1]] = 1, 0
    elif sender != "remote":
        raise ValueError("sender must be remote or unit")
    return cells


def encode_poll(running=False, pending=False):
    """Return the 44 bus-bit cells of one remote poll."""
    cells = [int(c) for c in POLL_BASE]
    cells[POLL_FIELDS["running"]] = int(bool(running))
    cells[POLL_FIELDS["pending"]] = int(bool(pending))
    return cells


def encode_off(sender="remote"):
    """Return the 33 bus-bit cells of the power-off message."""
    cells = [int(c) for c in OFF_BASE]
    if sender == "unit":
        cells[SENDER_CELLS[0]], cells[SENDER_CELLS[1]] = 1, 0
    elif sender != "remote":
        raise ValueError("sender must be remote or unit")
    return cells


def golden_lines():
    """One line per encodable message: `kind args cells`. Used by the C++ host test."""
    out = []
    for mode in sorted(MODES):
        low, high = TEMP_RANGES.get(mode, (None, None))
        for fan in sorted(FANS):
            for temp in ([None] if mode == "fan" else range(low, high + 1)):
                for eco in (0, 1):
                    for repeat in (0, 1):
                        cells = encode(mode, fan, temp, bool(eco), bool(repeat))
                        out.append("setting %s %s %s %d %d %s" % (mode, fan, "-" if temp is None else temp, eco, repeat,
                                                                    "".join(map(str, cells))))
    for running in (0, 1):
        for pending in (0, 1):
            out.append("poll %d %d %s" % (running, pending, "".join(map(str, encode_poll(running, pending)))))
    out.append("off %s" % "".join(map(str, encode_off())))
    return out


def decode(cells):
    values = {}
    for name, (first, width) in FIELDS.items():
        values[name] = sum(cells[first + i] << i for i in range(width))
    names_mode = {v: k for k, v in MODES.items()}
    names_fan = {v: k for k, v in FANS.items()}
    return {"mode": names_mode.get(values["mode"]), "fan": names_fan.get(values["fan"]),
            "temp": None if values["mode"] == 4 else values["temp"],
            "eco": bool(values["eco"]), "repeat": bool(values["round"])}


def runs(cells, cell_us=REMOTE_CELL_US):
    """Line-level pulse list [(level, duration_us)], starting from the idle (high) line."""
    out = []
    for bit in cells:
        level = 1 - bit
        if out and out[-1][0] == level:
            out[-1][1] += cell_us
        else:
            out.append([level, cell_us])
    if out and out[-1][0] == 0:
        out.append([1, cell_us])
    return [(lvl, round(us, 1)) for lvl, us in out]


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--golden":
        with open(sys.argv[2], "w") as f:
            f.write("\n".join(golden_lines()) + "\n")
        return
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=sorted(MODES))
    ap.add_argument("temp", help="setpoint 16..31, or -- for FAN mode")
    ap.add_argument("fan", nargs="?", default="auto", choices=sorted(FANS))
    ap.add_argument("--eco", action="store_true")
    ap.add_argument("--repeat", action="store_true", help="the second round (`13`) of a change")
    ap.add_argument("--sender", default="remote", choices=("remote", "unit"))
    args = ap.parse_args()
    temp = None if args.temp == "--" else int(args.temp)
    try:
        cells = encode(args.mode, args.fan, temp, args.eco, args.repeat, args.sender)
    except ValueError as err:
        sys.exit(str(err))
    print("cells (bus bits):")
    for i in range(0, len(cells), 11):
        print("%3d  %s" % (i, "".join(map(str, cells[i:i + 11]))))
    print("decodes to:", decode(cells))
    cell = REMOTE_CELL_US if args.sender == "remote" else UNIT_CELL_US
    print("line pulses (level, us), cell %.1f us:" % cell)
    print(" ".join("%d:%.0f" % (lvl, us) for lvl, us in runs(cells, cell)))


if __name__ == "__main__":
    main()
