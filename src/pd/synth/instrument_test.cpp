// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/synth/instrument_test.cpp -o t && ./t
#include "instrument.h"

#include <cassert>
#include <cstdio>

using namespace pd;

namespace {

constexpr size_t kBlock = 32;
using Synth = Instrument<6, kBlock>;

// Peak |x| over a run of blocks. Peak, not RMS: a beeper pin train is a sparse
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
  // A take for the sampler engines: 1 s of a 220 Hz tone with harmonics.
  {
    static float take[SampleStore::kRate];
    for (uint32_t i = 0; i < SampleStore::kRate; ++i) {
      const float t = 6.2831853f * 220.0f * i / SampleStore::kRate;
      take[i] = 0.4f * std::sin(t) + 0.2f * std::sin(2 * t) + 0.1f * std::sin(3 * t);
    }
    sampleStore().loadForTest(take, SampleStore::kRate);
    check(sampleStore().analyze(), "test take loads");
  }
  for (int eng = 0; eng < (int)EngineId::Count; ++eng) {
    Synth s;
    s.handle({EventType::Engine, (uint8_t)eng, 0, 0.0f});
    // Blocks to wait out a release (to -80 dB) plus margin.
    const int tail = (int)((releaseMs((EngineId)eng) * 1e-3f + 0.15f) * kSampleRate / kBlock);
    check(runPeak(s, 10) == 0.0f, "silent before any note");

    s.handle({EventType::NoteOn, 1, 100, 60.0f});
    const float on = runPeak(s, 200);
    check(on > 0.1f, "a held note sounds");
    check(s.activeVoices() == 1, "one voice for one note");

    s.handle({EventType::NoteOff, 1, 0, 0.0f});
    runPeak(s, tail);   // past the release
    check(s.activeVoices() == 0, "voice frees after release");
    check(runPeak(s, 10) == 0.0f, "silent after release");

    // Seven notes on six voices: the oldest is stolen, the pool stays full.
    for (int n = 0; n < 7; ++n) s.handle({EventType::NoteOn, (uint8_t)(10 + n), 100, 48.0f + n});
    runPeak(s, 50);
    check(s.activeVoices() == 6, "pool saturates at six");
    s.handle({EventType::AllOff, 0, 0, 0.0f});
    runPeak(s, tail);
    check(s.activeVoices() == 0, "all-off releases everything");

    // Every control at both extremes stays finite and bounded.
    for (int p = 0; p < (int)Param::Count; ++p)
      for (float v : {0.0f, 1.0f}) {
        s.handle({EventType::Param, (uint8_t)p, 0, v});
        s.handle({EventType::NoteOn, 2, 127, 72.0f});
        runPeak(s, 40);
        s.handle({EventType::NoteOff, 2, 0, 0.0f});
      }
    s.handle({EventType::AllOff, 0, 0, 0.0f});
    runPeak(s, tail);

    // A chord is several NoteOns under one tag; one NoteOff ends all of it,
    // and leaves other tags alone.
    for (float n : {60.0f, 63.5f, 67.0f}) s.handle({EventType::NoteOn, 5, 100, n});
    s.handle({EventType::NoteOn, 6, 100, 48.0f});
    runPeak(s, 20);
    check(s.activeVoices() == 4, "chord takes a voice per note");
    s.handle({EventType::NoteOff, 5, 0, 0.0f});
    runPeak(s, tail);
    check(s.activeVoices() == 1, "one NoteOff ends the whole chord, not other tags");

    // Ratchet: a retrigger with a decay falls silent while still held; a
    // retrigger with 0 brings it back to a held note.
    s.handle({EventType::Retrigger, 6, 0, 30.0f});
    runPeak(s, 3);
    check(runPeak(s, 30) > 0.05f, "a ratchet hit sounds");
    runPeak(s, (int)(0.1f * kSampleRate / kBlock));
    check(runPeak(s, 10) < 0.01f, "a ratchet hit decays while gated");
    check(s.activeVoices() == 1, "a decayed ratchet voice is still held");
    s.handle({EventType::Retrigger, 6, 0, 0.0f});
    runPeak(s, 50);
    check(runPeak(s, 100) > 0.05f, "retrigger 0 returns to a held note");
    s.handle({EventType::AllOff, 0, 0, 0.0f});

    printf("%s ok (held-note peak %.3f)\n", engineName((EngineId)eng), on);
  }

  // The NaN guards. Every engine stays finite through a NaN pitch and NaN
  // controls (runPeak asserts every sample is finite; an engine either goes
  // quiet or its voice is caught and rebuilt), and a normal note after it plays.
  for (int eng = 0; eng < (int)EngineId::Count; ++eng) {
    static TapeEcho echo;
    static MasterFx fx;
    Synth s;
    s.setEcho(&echo);
    s.setFx(&fx);
    echo.setTime(0.1f); echo.setFeedback(0.6f); echo.setMix(0.5f);
    fx.set(MasterFx::Tone, 0, 0.6f);
    s.handle({EventType::Engine, (uint8_t)eng, 0, 0.0f});
    s.handle({EventType::NoteOn, 1, 100, std::nanf("")});
    runPeak(s, 50);
    for (int k = 0; k < (int)Param::Count; ++k) s.handle({EventType::Param, (uint8_t)k, 0, std::nanf("")});
    s.handle({EventType::NoteOn, 2, 100, 60.0f});
    runPeak(s, 50);
    for (int k = 0; k < (int)Param::Count; ++k) s.handle({EventType::Param, (uint8_t)k, 0, 0.5f});
    s.handle({EventType::AllOff, 0, 0, 0.0f});
    runPeak(s, 400);
    s.handle({EventType::NoteOn, 3, 100, 64.0f});
    check(runPeak(s, 200) > 0.05f, "a note after the NaNs plays");
    printf("%s: finite through NaNs (%u caught)\n", engineName((EngineId)eng), (unsigned)s.faults());
  }
  // The bus guard: poison the filter's state, and the next block is caught,
  // silenced, and the effects start clean.
  {
    static TapeEcho echo;
    static MasterFx fx;
    Synth s;
    s.setEcho(&echo);
    s.setFx(&fx);
    fx.set(MasterFx::Tone, 0, 0.6f);
    s.handle({EventType::NoteOn, 1, 100, 60.0f});
    runPeak(s, 20);
    float bad[kBlock];
    for (auto& x : bad) x = std::nanf("");
    fx.process(bad, kBlock);   // the filter's integrators are NaN now
    const uint32_t before = s.faults();
    runPeak(s, 1);
    check(s.faults() == before + 1, "the bus guard catches it");
    check(runPeak(s, 100) > 0.05f, "and the note plays on through clean effects");
    printf("bus guard ok\n");
  }
  return 0;
}
