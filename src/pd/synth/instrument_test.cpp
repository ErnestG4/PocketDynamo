// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/synth/instrument_test.cpp -o t && ./t
#include "instrument.h"

#include <cassert>
#include <cstdio>

using namespace pd;

namespace {

constexpr size_t kBlock = 32;
using Synth = Instrument<6, kBlock>;

// Peak |x| over a run of blocks. Peak, not RMS: a Follin pin train is a sparse
// click train, mostly silent between pins by design.
float runPeak(Synth& s, int blocks) {
  float out[kBlock], peak = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    s.render(out);
    for (float x : out) {
      assert(std::isfinite(x) && x <= 1.0f && x >= -1.0f);
      peak = std::max(peak, std::fabs(x));
    }
  }
  return peak;
}

void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }

}  // namespace

int main() {
  for (int eng = 0; eng < (int)EngineId::Count; ++eng) {
    Synth s;
    s.handle({EventType::Engine, (uint8_t)eng, 0, 0.0f});
    check(runPeak(s, 10) == 0.0f, "silent before any note");

    s.handle({EventType::NoteOn, 60, 100, 0.0f});
    const float on = runPeak(s, 200);
    check(on > 0.1f, "a held note sounds");
    check(s.activeVoices() == 1, "one voice for one note");

    s.handle({EventType::NoteOff, 60, 0, 0.0f});
    runPeak(s, (int)(0.4f * kSampleRate / kBlock));   // past the 250 ms release
    check(s.activeVoices() == 0, "voice frees after release");
    check(runPeak(s, 10) == 0.0f, "silent after release");

    // Seven notes on six voices: the oldest is stolen, the pool stays full.
    for (int n = 0; n < 7; ++n) s.handle({EventType::NoteOn, (uint8_t)(48 + n), 100, 0.0f});
    runPeak(s, 50);
    check(s.activeVoices() == 6, "pool saturates at six");
    s.handle({EventType::AllOff, 0, 0, 0.0f});
    runPeak(s, (int)(0.4f * kSampleRate / kBlock));
    check(s.activeVoices() == 0, "all-off releases everything");

    // Every control at both extremes stays finite and bounded.
    for (int p = 0; p < (int)Param::Count; ++p)
      for (float v : {0.0f, 1.0f}) {
        s.handle({EventType::Param, (uint8_t)p, 0, v});
        s.handle({EventType::NoteOn, 72, 127, 0.0f});
        runPeak(s, 40);
        s.handle({EventType::NoteOff, 72, 0, 0.0f});
      }
    printf("%s ok (held-note peak %.3f)\n", engineName((EngineId)eng), on);
  }
  return 0;
}
