// Checks Aot30Cells.h against the Python model's output (tests/golden_cells.txt).
// Build and run from the repository root:
//   python3 tools/fuji_packet.py --golden tests/golden_cells.txt
//   g++ -std=c++17 -Wall -Wextra -Icomponents/fujitsu_aot30 tests/aot30_cells_test.cpp -o /tmp/aot30_cells_test
//   /tmp/aot30_cells_test tests/golden_cells.txt

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "Aot30Cells.h"

using namespace fuji_aot30;

static std::string to_string(const uint8_t *cells, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) s += cells[i] ? '1' : '0';
  return s;
}

int main(int argc, char **argv) {
  std::ifstream in(argc > 1 ? argv[1] : "tests/golden_cells.txt");
  if (!in) {
    std::fprintf(stderr, "cannot open golden file\n");
    return 2;
  }
  int checked = 0, failed = 0;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream is(line);
    std::string kind;
    is >> kind;
    std::string expected;
    uint8_t cells[kSettingCells];
    std::string got;
    if (kind == "setting") {
      std::string mode, fan, temp;
      int eco, repeat;
      is >> mode >> fan >> temp >> eco >> repeat >> expected;
      Mode m = mode == "cool" ? Mode::COOL : mode == "heat" ? Mode::HEAT : mode == "fan" ? Mode::FAN : Mode::AUTO;
      Fan f = fan == "high" ? Fan::FAN_HIGH : fan == "medium" ? Fan::FAN_MEDIUM : fan == "low" ? Fan::FAN_LOW : Fan::FAN_AUTO;
      uint8_t t = temp == "-" ? 0 : static_cast<uint8_t>(std::stoi(temp));
      if (!build_setting(m, f, t, eco != 0, repeat != 0, cells)) {
        std::fprintf(stderr, "build_setting rejected: %s\n", line.c_str());
        failed++;
        continue;
      }
      got = to_string(cells, kSettingCells);
    } else if (kind == "poll") {
      int running, pending;
      is >> running >> pending >> expected;
      build_poll(running != 0, pending != 0, cells);
      got = to_string(cells, kPollCells);
    } else if (kind == "off") {
      is >> expected;
      build_off(cells);
      got = to_string(cells, kOffCells);
    } else {
      continue;
    }
    checked++;
    if (got != expected) {
      std::fprintf(stderr, "MISMATCH: %s\n  got      %s\n", line.c_str(), got.c_str());
      failed++;
    }
  }

  // Out-of-range values are refused.
  uint8_t c[kSettingCells];
  if (build_setting(Mode::COOL, Fan::FAN_AUTO, 17, false, false, c)) { std::fprintf(stderr, "COOL 17 accepted\n"); failed++; }
  if (build_setting(Mode::HEAT, Fan::FAN_AUTO, 31, false, false, c)) { std::fprintf(stderr, "HEAT 31 accepted\n"); failed++; }
  if (build_setting(Mode::HEAT, Fan::FAN_AUTO, 15, false, false, c)) { std::fprintf(stderr, "HEAT 15 accepted\n"); failed++; }
  if (checked < 600) { std::fprintf(stderr, "only %d vectors checked\n", checked); failed++; }

  std::printf("%d vectors checked, %d failures\n", checked, failed);
  return failed ? 1 : 0;
}
