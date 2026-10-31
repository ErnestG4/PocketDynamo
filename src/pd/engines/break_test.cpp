// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/engines/break_test.cpp -o t && ./t
#include "break.h"

#include <cassert>
#include <cstdio>

using namespace pd;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }
constexpr size_t kBlock = 32;
}  // namespace

int main() {
  // Grammar: verbatim plays every step in place; full chop rearranges; the
  // cells always tile the bar; the same seed cuts the same way.
  {
    chop::Pattern a, b;
    a.Generate(16, 4, 1.0f, 1, 2);
    int covered = 0;
    for (int i = 0; i < 16;) {
      check(a.src[i] == i, "verbatim plays in place");
      covered += a.len[i]; i += a.len[i];
    }
    check(covered == 16, "verbatim tiles");
    for (uint64_t seed = 1; seed < 200; ++seed) {
      for (float v : {0.0f, 0.2f, 0.5f, 0.8f}) {
        a.Generate(16, 4, v, seed, seed * 7);
        int i = 0, cells = 0;
        while (i < 16) {
          check(a.len[i] >= 1 && i + a.len[i] <= 16, "cells tile the bar");
          check(a.src[i] == chop::Pattern::kRest || (a.src[i] >= 0 && a.src[i] + (a.rev[i] ? 0 : 0) < 16), "sources in range");
          for (int j = i + 1; j < i + a.len[i]; ++j) check(a.src[j] == chop::Pattern::kInterior, "interiors marked");
          i += a.len[i]; ++cells;
        }
        check(i == 16 && cells >= 1, "exact tiling");
      }
    }
    a.Generate(16, 4, 0.1f, 42, 43); b.Generate(16, 4, 0.1f, 42, 43);
    bool same = true;
    for (int i = 0; i < 16; ++i) same &= a.src[i] == b.src[i] && a.len[i] == b.len[i] && a.rev[i] == b.rev[i];
    check(same, "seeded: same seed, same chops");
    int moved = 0;
    a.Generate(16, 4, 0.3f, 5, 6);
    for (int i = 0; i < 16; ++i) moved += a.src[i] >= 0 && a.src[i] != i;
    check(moved > 0, "chopping moves cells");
    printf("chop grammar ok\n");
  }

  // Engine: a one-bar take of 16 distinct tones; verbatim at the take's own
  // tempo reproduces it; every control extreme stays bounded.
  {
    const uint32_t r = SampleStore::kRate;
    const uint32_t bar = r * 2;   // 2 s = one bar at 120 bpm
    static float x[SampleStore::kCapacity];
    for (uint32_t i = 0; i < bar; ++i) {
      const int step = i * 16 / bar;
      x[i] = 0.5f * std::sin(6.2831853f * (200.0f + 50.0f * step) * i / r);
    }
    sampleStore().loadForTest(x, bar);
    check(sampleStore().analyze(), "take loads");
    BreakEngine e;
    e.reset();
    EngineParams p;
    p.harmonics = 0.0f; p.timbre = 0.0f; p.morph = 1.0f; p.macro = 0.5f; p.note = 60.0f; p.tempo = 120.0f;
    p.trigger = kTriggerRising;
    float out[kBlock], aux[kBlock], peak = 0.0f;
    // Verbatim at the take's own tempo is the take, at the output rate:
    // correlate against it (the store trims a little lead-in, so align on
    // the trimmed take itself).
    const SampleStore& st = sampleStore();
    double sxy = 0, sxx = 0, syy = 0;
    for (int b = 0; b < 3000; ++b) {   // 2 s
      e.render(p, out, aux, kBlock);
      p.trigger = 0;
      for (size_t i = 0; i < kBlock; ++i) {
        const float v = out[i];
        check(std::isfinite(v) && std::fabs(v) <= 1.0f, "bounded");
        peak = std::max(peak, std::fabs(v));
        const size_t t = b * kBlock + i;
        if (t < 2 * (size_t)st.length() - 4 && t > 480) {
          const float ref = st.read((uint32_t)(t / 2), (t & 1) ? 0.5f : 0.0f);
          sxy += v * ref; sxx += v * v; syy += ref * ref;
        }
      }
    }
    const double corr = sxy / std::sqrt(sxx * syy + 1e-30);
    check(corr > 0.98, "verbatim reproduces the take");
    check(peak > 0.3f && peak < 0.7f, "level sits with the other engines");
    for (float h : {0.0f, 0.3f, 0.7f, 1.0f}) for (float t : {0.0f, 1.0f}) for (float m : {0.0f, 1.0f})
      for (float mac : {0.0f, 0.5f, 1.0f}) for (float note : {0.0f, 60.0f, 127.0f}) for (float bpm : {40.0f, 300.0f}) {
        p.harmonics = h; p.timbre = t; p.morph = m; p.macro = mac; p.note = note; p.tempo = bpm; p.trigger = kTriggerRising;
        for (int b = 0; b < 600; ++b) {
          e.render(p, out, aux, kBlock);
          p.trigger = (b % 97 == 0) ? kTriggerRising : 0;
          for (float v : out) check(std::isfinite(v) && std::fabs(v) <= 1.0f, "extremes bounded");
        }
      }
    printf("BREAK ok (peak %.2f, verbatim corr %.4f)\n", peak, corr);
  }
  return 0;
}
