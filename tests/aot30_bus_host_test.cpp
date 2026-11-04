// Exercises the master-controller code in Aot30Bus.cpp on a desktop compiler with fake time, GPIO and UART.
//   g++ -std=c++17 -w -DESP32 -Itests/stubs -Icomponents/fujitsu_aot30 tests/aot30_bus_host_test.cpp \
//       components/fujitsu_aot30/Aot30Bus.cpp -o /tmp/aot30_bus_host_test && /tmp/aot30_bus_host_test
// Nothing here touches hardware. The fake GPIO loops the driven level back to the RX pin, like the transceiver.

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>

#include "Aot30Bus.h"

using namespace fuji_aot30;

// ---- fakes ---------------------------------------------------------------------------------------
static int64_t g_now_us = 0;
static std::vector<std::string> g_log;
static int g_tx_level = 1;
static bool g_loopback = true;
static bool g_rx_forced_low = false;
struct Edge {
  int64_t t;
  int level;
};
static std::vector<Edge> g_edges;
static int g_uart_set_pin_calls = 0;
static int g_matrix_calls = 0;
static int g_gpio_sets = 0;

unsigned long millis() { return static_cast<unsigned long>(g_now_us / 1000); }
void delay(unsigned long ms) { g_now_us += static_cast<int64_t>(ms) * 1000; }
int64_t esp_timer_get_time() { return g_now_us += 3; }  // each read advances 3 us
void fake_log(char level, const char *fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g_log.push_back(std::string(1, level) + " " + buf);
}
int gpio_set_direction(int, gpio_mode_t) { return 0; }
int gpio_set_level(int, uint32_t level) {
  g_gpio_sets++;
  g_tx_level = static_cast<int>(level);
  g_edges.push_back({g_now_us, g_tx_level});
  return 0;
}
int gpio_get_level(int) { return g_rx_forced_low ? 0 : (g_loopback ? g_tx_level : 1); }
int uart_set_pin(int, int, int, int, int) {
  g_uart_set_pin_calls++;
  return 0;
}
void esp_rom_gpio_connect_out_signal(uint32_t, uint32_t, bool, bool) { g_matrix_calls++; }
// Relay pin recording (Aot30Bus drives it with pinMode/digitalWrite).
static int g_relay_level = -1;
static int g_relay_writes = 0;
void pinMode(int, int) {}
void digitalWrite(int, int level) {
  g_relay_level = level;
  g_relay_writes++;
}

// ---- helpers -------------------------------------------------------------------------------------
static int failures = 0;
#define CHECK(c)                                          \
  do {                                                    \
    if (!(c)) {                                           \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);    \
      failures++;                                         \
    }                                                     \
  } while (0)

static bool log_has(const char *needle) {
  for (const auto &l : g_log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}
static size_t log_count(const char *needle) {
  size_t n = 0;
  for (const auto &l : g_log)
    if (l.find(needle) != std::string::npos) n++;
  return n;
}

static void advance(Bus &hp, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    g_now_us += 1000;
    hp.pump();
  }
}

static void reset(HardwareSerial &serial, Bus &hp) {
  g_log.clear();
  g_edges.clear();
  g_loopback = true;
  g_rx_forced_low = false;
  g_tx_level = 1;
  g_uart_set_pin_calls = g_matrix_calls = g_gpio_sets = 0;
  serial.rx.clear();
  hp.connect(&serial, 16, 17);
}

