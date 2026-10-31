// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -Wall -I src/pd src/pd/engines/brocot_fm_test.cpp -o t && ./t
#include "brocot_fm.h"

#include <cassert>
#include <cstdio>

using namespace pd;

namespace {

constexpr size_t kBlock = 32;

void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }

struct Stats { float peak = 0.0f, aux_peak = 0.0f; bool bounded = true; };

// Render `blocks` blocks, triggering on the first; optional capture of out.
Stats run(BrocotFmEngine& e, EngineParams p, int blocks, float* capture = nullptr,
          bool trigger = true) {
  Stats s;
  float out[kBlock], aux[kBlock];
  for (int b = 0; b < blocks; ++b) {
    p.trigger = (trigger && b == 0) ? kTriggerRising : 0;
    e.render(p, out, aux, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      if (!std::isfinite(out[i]) || !std::isfinite(aux[i]) || std::fabs(out[i]) > 1.0f ||
          std::fabs(aux[i]) > 1.0f)
        s.bounded = false;
      s.peak = std::max(s.peak, std::fabs(out[i]));
      s.aux_peak = std::max(s.aux_peak, std::fabs(aux[i]));
      if (capture) capture[b * kBlock + i] = out[i];
    }
  }
  return s;
}

int blocksFor(float seconds) { return (int)(seconds * kSampleRate / kBlock); }

// Brightness: rms(first difference) / rms(signal). A pure sine at f gives
// 2·sin(π f / sr); FM sidebands push it up.
float brightness(const float* x, int from, int to) {
  double d = 0.0, s = 0.0;
  for (int i = from + 1; i < to; ++i) { d += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); s += x[i] * x[i]; }
  return (float)std::sqrt(d / std::max(s, 1e-12));
}

// Fraction of a window's energy lying on multiples of `base_hz` (exact DFT
// bins when the window is a whole number of base periods). True FM with an
// undetuned carrier puts every line on f_c·(1 + Σ n_k p_k/q_k), i.e. on
// multiples of f_c / lcm(q); a carrier shifted by modulator DC moves off them.
float latticeEnergy(const float* x, int len, float base_hz) {
  double total = 0.0;
  for (int i = 0; i < len; ++i) total += (double)x[i] * x[i];
  double on = 0.0;
  for (int k = 0; base_hz * k < kSampleRate * 0.5f; ++k) {
    const double w = 2.0 * 3.14159265358979323846 * base_hz * k / kSampleRate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < len; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double power = s1 * s1 + s2 * s2 - c * s1 * s2;   // |X_k|^2
    on += (k == 0 ? 1.0 : 2.0) * power / len;
  }
  return (float)(on / total);
}

float hzToNote(float hz) { return 69.0f + 12.0f * std::log2(hz / 440.0f); }

