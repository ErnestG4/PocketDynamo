// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/engines/sampler_test.cpp -o t && ./t
#include "cloud.h"
#include "sampler.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace pd;

namespace {

void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }

constexpr size_t kBlock = 32;

// A take: 100 ms of silence, then four decaying 220 Hz hits 250 ms apart,
// then silence. At the store's 24 kHz.
std::vector<float> fourHits() {
  const uint32_t r = SampleStore::kRate;
  std::vector<float> x(r * 3 / 2, 0.0f);
  for (int h = 0; h < 4; ++h) {
    const uint32_t at = r / 10 + h * r / 4;
    for (uint32_t i = 0; i < r / 5 && at + i < x.size(); ++i)
      x[at + i] = 0.3f * std::exp(-(float)i / (0.03f * r)) * std::sin(6.2831853f * 220.0f * i / r);
  }
  return x;
}

struct Run { float peak = 0.0f; float energy = 0.0f; size_t lastSound = 0; };

Run render(Engine& e, EngineParams p, size_t samples) {
  Run r;
  float out[kBlock], aux[kBlock];
  for (size_t t = 0; t < samples; t += kBlock) {
    e.render(p, out, aux, kBlock);
    p.trigger = 0;
    for (size_t i = 0; i < kBlock; ++i) {
      check(std::isfinite(out[i]) && std::fabs(out[i]) <= 1.0f && std::fabs(aux[i]) <= 1.0f, "bounded and finite");
      r.peak = std::max(r.peak, std::fabs(out[i]));
      r.energy += out[i] * out[i];
      if (std::fabs(out[i]) > 1e-3f) r.lastSound = t + i;
    }
  }
  return r;
}

}  // namespace

