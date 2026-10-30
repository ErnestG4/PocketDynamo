// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/synth/looper_test.cpp -o t && ./t
#include "looper.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace pd;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }

struct Log { std::vector<std::pair<uint32_t, Event>> got; uint32_t now = 0; };
}  // namespace

int main() {
  constexpr uint32_t kBeat = 500000;   // 120 bpm
  Looper l;
  Log log;
  auto emit = [&](const Event& e) { log.got.push_back({log.now, e}); };
  auto at = [&](uint32_t t) { log.now = t; l.service(t, emit); };

  // Record: note at 0.1 s, off at 0.3 s, a control move at 0.5 s; close at
  // 0.95 s -> snaps to 2 beats (1 s).
  const uint32_t t0 = 4000000000u;   // near the uint32 wrap on purpose
  l.press(t0, kBeat, nullptr, 0, emit);
  check(l.state() == Looper::Recording, "first press records");
  l.capture(t0 + 100000, {EventType::NoteOn, 5, 100, 60.0f});
  l.capture(t0 + 300000, {EventType::NoteOff, 5, 0, 0.0f});
  l.capture(t0 + 500000, {EventType::Param, 1, 0, 0.25f});
  l.press(t0 + 950000, kBeat, nullptr, 0, emit);
  check(l.state() == Looper::Playing && l.lapUs() == 2 * kBeat, "lap snaps to whole beats");

  for (uint32_t t = 950000; t < 3200000; t += 1000) at(t0 + t);
  // Laps from t0 + 1 s: events at 1.1, 1.3, 1.5, 2.1, 2.3, 2.5, 3.1 s.
  check(log.got.size() == 7, "each event once per lap");
  check(log.got[0].first - t0 >= 1100000 && log.got[0].first - t0 < 1102000, "on time");
  check(log.got[0].second.type == EventType::NoteOn && log.got[0].second.a == 5 + Looper::kLoopTagOffset,
        "replayed notes get loop tags");
  check(log.got[2].second.type == EventType::Param && log.got[2].second.a == 1, "params replay untouched");
  check(log.got[3].first - t0 >= 2100000 && log.got[3].first - t0 < 2102000, "second lap on time");

  // Overdub at 3.4 s (0.4 into the lap): heard from the next lap, not twice.
  l.press(t0 + 3200000, kBeat, nullptr, 0, emit);
  check(l.state() == Looper::Overdub, "third press overdubs");
  log.got.clear();
  for (uint32_t t = 3200000; t < 3400000; t += 1000) at(t0 + t);
  l.capture(t0 + 3400000, {EventType::NoteOn, 9, 100, 67.0f});
  l.capture(t0 + 3450000, {EventType::NoteOff, 9, 0, 0.0f});
  for (uint32_t t = 3400000; t < 4000000; t += 1000) at(t0 + t);
  size_t sameLap = 0;
  for (auto& g : log.got) sameLap += g.second.a == 9 + Looper::kLoopTagOffset;
  check(sameLap == 0, "an overdub doesn't replay in the lap it was played");
  log.got.clear();
  for (uint32_t t = 4000000; t < 5000000; t += 1000) at(t0 + t);
  check(log.got.size() == 5, "next lap has the overdub too");
  check(log.got[2].second.a == 9 + Looper::kLoopTagOffset && log.got[2].first - t0 >= 4400000 &&
        log.got[2].first - t0 < 4402000, "overdub lands where it was played");

  // A replayed event coming back through capture is not re-recorded.
  const int before = l.events();
  l.capture(t0 + 5000000, {EventType::NoteOn, 9 + Looper::kLoopTagOffset, 100, 67.0f});
  check(l.events() == before, "replays aren't recorded again");

  // Stop releases every loop tag; stop again clears.
  log.got.clear();
  l.stop(emit);
  check(l.state() == Looper::Stopped && log.got.size() <= 2, "stop releases only sounding loop tags");
  for (auto& g : log.got) check(g.second.type == EventType::NoteOff && g.second.a >= Looper::kLoopTagOffset, "only NoteOffs");
  l.stop(emit);
  check(l.state() == Looper::Empty && l.events() == 0, "second stop clears");

  // A note held across the close gets a NoteOff at the lap end.
  Looper h;
  log.got.clear();
  h.press(0, kBeat, nullptr, 0, emit);
  h.capture(100000, {EventType::NoteOn, 3, 100, 60.0f});
  const uint8_t held[1] = {3};
  h.press(480000, kBeat, held, 1, emit);
  for (uint32_t t = 480000; t < 1100000; t += 1000) { log.now = t; h.service(t, emit); }
  check(log.got.size() == 2 && log.got[1].second.type == EventType::NoteOff, "held note closes at the lap end");
  // Stopped mid-note, the looper releases exactly the loop tag still sounding.
  for (uint32_t t = 1100000; t < 1200000; t += 1000) { log.now = t; h.service(t, emit); }
  log.got.clear();
  h.stop(emit);
  check(log.got.size() == 1 && log.got[0].second.type == EventType::NoteOff &&
        log.got[0].second.a == 3 + Looper::kLoopTagOffset, "stop releases the sounding loop tag");

  // Closing late skips what was just heard live (no double at the close).
  Looper late;
  log.got.clear();
  late.press(0, kBeat, nullptr, 0, emit);
  late.capture(100000, {EventType::NoteOn, 4, 100, 60.0f});
  late.capture(1020000, {EventType::NoteOn, 6, 100, 62.0f});   // past the 1 s lap: folds to 0.02 s
  late.press(1100000, kBeat, nullptr, 0, emit);                 // snaps down to 2 beats
  // The lap restarted at 1.0 s: the note at lap 0.1 s is due now (1.1 s);
  // the one folded to 0.02 s was heard live at 1.02 s and must not replay.
  for (uint32_t t = 1100000; t < 1150000; t += 1000) { log.now = t; late.service(t, emit); }
  check(log.got.size() == 1 && log.got[0].second.a == 4 + Looper::kLoopTagOffset, "no burst on a late close");
  for (uint32_t t = 1150000; t < 2200000; t += 1000) { log.now = t; late.service(t, emit); }
  check(log.got.size() == 3, "both events next lap");
  printf("looper ok\n");
  return 0;
}
