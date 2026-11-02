// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/synth/instrument_test.cpp -o t && ./t
#include "instrument.h"

#include <cassert>
#include <cstdio>
#include <cstring>

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
  // The ADSR, block by block: 1.5 blocks per ms at 48 kHz / 32.
  {
    Envelope e;
    e.setShape(Envelope::shape(10.0f, 100.0f, 0.5f, 200.0f, kBlock), kBlock);
    float a, b;
    e.gateOn();
    int n = 0;
    do { e.advance(&a, &b); ++n; } while (b < 1.0f && n < 1000);
    check(n == 15, "attack reaches full level in its time");
    for (int i = 0; i < 150; ++i) e.advance(&a, &b);   // the decay time: -80 dB of the way
    check(std::fabs(b - 0.5f) < 1e-3f, "decay lands on the sustain level");
    for (int i = 0; i < 100; ++i) e.advance(&a, &b);
    check(b == 0.5f, "and holds there exactly");
    e.setShape(Envelope::shape(10.0f, 100.0f, 0.8f, 200.0f, kBlock), kBlock);
    for (int i = 0; i < 300; ++i) e.advance(&a, &b);
    check(b == 0.8f, "a held note follows a live sustain change, up as well as down");
    e.retrigger(50.0f);   // a ratchet hit decays to silence...
    for (int i = 0; i < 300; ++i) e.advance(&a, &b);
    check(b < 1e-3f, "a ratchet hit decays to silence under the ADSR");
    e.retrigger(0.0f);    // ...and the ratchet's end returns to the ADSR's sustain
    for (int i = 0; i < 400; ++i) e.advance(&a, &b);
    check(b == 0.8f, "the ratchet's end returns to the sustain level");
    e.gateOff();
    n = 0;
    while (e.active() && n < 1000) { e.advance(&a, &b); ++n; }
    check(n >= 288 && n <= 296, "release: its time is from full level, so from 0.8 a little less");
    printf("adsr ok\n");
  }
  // The instrument's ENV: on, new notes take the ADSR; off again, they take
  // their engine's envelope back.
  {
    Synth s;
    s.handle({EventType::Engine, (uint8_t)EngineId::BrocotFm, 0, 0.0f});
    s.handle({EventType::Env, 3, 0, 2000.0f});   // a long release
    s.handle({EventType::Env, 4, 0, 1.0f});
    s.handle({EventType::NoteOn, 1, 100, 60.0f});
    runPeak(s, 100);
    s.handle({EventType::NoteOff, 1, 0, 0.0f});
    runPeak(s, (int)(0.6f * kSampleRate / kBlock));   // past the engine's own 350 ms
    check(s.activeVoices() == 1, "the ADSR's release outlasts the engine's");
    s.handle({EventType::Env, 4, 0, 0.0f});
    s.handle({EventType::AllOff, 0, 0, 0.0f});
    runPeak(s, (int)(3.0f * kSampleRate / kBlock));
    s.handle({EventType::NoteOn, 1, 100, 60.0f});
    runPeak(s, 100);
    s.handle({EventType::NoteOff, 1, 0, 0.0f});
    runPeak(s, (int)(0.6f * kSampleRate / kBlock));
    check(s.activeVoices() == 0, "ENV off: the engine's release again");
    // Sustain 0: a pluck that dies while held.
    s.handle({EventType::Env, 1, 0, 100.0f});
    s.handle({EventType::Env, 2, 0, 0.0f});
    s.handle({EventType::Env, 4, 0, 1.0f});
    s.handle({EventType::NoteOn, 2, 100, 60.0f});
    check(runPeak(s, 30) > 0.05f, "a pluck sounds");
    runPeak(s, 300);
    check(runPeak(s, 20) < 1e-3f, "and dies while the key is held");
    printf("env ok\n");
  }
  // The LFO on its own.
  {
    Lfo l(kBlock);
    l.setDepth(1.0f);
    l.setRate(15.0f);   // 100 blocks a cycle
    float lo = 1.0f, hi = -1.0f;
    for (int sh = 0; sh < (int)LfoShape::Count; ++sh) {
      l.setShape(sh);
      lo = 1.0f; hi = -1.0f;
      for (int i = 0; i < 1000; ++i) { const float v = l.advance(); lo = std::min(lo, v); hi = std::max(hi, v); }
      check(lo >= -1.0f && hi <= 1.0f && hi - lo > 1.0f, "every shape swings within -1..1");
    }
    l.setShape((int)LfoShape::Square);
    l.setSync(1.0f);   // a beat at 120 bpm: 0.5 s, 750 blocks
    l.setTempo(120.0f);
    int flips = 0;
    float last = l.advance();
    for (int i = 0; i < 7500; ++i) { const float v = l.advance(); flips += (v > 0) != (last > 0); last = v; }
    check(flips >= 19 && flips <= 21, "synced to a beat: ten cycles in five seconds");
    l.setDelay(100.0f);   // 150 blocks
    l.restartDelay();
    l.setSync(0.0f);
    l.setRate(15.0f);
    l.setShape((int)LfoShape::Square);
    check(std::fabs(l.advance()) < 0.02f, "the delay starts the depth at zero");
    for (int i = 0; i < 160; ++i) l.advance();
    check(std::fabs(std::fabs(l.advance()) - 1.0f) < 1e-6f, "and has faded it in after its time");
    printf("lfo ok\n");
  }
  // The LFO in the instrument: it moves what it's routed to, and routed
  // nowhere it leaves the output bit-exact to no LFO at all.
  {
    auto render = [](bool lfo, LfoDest d) {
      Synth s;
      s.handle({EventType::Engine, (uint8_t)EngineId::BrocotFm, 0, 0.0f});
      if (lfo) {
        s.handle({EventType::Lfo, 0, 0, 5.0f});
        s.handle({EventType::Lfo, 1, 0, 0.5f});
        s.handle({EventType::Lfo, 4, (uint8_t)d, 0.0f});
      }
      s.handle({EventType::NoteOn, 1, 100, 60.0f});
      uint64_t h = 1469598103934665603ull;
      float out[kBlock];
      for (int b = 0; b < 400; ++b) {
        s.render(out);
        for (float x : out) { uint32_t u; memcpy(&u, &x, 4); h = (h ^ u) * 1099511628211ull; }
      }
      return h;
    };
    const uint64_t plain = render(false, LfoDest::None);
    check(render(true, LfoDest::None) == plain, "an LFO routed nowhere changes nothing");
    check(render(true, LfoDest::Timbre) != plain, "the LFO moves TIMBRE");
    check(render(true, LfoDest::Pitch) != plain, "the LFO moves the pitch");
    printf("lfo routing ok\n");
  }
  return 0;
}