int main() {
  using MS = MasterState;
  static HardwareSerial serial;
  static Bus hp;
  g_now_us = 10 * 1000 * 1000;
  const uint8_t kRemotePoll[] = {0x80, 0xD7, 0xEC, 0xF7, 0x80};  // 7F 28 13 08 plus the next start byte

  // 1. Dry run: starts after 1.5 s of silence, polls, then runs and waits for a command.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(!hp.master_active() && hp.master_state() == MS::OFF);
  CHECK(!hp.submit_target(Target()));  // not running yet
  CHECK(hp.start_master());
  CHECK(!hp.start_master());  // already running
  CHECK(hp.master_active() && hp.master_state() == MS::CHECKING_BUS);
  advance(hp, 1000);
  CHECK(!log_has("polling started"));
  advance(hp, 700);
  CHECK(log_has("polling started"));
  CHECK(hp.master_state() == MS::CHECKING_BUS);
  advance(hp, 2000);
  CHECK(log_has("running, polling continuously"));
  CHECK(hp.master_state() == MS::RUNNING);
  CHECK(hp.observed().valid && !hp.observed().on);
  CHECK(g_gpio_sets == 0 && g_matrix_calls == 0);  // dry run touches no pins
  CHECK(log_count("DRY RUN would send poll") >= 8);

  // 1b. A submitted target becomes the two-round setting (first round 397 ms after the last poll), then the off message.
  // Polls continue and nothing is resent without a new target.
  g_log.clear();
  Target heat;
  heat.on = true;
  heat.mode = Mode::HEAT;
  heat.temp = 20;
  heat.eco = true;
  CHECK(hp.submit_target(heat));
  advance(hp, 100);
  CHECK(!hp.observed().on);  // not shown until the setting has actually been sent
  for (int i = 0; i < 3000 && !log_has("would send setting (round 2)"); i++) advance(hp, 1);
  CHECK(log_has("would send setting (round 2)"));
  Observed st = hp.observed();
  CHECK(st.on && st.mode == fuji_passive::kModeHeat && st.temp == 20 && st.temp_valid);
  CHECK(st.eco_valid && st.eco && st.fan_valid && st.fan == fuji_passive::kFanAuto);
  advance(hp, 100);
  for (uint8_t b : {0x80, 0xED, 0xEC, 0xA9, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // the unit echoes (7F 12 13 56 02 5F 7E)
  advance(hp, 3000);
  CHECK(hp.last_ack() == 1 && hp.observed().on);  // confirmed, so it stays
  CHECK(log_count("DRY RUN would send setting (round 1)") == 1);
  CHECK(log_count("DRY RUN would send setting (round 2)") == 1);
  CHECK(log_count("DRY RUN would send poll (pending)") == 2);
  g_log.clear();
  advance(hp, 5000);
  CHECK(log_count("would send setting") == 0 && log_count("would send poll") >= 15);
  CHECK(hp.submit_target(heat));  // same state again: nothing is sent
  advance(hp, 1000);
  CHECK(log_count("would send setting") == 0);
  Target off = heat;
  off.on = false;
  CHECK(hp.submit_target(off));
  for (int i = 0; i < 3000 && log_count("DRY RUN would send power-off") == 0; i++) advance(hp, 1);
  CHECK(log_count("DRY RUN would send power-off") == 1);
  advance(hp, 100);
  for (uint8_t b : {0x80, 0xEB, 0xFC, 0x80}) serial.rx.push_back(b);  // the unit echoes the off (7F 14 03)
  advance(hp, 1500);
  CHECK(!hp.observed().on);
  CHECK(hp.submit_target(off));  // already off: nothing sent
  advance(hp, 1500);
  CHECK(log_count("would send power-off") == 1);
  hp.stop_master();
  advance(hp, 50);
  CHECK(!hp.master_active() && hp.master_state() == MS::OFF);
  CHECK(log_has("Off. The bus is released"));
  CHECK(!hp.submit_target(heat));

  // 2. Refuses to start when a wired remote was heard just before or during the silence check.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.start_master());
  for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
  advance(hp, 300);
  CHECK(log_has("wired remote messages seen"));
  CHECK(!hp.master_active() && hp.master_state() == MS::REFUSED_REMOTE_PRESENT);
  advance(hp, 2000);  // the bus has now been quiet for longer than the check
  CHECK(hp.start_master());
  advance(hp, 2000);
  CHECK(log_has("polling started"));
  hp.stop_master();
  advance(hp, 10);
  CHECK(!hp.master_active());

  // 2b. Stray bytes from the unit itself (no remote) do not block the start.
  reset(serial, hp);
  hp.set_dry_run(true);
  serial.rx.push_back(0xFF);
  serial.rx.push_back(0xFF);
  CHECK(hp.start_master());
  advance(hp, 100);
  serial.rx.push_back(0xFF);
  advance(hp, 1700);
  CHECK(log_has("polling started"));
  CHECK(!log_has("refused"));
  hp.stop_master();
  advance(hp, 10);

  // 3. Live: pulse train equals the encoded message, pin handed back to the UART after each message.
  reset(serial, hp);
  hp.set_dry_run(false);
  CHECK(hp.start_master());
  advance(hp, 1600);
  CHECK(log_has("polling started"));
  g_edges.clear();
  g_uart_set_pin_calls = 0;
  advance(hp, 300);  // the first poll goes out within one poll period
  CHECK(g_uart_set_pin_calls >= 1);
  CHECK(!g_edges.empty());
  uint8_t cells[kSettingCells];
  build_poll(false, false, cells);
  std::vector<Edge> expected;
  int last = 1;
  for (size_t i = 0; i < kPollCells; i++) {
    int lvl = cells[i] ? 0 : 1;
    if (lvl != last) {
      expected.push_back({static_cast<int64_t>(i * kRemoteCellTenthsUs / 10), lvl});
      last = lvl;
    }
  }
  // g_edges starts with the initial idle-high write, then one entry per line change, then the release.
  bool timing_ok = g_edges.size() >= expected.size() + 1;
  if (timing_ok) {
    int64_t base = g_edges[1].t;
    for (size_t i = 0; i < expected.size() && timing_ok; i++) {
      const Edge &e = g_edges[1 + i];
      if (e.level != expected[i].level || std::llabs((e.t - base) - (expected[i].t - expected[0].t)) > 10) {
        timing_ok = false;
        std::printf("edge %zu: level %d at %lld, expected level %d at %lld\n", i, e.level,
                    static_cast<long long>(e.t - base), expected[i].level,
                    static_cast<long long>(expected[i].t - expected[0].t));
      }
    }
  }
  CHECK(timing_ok);

  // 3b. Live, continuous: our own transmissions come back on RX; they must neither count as a wired remote nor
  // reach the state decoder, over a whole change and power-off.
  size_t uart_calls = g_uart_set_pin_calls;
  g_log.clear();
  advance(hp, 2000);
  int echo_set_at = -1, echo_off_at = -1;
  for (int i = 0; i < 16000; i++) {
    advance(hp, 1);
    if (static_cast<size_t>(g_uart_set_pin_calls) != uart_calls) {
      uart_calls = g_uart_set_pin_calls;
      for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
    }
    if (i == 1000) CHECK(hp.submit_target(heat));
    if (i == 9000) CHECK(hp.submit_target(off));
    // The unit echoes what it accepts (so the commanded state stays).
    if (echo_set_at < 0 && log_count("sent setting (round 2)") == 1) echo_set_at = i + 100;
    if (i == echo_set_at) {
      for (uint8_t b : {0x80, 0xED, 0xEC, 0xA9, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);
    }
    if (echo_off_at < 0 && log_count("sent power-off") == 1) echo_off_at = i + 100;
    if (i == echo_off_at) {
      for (uint8_t b : {0x80, 0xEB, 0xFC, 0x80}) serial.rx.push_back(b);
    }
  }
  CHECK(hp.master_active() && hp.master_state() == MS::RUNNING);
  CHECK(!log_has("giving it back"));
  CHECK(log_count("sent setting (round 1)") == 1);
  CHECK(log_count("sent setting (round 2)") == 1);
  CHECK(log_count("sent power-off") == 1);
  CHECK(!hp.observed().on);
  hp.stop_master();
  advance(hp, 50);
  CHECK(!hp.master_active());
  CHECK(!g_edges.empty() && g_edges.back().level == 1 && g_tx_level == 1);  // line released high

  // 4. Read-back mismatch (no loopback, or another device pulling the line) stops it and releases the line.
  reset(serial, hp);
  hp.set_dry_run(false);
  CHECK(hp.start_master());
  advance(hp, 1600);
  g_loopback = false;
  g_log.clear();
  advance(hp, 600);
  CHECK(log_has("aborted"));
  CHECK(!hp.master_active() && hp.master_state() == MS::STOPPED_TX_FAILED);
  CHECK(g_tx_level == 1);

  // 4b. A bus held low (no idle-high, as in test.sr) is refused before anything is sent.
  reset(serial, hp);
  hp.set_dry_run(false);
  g_rx_forced_low = true;
  CHECK(hp.start_master());
  advance(hp, 1700);
  CHECK(log_has("held low with no traffic"));
  CHECK(!hp.master_active() && hp.master_state() == MS::REFUSED_BUS_LOW);
  CHECK(g_gpio_sets == 0);

  // 5. A wired remote showing up while running makes the master controller give the bus back.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.start_master());
  advance(hp, 4000);
  CHECK(hp.master_active());
  for (int n = 0; n < 3; n++) {
    for (uint8_t b : kRemotePoll) serial.rx.push_back(b);
    advance(hp, 250);
  }
  CHECK(log_has("giving it back"));
  CHECK(!hp.master_active() && hp.master_state() == MS::STOPPED_REMOTE_PRESENT);

  // 6. A stop before the bus task handled the start leaves it off, and the unit run flag follows the unit's replies.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.unit_run_state() == -1);
  CHECK(hp.start_master());
  hp.stop_master();
  advance(hp, 20);
  CHECK(!hp.master_active() && hp.master_state() == MS::OFF);
  CHECK(std::string(Bus::master_state_text(MS::REFUSED_BUS_LOW)).find("held low") != std::string::npos);
  CHECK(hp.start_master());
  advance(hp, 1600);
  for (uint8_t b : {0x80, 0xE7, 0xEC, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // unit reply 7F 18 13 48 80, idle
  advance(hp, 100);
  CHECK(hp.unit_run_state() == 0);
  for (uint8_t b : {0x80, 0xE7, 0xE8, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // same with the run flag 17
  advance(hp, 100);
  CHECK(hp.unit_run_state() == 1);
  hp.stop_master();
  advance(hp, 10);

  // 6b. The unit's echo of a setting is checked: echoed -> acknowledged, no echo -> not acknowledged.
  reset(serial, hp);
  hp.set_dry_run(true);
  CHECK(hp.start_master());
  advance(hp, 4000);
  CHECK(hp.last_ack() == 0);
  Target t1;
  t1.on = true;
  t1.mode = Mode::COOL;
  t1.temp = 24;
  CHECK(hp.submit_target(t1));
  for (int i = 0; i < 3000 && !log_has("would send setting (round 2)"); i++) advance(hp, 1);
  CHECK(log_has("would send setting (round 2)"));
  advance(hp, 100);
  for (uint8_t b : {0x80, 0xED, 0xEC, 0xA9, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // 7F 12 13 56 02 5F 7E
  advance(hp, 600);
  CHECK(hp.last_ack() == 1 && log_has("acknowledged the setting"));
  Target t2 = t1;
  t2.temp = 25;
  g_log.clear();
  CHECK(hp.submit_target(t2));
  advance(hp, 2700);  // sent, and no echo arrives
  CHECK(hp.last_ack() == 2 && log_has("did NOT acknowledge the setting"));
  hp.stop_master();
  advance(hp, 10);

  // 7. Monitor mode (master controller off, real remote connected): messages are parsed into a best-effort state.
  static Bus hp2;
  reset(serial, hp2);
  CHECK(hp2.last_message_ms() == 0 && !hp2.observed().valid);
  for (uint8_t b : {0x80, 0xDD, 0xFC, 0x43, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // 7F 22 03 BC 02 5F 7E: COOL low 23
  advance(hp2, 50);
  CHECK(hp2.last_message_ms() != 0);
  Observed mon = hp2.observed();
  CHECK(mon.valid && mon.on && mon.mode == fuji_passive::kModeCool && mon.fan == fuji_passive::kFanLow &&
        mon.temp_valid && mon.temp == 23);
  uint32_t rev = hp2.observed_revision();
  for (uint8_t b : {0xDB, 0xFC, 0x80}) serial.rx.push_back(b);  // 7F 24 03: power off
  advance(hp2, 50);
  CHECK(hp2.observed().valid && !hp2.observed().on && hp2.observed_revision() != rev);
  CHECK(Observed::unpack(mon.pack()) == mon);

  // 7b. The remote's poll flag does not turn the state off, only the unit's own reply does (a power-cycled remote polls
  // "idle" while the unit keeps running).
  static Bus hp3;
  reset(serial, hp3);
  for (uint8_t b : {0x80, 0xDD, 0xFC, 0x43, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // remote: COOL low 23
  advance(hp3, 50);
  CHECK(hp3.observed().valid && hp3.observed().on);
  for (uint8_t b : {0x80, 0xE7, 0xE8, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // unit reply 7F 18 17 48 80 (running)
  advance(hp3, 50);
  CHECK(hp3.unit_run_state() == 1);
  for (uint8_t b : {0xD7, 0xEC, 0xF7, 0x80}) serial.rx.push_back(b);  // remote poll 7F 28 13 08 (idle)
  advance(hp3, 50);
  CHECK(hp3.observed().on);  // still on
  for (uint8_t b : {0xE7, 0xE8, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // unit still running
  advance(hp3, 50);
  for (uint8_t b : {0xE7, 0xEC, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);  // unit reply 7F 18 13 48 80 (stopped)
  advance(hp3, 50);
  CHECK(hp3.unit_run_state() == 0 && !hp3.observed().on);  // the unit's own reply turns it off

  // 8. Takeover with a remote relay: listen, cut the remote off in a gap, send the change, check the echo, hand it back.
  {
    static Bus tk;
    auto setup_bus = [&](Bus &b, bool inverted) {
      g_log.clear();
      serial.rx.clear();
      g_relay_level = -1;
      b.set_remote_relay(26, inverted);
      b.connect(&serial, 16, 17);
      b.set_dry_run(true);
    };
    // Traffic like a live bus: the remote polls every 247 ms (unless cut off) and the unit replies 90 ms later.
    auto run = [&](Bus &b, int ms, bool remote_polls) {
      for (int i = 0; i < ms; i++) {
        advance(b, 1);
        if (millis() % 247 == 0 && remote_polls && !b.remote_cut()) {
          for (uint8_t x : {0x80, 0xD7, 0xE8, 0xF7}) serial.rx.push_back(x);  // 7F 28 17 08
        }
        if (millis() % 247 == 90) {
          for (uint8_t x : {0x80, 0xE7, 0xE8, 0xB7, 0x7F}) serial.rx.push_back(x);  // 7F 18 17 48 80
        }
      }
    };
    setup_bus(tk, false);
    CHECK(tk.takeover_available() && g_relay_level == 0);  // remote connected
    run(tk, 3000, true);
    CHECK(!tk.master_active() && !tk.remote_cut() && tk.unit_run_state() == 1);

    Target want;
    want.on = true;
    want.mode = Mode::HEAT;
    want.temp = 22;
    CHECK(tk.request_takeover(want));
    CHECK(tk.master_state() == MasterState::TAKEOVER_PENDING);
    run(tk, 1500, true);
    CHECK(g_relay_level == 0);  // still inside the merge window
    int waited = 0;
    while (!tk.remote_cut() && waited++ < 4000) run(tk, 1, true);
    CHECK(tk.remote_cut() && g_relay_level == 1 && tk.master_active());  // cut
    CHECK(!log_has("would send poll"));                                  // nothing was sent before the cut
    waited = 0;
    while (!log_has("would send setting (round 2)") && waited++ < 8000) run(tk, 1, true);
    CHECK(log_has("would send setting (round 2)") && tk.master_state() == MasterState::TAKEOVER);
    run(tk, 100, true);
    for (uint8_t x : {0x80, 0xED, 0xEC, 0xA9, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(x);  // the unit's echo (7F 12 13 56 02 5F 7E)
    waited = 0;
    while (tk.master_active() && waited++ < 4000) run(tk, 1, true);
    CHECK(!tk.master_active() && !tk.remote_cut() && g_relay_level == 0);  // handed back
    CHECK(tk.last_ack() == 1);
    CHECK(tk.master_state() == MasterState::OFF);
    Observed after = tk.observed();
    CHECK(after.valid && after.on && after.mode == fuji_passive::kModeHeat && after.temp == 22);  // commanded state kept

    // Commands arriving together are merged into one takeover.
    g_log.clear();
    Target a = want;
    a.temp = 23;
    CHECK(tk.request_takeover(a));
    run(tk, 500, true);
    a.temp = 24;
    CHECK(tk.request_takeover(a));
    waited = 0;
    while (!tk.remote_cut() && waited++ < 6000) run(tk, 1, true);
    waited = 0;
        while (tk.master_active() && waited++ < 20000) run(tk, 1, true);  // no echo this time
    CHECK(log_count("would send setting (round 1)") == 1 && tk.observed().temp == 22);  // not echoed, so the shown state went back to 22
    CHECK(tk.last_ack() == 2 && !tk.remote_cut());  // not acknowledged, still handed back

    // Someone using the remote delays the takeover (the remote's setting arrives well after our own last transmission).
    run(tk, 200, true);
    for (uint8_t x : {0x80, 0xDD, 0xFC, 0x43, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(x);  // 7F 22 03 BC 02 5F 7E
    run(tk, 300, true);
    CHECK(tk.request_takeover(a));
    run(tk, 10000, true);
    CHECK(!tk.remote_cut());  // still waiting: the remote setting was less than 15 s ago
    waited = 0;
    while (!tk.remote_cut() && waited++ < 10000) run(tk, 1, true);
    CHECK(tk.remote_cut());
    waited = 0;
    while (tk.master_active() && waited++ < 20000) run(tk, 1, true);

    // The relay did not work: the remote keeps talking after the cut. Refuse and hand back.
    CHECK(tk.request_takeover(a));
    waited = 0;
    while (!tk.remote_cut() && waited++ < 20000) run(tk, 1, true);
    CHECK(tk.remote_cut());
    for (int i = 0; i < 700 && tk.master_active(); i++) {
      advance(tk, 1);
      if (i % 40 == 0) {
        for (uint8_t x : {0x80, 0xD7, 0xE8, 0xF7, 0x80}) serial.rx.push_back(x);  // the remote still polling
      }
    }
    CHECK(!tk.master_active() && tk.master_state() == MasterState::REFUSED_REMOTE_PRESENT && !tk.remote_cut());
    CHECK(log_has("did not cut the remote off"));
    CHECK(tk.observed().valid && tk.observed().on);  // the unit carries on under the remote, so no forced "off"

    // Continuous master with a relay: the remote is cut at the start and restored at the stop.
    setup_bus(tk, false);
    CHECK(tk.start_master());
    advance(tk, 5);
    CHECK(tk.remote_cut() && g_relay_level == 1);
    run(tk, 4000, false);
    CHECK(tk.master_state() == MasterState::RUNNING);
    tk.stop_master();
    advance(tk, 10);
    CHECK(!tk.remote_cut() && g_relay_level == 0);

    // Inverted polarity: inactive is high.
    static Bus inv;
    setup_bus(inv, true);
    CHECK(g_relay_level == 1);
    CHECK(inv.start_master());
    advance(inv, 5);
    CHECK(g_relay_level == 0 && inv.remote_cut());
    inv.stop_master();
    advance(inv, 10);
    CHECK(g_relay_level == 1);

    // Without a relay there is no takeover.
    static Bus plain;
    g_log.clear();
    serial.rx.clear();
    plain.connect(&serial, 16, 17);
    plain.set_dry_run(true);
    CHECK(!plain.takeover_available() && !plain.request_takeover(want));
  }

  // 7c. Without a relay, stopping the master does not force the state off: if the remote takes over polling, the unit carries
  // on and the state stays; only a bus silent for longer than the unit's no-poll timeout shows it off.
  static Bus hp4;
  reset(serial, hp4);
  hp4.set_dry_run(true);
  CHECK(hp4.start_master());
  advance(hp4, 4000);
  Target on_target;
  on_target.on = true;
  on_target.mode = Mode::HEAT;
  on_target.temp = 22;
  CHECK(hp4.submit_target(on_target));
  for (int i = 0; i < 4000 && !log_has("would send setting (round 2)"); i++) advance(hp4, 1);
  advance(hp4, 100);
  for (uint8_t b : {0x80, 0xED, 0xEC, 0xA9, 0xFD, 0xA0, 0x81, 0x80}) serial.rx.push_back(b);  // the unit echoes the setting
  advance(hp4, 800);
  CHECK(hp4.observed().on);
  hp4.stop_master();
  advance(hp4, 50);
  CHECK(!hp4.master_active() && hp4.observed().on);  // not forced off
  for (int i = 0; i < 70000; i++) {                   // the remote now polls and the unit keeps replying running
    advance(hp4, 1);
    if (millis() % 247 == 90) {
      for (uint8_t b : {0x80, 0xE7, 0xE8, 0xB7, 0x7F, 0x80}) serial.rx.push_back(b);
    }
  }
  CHECK(hp4.observed().on && hp4.unit_run_state() == 1);
  advance(hp4, 66000);  // nobody polls any more
  CHECK(!hp4.observed().on);  // the unit has shut down

  // 9. Regression (logs-10): a command folded into a never-set target must give a valid setting. The unset target word used to
  // unpack to fan 0, so "Master: invalid setting, stopping" ended the master on every command.
  static Bus hp5;
  reset(serial, hp5);
  hp5.set_dry_run(true);
  {
    Target fresh = hp5.last_target();
    CHECK(fresh.mode == Mode::HEAT && fresh.fan == Fan::FAN_AUTO && fresh.temp == 20 && !fresh.on);
    CHECK(Target::unpack(0).fan == Fan::FAN_AUTO && Target::unpack(0).temp == 20);
  }
  CHECK(hp5.start_master());
  advance(hp5, 4000);
  CHECK(hp5.master_state() == MS::RUNNING);
  Target partial = hp5.last_target();  // what a climate call with only a mode and temperature starts from
  partial.on = true;
  partial.mode = Mode::HEAT;
  partial.temp = 16;
  CHECK(hp5.submit_target(partial));
  for (int i = 0; i < 4000 && !log_has("would send setting (round 2)"); i++) advance(hp5, 1);
  CHECK(log_has("would send setting (round 1)") && log_has("would send setting (round 2)"));
  CHECK(!log_has("invalid setting") && hp5.master_active());
  hp5.stop_master();
  advance(hp5, 10);

  std::printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