uint32_t gP, gQ;
bool ratioIs(const char* path, uint32_t p, uint32_t q) {
  SbPath(path).walk(&gP, &gQ);
  return gP == p && gQ == q;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  // --- Stern-Brocot paths (Brocot convention: R = 2/1, L = 1/2) ---
  check(ratioIs("", 1, 1), "root is 1/1");
  check(ratioIs("R", 2, 1) && ratioIs("L", 1, 2), "R = 2/1, L = 1/2");
  check(ratioIs("RL", 3, 2) && ratioIs("RLL", 4, 3) && ratioIs("RLLL", 5, 4), "RL 3/2, RLL 4/3, RLLL 5/4");
  check(ratioIs("RLLLR", 9, 7), "Avatar RLLLR = 9/7");
  check(ratioIs("RLRLRLRL", 55, 34), "Golden RLRLRLRL = 55/34 (Fibonacci convergent)");
  check(ratioIs("RRLRRLRR", 41, 15), "Silver trunk = 41/15");
  check(ratioIs("LLLL", 1, 5) && ratioIs("RRRR", 5, 1), "straight trunks");
  check(std::fabs(SbPath("RLLLR").ratio() - 9.0f / 7.0f) < 1e-6f, "ratio() float");
  {
    const SbPath t("RLLLRLRL");
    check(t.prefix(5) == SbPath("RLLLR") && t.prefix(99) == t && t.prefix(0) == SbPath(), "prefix");
    check(t.parent() == SbPath("RLLLRLR") && SbPath().parent() == SbPath(), "parent");
    check(t.startsWith(SbPath("RLL")) && !t.startsWith(SbPath("RR")), "startsWith");
    check(t.commonPrefix(SbPath("RLLR")) == SbPath("RLL"), "commonPrefix");
    check(SbPath("RL").extended(false) == SbPath("RLL"), "extended");
    SbPath f;
    check(SbPath::fromRatio(9, 7, &f) && f == SbPath("RLLLR"), "fromRatio 9/7");
    check(SbPath::fromRatio(18, 14, &f) && f == SbPath("RLLLR"), "fromRatio unreduced");
    check(SbPath::fromRatio(1, 1, &f) && f.depth() == 0, "fromRatio 1/1 is root");
    check(!SbPath::fromRatio(0, 3, &f) && !SbPath::fromRatio(100, 1, &f), "fromRatio failures");
    SbPath deep;   // 32 alternating steps: the fastest-growing path, no overflow
    for (int i = 0; i < 32; ++i) deep = deep.extended(i % 2 == 0);
    deep.walk(&gP, &gQ);
    check(gP == 5702887u && gQ == 3524578u, "32-step Fibonacci path");
    check(SbPath::fromRatio(gP, gQ, &f) && f == deep, "fromRatio round trip");
  }
  printf("stern-brocot: ok\n");

  check(sizeof(BrocotFmEngine) <= 1536, "RAM per instance");
  printf("sizeof(BrocotFmEngine) = %zu B\n", sizeof(BrocotFmEngine));

  // --- every control at 0 / 0.5 / 1 x notes, 2 s each: finite, bounded ---
  {
    BrocotFmEngine e;
    const float vals[3] = {0.0f, 0.5f, 1.0f};
    float worst = 0.0f, worst_aux = 0.0f, least = 1.0f;
    int cases = 0;
    for (float note : {24.0f, 60.0f, 96.0f, 120.0f})
      for (float h : vals) for (float t : vals) for (float m : vals) for (float mc : vals) {
        EngineParams p;
        p.note = note; p.harmonics = h; p.timbre = t; p.morph = m; p.macro = mc;
        const Stats s = run(e, p, blocksFor(2.0f));
        check(s.bounded, "finite and within +-1 at every control extreme");
        worst = std::max(worst, s.peak);
        worst_aux = std::max(worst_aux, s.aux_peak);
        least = std::min(least, s.peak);
        ++cases;
      }
    // Notes 0..127 at the brightest corner, plus fresh-engine starts.
    for (int note = 0; note <= 127; note += 1) {
      BrocotFmEngine f;
      EngineParams p;
      p.note = (float)note; p.harmonics = 0.0f; p.timbre = 1.0f; p.morph = 1.0f; p.macro = 1.0f;
      check(run(f, p, blocksFor(0.1f)).bounded, "bounded across notes 0..127");
    }
    printf("sweep: %d cases x 2 s, peak %.3f..%.3f, aux peak <= %.3f\n", cases, least, worst, worst_aux);
    check(least >= 0.3f && worst <= 0.7f, "held-note peak in 0.3..0.7");
  }

  // --- trigger: macro CW strikes (bright then decays), CCW swells ---
  {
    const int len = blocksFor(2.0f) * kBlock;
    static float buf[200000];
    BrocotFmEngine e;
    EngineParams p;
    p.note = 60.0f; p.harmonics = 0.5f; p.timbre = 0.0f; p.morph = 0.5f; p.macro = 1.0f;
    run(e, p, blocksFor(2.0f), buf);
    const float early = brightness(buf, 0, 2400), late = brightness(buf, len - 9600, len);
    const float sine = 2.0f * std::sin(3.14159265f * noteToDt(60.0f));
    printf("strike: brightness early %.4f late %.4f (pure sine %.4f)\n", early, late, sine);
    check(early > 3.0f * late, "strike is bright at onset");
    check(std::fabs(late - sine) < 0.02f * sine, "strike decays to a pure sine at TIMBRE 0");
    // Re-strike without reset: brightness comes back.
    run(e, p, blocksFor(0.05f), buf);
    check(brightness(buf, 0, 2400) > 3.0f * late, "re-trigger restarts the strike");
    // Once decayed, rendering on without a trigger stays dark.
    run(e, p, blocksFor(1.5f), buf, false);
    run(e, p, blocksFor(0.05f), buf, false);
    check(brightness(buf, 0, 2400) < 1.1f * late, "no trigger, no strike");

    p.macro = 0.0f; p.timbre = 0.7f;
    run(e, p, blocksFor(2.0f), buf);
    const float s_early = brightness(buf, 0, 2400), s_late = brightness(buf, len - 9600, len);
    printf("swell: brightness early %.4f late %.4f\n", s_early, s_late);
    check(s_late > 2.0f * s_early, "swell grows");

    p.macro = 0.5f;
    run(e, p, blocksFor(2.0f), buf);
    const float n_early = brightness(buf, 2400, 4800), n_late = brightness(buf, len - 9600, len);
    check(std::fabs(n_early - n_late) < 0.05f * n_late, "macro noon is steady");
  }

  // --- true-FM DC caveat: non-sine modulators must not detune the carrier ---
  {
    struct Case { float harmonics, base_hz, fc_hz; const char* name; };
    // Slot 2: 2, 3/2, 4/3 -> lattice f_c/6. Slot 4 (Avatar): 2, 4/3, 9/7 -> f_c/21.
    const Case cases[] = {{2.5f / 9.0f, 40.0f, 240.0f, "2:3/2:4/3"},
                          {4.5f / 9.0f, 10.0f, 210.0f, "Avatar"}};
    static float buf[200000];
    const int settle = blocksFor(0.5f), window = (int)kSampleRate;   // 1 s, whole periods
    for (const Case& c : cases)
      for (float morph : {0.0f, 0.5f, 0.75f, 0.875f, 1.0f}) {
        BrocotFmEngine e;
        EngineParams p;
        p.note = hzToNote(c.fc_hz); p.harmonics = c.harmonics; p.timbre = 0.6f;
        p.morph = morph; p.macro = 0.5f;
        run(e, p, settle);
        run(e, p, window / kBlock, buf, false);
        const float frac = latticeEnergy(buf, window, c.base_hz);
        printf("dc: %-9s morph %.3f  energy on lattice %.5f\n", c.name, morph, frac);
        check(frac > 0.995f, "carrier stays on its FM lattice (no DC detune)");
        // From a fresh engine the measured-DC window settles within ~25 ms.
        BrocotFmEngine f;
        run(f, p, blocksFor(0.1f), buf);
        check(latticeEnergy(buf + 1200, 3600, c.base_hz) > 0.99f, "onset settles onto the lattice");
      }
  }

  // --- HARMONICS sweep mid-note stays bounded (ratio glides) ---
  {
    BrocotFmEngine e;
    EngineParams p;
    p.note = 48.0f; p.timbre = 0.8f; p.morph = 0.25f;
    float out[kBlock], aux[kBlock];
    for (int b = 0; b < blocksFor(3.0f); ++b) {
      p.harmonics = 0.5f + 0.5f * std::sin(b * 0.002f);
      p.trigger = b == 0 ? kTriggerRising : 0;
      e.render(p, out, aux, kBlock);
      for (float x : out) check(std::isfinite(x) && std::fabs(x) <= 1.0f, "sweep bounded");
    }
  }

  printf("brocot_fm: all ok\n");
  return 0;
}