int main() {
  SampleStore& s = sampleStore();

  // 12-bit packing: neighbours sharing a byte stay independent, values round
  // to within half a 12-bit step, the extremes clamp.
  {
    s.startRecording();
    const int vals[] = {0, 1, -1, 7, 8, -8, -9, 1000, -1000, 32767, -32768, 12345, -23456, 15, -16, 300};
    for (uint32_t pass = 0; pass < 2; ++pass)   // write in both orders: no clobbering either way
      for (uint32_t k = 0; k < 16; ++k) {
        const uint32_t i = pass ? 15 - k : k;
        s.put(i, vals[i]);
      }
    for (uint32_t i = 0; i < 16; ++i) {
      const int want = std::max(-32768, std::min(32752, vals[i]));
      check(std::abs(s.at(i) - want) <= 8 && (s.at(i) & 0xF) == 0, "12-bit round trip");
    }
    s.put(4, 20000);
    check(std::abs(s.at(4) - 20000) <= 8 && std::abs(s.at(5) - vals[5]) <= 8 && std::abs(s.at(3) - vals[3]) <= 8,
          "rewriting one sample leaves its neighbours");
    s.stopRecording();
    s.clear();
  }

  // Recording decimates 2:1 and stops at capacity.
  {
    s.startRecording();
    float in[kBlock];
    for (size_t i = 0; i < kBlock; ++i) in[i] = (i & 1) ? 0.5f : 0.25f;
    s.record(in, kBlock);
    check(s.length() == kBlock / 2, "2:1 decimation");
    check(std::abs(s.at(0) - (int)(0.375f * 32767)) <= 8, "pairs are averaged (to 12 bits)");
    for (int b = 0; b < 20000; ++b) s.record(in, kBlock);
    check(s.full() && s.length() == SampleStore::kCapacity, "stops at capacity");
    s.stopRecording();
    s.clear();
  }

  // Silence is refused; a take with hits is trimmed, normalized and sliced.
  {
    std::vector<float> quiet(SampleStore::kRate, 0.0f);
    s.loadForTest(quiet.data(), (uint32_t)quiet.size());
    check(!s.analyze() && s.state() == SampleStore::Empty, "a silent take is refused");
    auto x = fourHits();
    s.loadForTest(x.data(), (uint32_t)x.size());
    check(s.analyze() && s.ready(), "a take with hits is kept");
    check(s.length() < x.size() - SampleStore::kRate / 20, "leading/trailing silence trimmed");
    check(s.gain() > 2.5f && s.gain() < 3.5f, "normalized to ~0.9 peak");
    check(s.slices() == 4, "one slice per hit");
    for (int i = 1; i < s.slices(); ++i) {
      const float gap = (float)(s.sliceStart(i) - s.sliceStart(i - 1)) / SampleStore::kRate;
      check(std::fabs(gap - 0.25f) < 0.01f, "slices land on the hits");
    }
    printf("store ok (len %.2fs, %d slices, gain %.2f)\n", (float)s.length() / SampleStore::kRate, s.slices(), s.gain());
  }

  // SAMPLE: one-shot ends; an octave up ends twice as fast; loop keeps going;
  // reverse ends; every control extreme stays bounded.
  {
    SamplerEngine e;
    e.reset();
    EngineParams p;
    p.harmonics = 0.0f; p.timbre = 1.0f; p.morph = 0.0f; p.macro = 0.5f;
    p.note = 60.0f; p.trigger = kTriggerRising;
    const Run c4 = render(e, p, 2 * 48000);
    p.note = 72.0f;
    const Run c5 = render(e, p, 2 * 48000);
    // Output samples at C4: the whole take, though its last 20 ms (the trim's
    // pad below -50 dB) can round to silence at 12 bits.
    const float expect = 2.0f * s.length(), padOut = 2.0f * SampleStore::kRate / 50;
    check(c4.lastSound > 0.98f * (expect - padOut) && c4.lastSound < 1.02f * expect,
          "C4 plays the take at its own speed");
    check(std::fabs(2.0f * c5.lastSound - c4.lastSound) < 0.02f * c4.lastSound, "an octave up is twice as fast");
    check(c4.peak > 0.3f && c4.peak < 0.7f, "level sits with the synth engines");
    p.note = 60.0f; p.morph = 0.3f; p.timbre = 0.6f;
    const Run loop = render(e, p, 3 * 48000);
    check(loop.lastSound > 3 * 48000 - 4800, "loop keeps sounding");
    p.morph = 0.6f;
    check(render(e, p, 3 * 48000).lastSound > 3 * 48000 - 4800, "ping-pong keeps sounding");
    p.morph = 0.9f; p.timbre = 1.0f;
    const Run rev = render(e, p, 2 * 48000);
    check(rev.peak > 0.3f && rev.lastSound < c4.lastSound + 4800, "reverse plays and ends");
    p.morph = 0.0f; p.harmonics = 0.6f;   // slice 2 of 4
    const Run slice = render(e, p, 2 * 48000);
    check(slice.lastSound < c4.lastSound * 0.6f, "a later slice starts later in the take");
    for (float h : {0.0f, 1.0f}) for (float t : {0.0f, 1.0f}) for (float mo : {0.0f, 0.3f, 0.6f, 0.9f})
      for (float x : {0.0f, 0.5f, 1.0f}) for (float note : {0.0f, 60.0f, 127.0f}) {
        p.harmonics = h; p.timbre = t; p.morph = mo; p.macro = x; p.note = note; p.trigger = kTriggerRising;
        render(e, p, 4800);
      }
    // The region moving under a running voice (no retrigger) never reads
    // outside the take: sweep every control per block, all modes.
    float o[kBlock], a[kBlock];
    for (float note : {24.0f, 60.0f, 108.0f}) {
      p.note = note; p.trigger = kTriggerRising;
      for (int b = 0; b < 6000; ++b) {
        p.harmonics = 0.5f + 0.5f * std::sin(b * 0.013f);
        p.timbre = 0.5f + 0.5f * std::sin(b * 0.007f);
        p.morph = 0.5f + 0.5f * std::sin(b * 0.0031f);
        p.macro = 0.5f + 0.5f * std::sin(b * 0.002f);
        e.render(p, o, a, kBlock);
        p.trigger = (b % 997 == 0) ? kTriggerRising : 0;
        for (float v : o) check(std::isfinite(v) && std::fabs(v) <= 1.0f, "sweep bounded");
      }
    }
    printf("SAMPLE ok (C4 peak %.2f)\n", c4.peak);
  }

  // CLOUD: sounds, density raises the grain count without swinging the
  // level, every zone and extreme stays bounded.
  {
    CloudEngine e;
    e.reset();
    EngineParams p;
    p.harmonics = 0.3f; p.timbre = 0.5f; p.morph = 0.5f; p.macro = 0.5f; p.note = 60.0f;
    p.trigger = kTriggerRising;
    const Run mid = render(e, p, 2 * 48000);
    check(mid.peak > 0.1f, "a cloud sounds");
    p.macro = 1.0f; p.trigger = kTriggerRising;
    const Run dense = render(e, p, 2 * 48000);
    check(dense.energy < 6.0f * mid.energy && dense.energy > mid.energy / 6.0f, "density doesn't swing the level");
    for (float h : {0.0f, 1.0f}) for (float t : {0.0f, 1.0f}) for (float mo : {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f})
      for (float x : {0.0f, 1.0f}) for (float note : {0.0f, 60.0f, 127.0f}) {
        p.harmonics = h; p.timbre = t; p.morph = mo; p.macro = x; p.note = note; p.trigger = kTriggerRising;
        render(e, p, 9600);
      }
    float o[kBlock], a[kBlock];
    for (float note : {0.0f, 60.0f, 127.0f}) {
      p.note = note; p.trigger = kTriggerRising;
      for (int b = 0; b < 6000; ++b) {
        p.harmonics = 0.5f + 0.5f * std::sin(b * 0.013f);
        p.timbre = 0.5f + 0.5f * std::sin(b * 0.007f);
        p.morph = 0.5f + 0.5f * std::sin(b * 0.0031f);
        p.macro = 0.5f + 0.5f * std::sin(b * 0.002f);
        e.render(p, o, a, kBlock);
        p.trigger = 0;
        for (float v : o) check(std::isfinite(v) && std::fabs(v) <= 1.0f, "cloud sweep bounded");
      }
    }
    printf("CLOUD ok (peak %.2f, dense peak %.2f)\n", mid.peak, dense.peak);
  }

  // Nothing recorded: both are silent.
  {
    s.clear();
    SamplerEngine a; CloudEngine b;
    a.reset(); b.reset();
    EngineParams p; p.trigger = kTriggerRising;
    check(render(a, p, 4800).peak == 0.0f && render(b, p, 4800).peak == 0.0f, "silent with no take");
  }
  printf("sampler tests ok\n");
  return 0;
}
