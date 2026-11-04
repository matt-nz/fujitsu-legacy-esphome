#!/bin/sh
# Runs every host-side test. From the repository root:  sh tests/run_all.sh
set -e
OUT=${TMPDIR:-/tmp}/fuji_tests_$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
INC="-Icomponents/fujitsu_aot30"

g++ -std=c++17 -Wall -Wextra $INC tests/aot30_cells_test.cpp -o "$OUT/cells"
"$OUT/cells" tests/golden_cells.txt
g++ -std=c++17 -Wall -Wextra $INC tests/aot30_scheduler_test.cpp -o "$OUT/sched"
"$OUT/sched"
g++ -std=c++17 -w -DESP32 -Itests/stubs $INC tests/aot30_bus_host_test.cpp components/fujitsu_aot30/Aot30Bus.cpp -o "$OUT/emu"
"$OUT/emu"
find . -name __pycache__ -not -path "./.git/*" -prune -exec rm -rf {} + 2>/dev/null || true
echo "ALL HOST TESTS PASSED"
