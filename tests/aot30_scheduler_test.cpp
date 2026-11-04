// Checks Aot30Scheduler.h against the timing seen in the captures.
//   g++ -std=c++17 -Wall -Wextra -Icomponents/fujitsu_aot30 tests/aot30_scheduler_test.cpp -o /tmp/aot30_scheduler_test
//   /tmp/aot30_scheduler_test

#include <cstdio>
#include <vector>

#include "Aot30Scheduler.h"

using namespace fuji_aot30;

struct Event {
  uint32_t t;
  Action a;
};

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);            \
      failures++;                                                 \
    }                                                             \
  } while (0)

static std::vector<Event> run(Scheduler &s, uint32_t from, uint32_t to, uint32_t step = 1) {
  std::vector<Event> out;
  for (uint32_t t = from; t < to; t += step) {
    Action a;
    if (s.due(t, &a)) {
      out.push_back({t, a});
      s.sent(t);
    }
  }
  return out;
}

int main() {
  Scheduler s;
  Action a;
  CHECK(!s.due(0, &a));  // inactive: nothing due

  s.start(1000);
  auto idle = run(s, 1000, 2000);
  CHECK(idle.size() == 5);  // 1000, 1247, 1494, 1741, 1988
  for (size_t i = 0; i < idle.size(); i++) {
    CHECK(idle[i].a.kind == Action::Kind::POLL && !idle[i].a.running && !idle[i].a.pending);
    if (i) CHECK(idle[i].t - idle[i - 1].t == Scheduler::kPollPeriodMs);
  }

  // Turn on: setting one poll slot plus 150 ms after the last poll (397 ms, as the real remote does), then the captured script.
  CHECK(s.queue_setting(Mode::HEAT, Fan::FAN_AUTO, 20, false));
  CHECK(!s.queue_setting(Mode::HEAT, Fan::FAN_AUTO, 21, false));  // one change at a time
  CHECK(!s.queue_off());
  auto ev = run(s, 2000, 6000);
  CHECK(ev.size() >= 9);
  uint32_t t0 = ev[0].t;
  CHECK(t0 == 2385);  // 1988 + 247 + 150
  CHECK(ev[0].a.kind == Action::Kind::SETTING && !ev[0].a.repeat && ev[0].a.temp == 20 && ev[0].a.mode == Mode::HEAT);
  CHECK(ev[1].t == t0 + 405 && ev[1].a.kind == Action::Kind::POLL && ev[1].a.running && ev[1].a.pending);
  CHECK(ev[2].t == t0 + 803 && ev[2].a.kind == Action::Kind::SETTING && ev[2].a.repeat);
  CHECK(ev[3].t == t0 + 1210 && ev[3].a.kind == Action::Kind::POLL && ev[3].a.pending);
  CHECK(ev[4].t == t0 + 1516 && ev[4].a.kind == Action::Kind::POLL && ev[4].a.running && !ev[4].a.pending);
  CHECK(ev[5].t == ev[4].t + Scheduler::kPollPeriodMs && ev[5].a.running && !ev[5].a.pending);
  CHECK(s.running());

  // A late transmission (bus busy) shifts the rest of the script, it does not skip steps.
  Scheduler late;
  late.start(0);
  CHECK(late.queue_setting(Mode::COOL, Fan::FAN_LOW, 24, true));
  Action x;
  CHECK(!late.due(Scheduler::kSettingExtraDelayMs - 1, &x));
  CHECK(late.due(Scheduler::kSettingExtraDelayMs, &x) && x.kind == Action::Kind::SETTING);
  late.sent(200);  // went out 50 ms late
  CHECK(!late.due(200 + 404, &x));
  CHECK(late.due(200 + 405, &x) && x.kind == Action::Kind::POLL);

  // Turn off: off message, a poll still flagged running, then idle polls.
  Scheduler off;
  off.start(0);
  CHECK(off.queue_setting(Mode::AUTO, Fan::FAN_AUTO, 22, false));
  run(off, 0, 3000);
  CHECK(off.running());
  CHECK(off.queue_off());
  auto o = run(off, 3000, 5000);
  CHECK(o.size() >= 4);
  CHECK(o[0].a.kind == Action::Kind::OFF);
  CHECK(o[1].t == o[0].t + 167 && o[1].a.kind == Action::Kind::POLL && o[1].a.running);
  CHECK(o[2].t == o[0].t + 405 && o[2].a.kind == Action::Kind::POLL && !o[2].a.running);
  CHECK(o[3].a.kind == Action::Kind::POLL && !o[3].a.running);
  CHECK(!off.running());

  // Invalid setpoints are refused; FAN ignores the setpoint.
  Scheduler bad;
  bad.start(0);
  CHECK(!bad.queue_setting(Mode::COOL, Fan::FAN_AUTO, 17, false));
  CHECK(!bad.queue_setting(Mode::HEAT, static_cast<Fan>(0), 20, false));  // an invalid fan code is refused, not sent
  CHECK(!bad.queue_setting(Mode::HEAT, Fan::FAN_AUTO, 31, false));
  CHECK(bad.queue_setting(Mode::FAN, Fan::FAN_AUTO, 0, false));

  // millis() wrap-around.
  Scheduler wrap;
  wrap.start(0xFFFFFF00u);
  size_t polls = 0;
  for (uint32_t i = 0; i < 600; i++) {
    const uint32_t t = 0xFFFFFF00u + i;  // wraps past zero
    Action wa;
    if (wrap.due(t, &wa)) {
      polls++;
      wrap.sent(t);
    }
  }
  CHECK(polls == 3);

  // Idle polls follow the unit's run flag; after a queued change the commanded flag holds until the unit agrees.
  Scheduler mirror;
  mirror.start(0);
  Action m;
  CHECK(mirror.due(0, &m) && !m.running);
  mirror.sent(0);
  mirror.observe_unit_running(true);  // unit already running: the next poll says so
  CHECK(mirror.due(247, &m) && m.running);
  mirror.sent(247);
  CHECK(mirror.queue_off());
  mirror.observe_unit_running(true);  // still reports running while the off command goes out
  auto go = run(mirror, 494, 1500);
  CHECK(go.size() >= 3 && go[0].a.kind == Action::Kind::OFF);
  CHECK(!mirror.running());
  mirror.observe_unit_running(false);  // unit confirms off
  mirror.observe_unit_running(true);   // and later follows it back on (a person used a remote)
  CHECK(mirror.running());

  // After stop() nothing is due.
  s.stop();
  CHECK(!s.due(100000, &a));

  std::printf(failures ? "%d FAILURES\n" : "ALL PASSED\n", failures);
  return failures ? 1 : 0;
}
