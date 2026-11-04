# Retro-Fujitsu-ESPHome (AOT30)

ESPHome external component for older Fujitsu ducted systems that use the 3-wire LIN wired-remote bus.

This repository is now focused on the **AOT30/ART30RL master-controller implementation** only.

## What this repo contains

- Runtime component: `components/fujitsu_aot30/`
- Example configuration: `aot30.yaml`
- Host-side tests: `tests/`
- Protocol/helpers: `tools/fuji_packet.py`
- AOT30 documentation: `docs/aot30-*.md`

## What this repo does not contain

Legacy `fujitsu_climate` (newer 8-byte protocol), exploratory capture dumps, and historical session notes
have been removed from this branch to keep a clean project scope.

## Quick start

1. Keep this repository (or a copy of it) next to your ESPHome YAML.
2. Use `external_components` in `aot30.yaml` to load `fujitsu_aot30`.
3. Wire your LIN transceiver and ESP32 as documented in [docs/aot30-circuit.md](docs/aot30-circuit.md).

## Validation

Run host-side tests from the repository root:

```sh
sh tests/run_all.sh
```

Expected result:

```text
ALL HOST TESTS PASSED
```

## Core docs

- [docs/aot30-master-controller.md](docs/aot30-master-controller.md)
- [docs/aot30-protocol.md](docs/aot30-protocol.md)
- [docs/aot30-touchscreen-controller.md](docs/aot30-touchscreen-controller.md)
- [docs/aot30-circuit.md](docs/aot30-circuit.md)
