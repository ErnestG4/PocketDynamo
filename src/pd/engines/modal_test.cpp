// Host test: g++ -std=c++17 -O2 -Wall -I src/pd src/pd/engines/modal_test.cpp -o t && ./t [wav dir]
// With a directory argument it also writes a few 3 s mono 16-bit WAVs there.
#include "engines/modal.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace pd;

namespace {

constexpr size_t kBlock = 32;
constexpr int kBlocksPerSecond = (int)(kSampleRate / kBlock);

void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }

struct Stats { float peak = 0.0f; double energy = 0.0; };

EngineParams params(float note, float h, float t, float m, float x) {
  EngineParams p;
  p.note = note; p.harmonics = h; p.timbre = t; p.morph = m; p.macro = x;
  return p;
}

// Render `blocks` blocks; trigger on the first if asked. Every sample of both
// outputs must be finite and inside ±1, and every pole inside the circle.
Stats run(ModalEngine& e, EngineParams p, int blocks, bool strike, float* record = nullptr) {
  Stats s;
  float out[kBlock], aux[kBlock];
  for (int b = 0; b < blocks; ++b) {
    p.trigger = (strike && b == 0) ? kTriggerRising : 0;
    e.render(p, out, aux, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      check(std::isfinite(out[i]) && std::isfinite(aux[i]), "finite");
      check(std::fabs(out[i]) < 1.0f && std::fabs(aux[i]) < 1.0f, "inside ±1");
      s.peak = std::max(s.peak, std::fabs(out[i]));
      s.energy += (double)out[i] * out[i];
      if (record) *record++ = out[i];
    }
    check(e.maxPoleRadius() < 1.0f, "poles inside the unit circle");
  }
  return s;
}

void writeWav(const char* dir, const char* name, const float* x, int n) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s.wav", dir, name);
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "can't write %s\n", path); return; }
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  const uint32_t sr = (uint32_t)kSampleRate;
  fwrite("RIFF", 1, 4, f); u32(36 + 2 * n); fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(sr); u32(sr * 2); u16(2); u16(16);
  fwrite("data", 1, 4, f); u32(2 * n);
  for (int i = 0; i < n; ++i) u16((uint16_t)(int16_t)(clamp(x[i], -1.0f, 1.0f) * 32767.0f));
  fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const float vals[3] = {0.0f, 0.5f, 1.0f};
  const float notes[4] = {24.0f, 60.0f, 96.0f, 120.0f};

  // 1. Every control at 0 / 0.5 / 1 x four notes, 3 s each: finite, bounded,
  // stable. Strike peaks reported for the clean (MACRO noon) cases.
  float loPeak = 1.0f, hiPeak = 0.0f, allPeak = 0.0f, lo[4] = {};
  for (float h : vals) for (float t : vals) for (float m : vals) for (float x : vals)
    for (float n : notes) {
      ModalEngine e;
      const Stats s = run(e, params(n, h, t, m, x), 3 * kBlocksPerSecond, true);
      allPeak = std::max(allPeak, s.peak);
      if (x == 0.5f && n <= 96.0f) {
        if (s.peak < loPeak) { loPeak = s.peak; lo[0] = n; lo[1] = h; lo[2] = t; lo[3] = m; }
        hiPeak = std::max(hiPeak, s.peak);
      }
    }
  printf("grid: 324 runs of 3 s ok; clean strike peaks %.3f .. %.3f (notes 24-96; lowest at note %g H %g T %g M %g), max any %.3f\n",
         loPeak, hiPeak, lo[0], lo[1], lo[2], lo[3], allPeak);

  // 2. A fresh strike's peak, across HARMONICS at the default controls.
  printf("strike peak by HARMONICS (note 60, TIMBRE/MORPH 0.5, clean):");
  for (int i = 0; i <= 14; ++i) {
    ModalEngine e;
    const float h = (float)i / 14.0f;
    const Stats s = run(e, params(60.0f, h, 0.5f, 0.5f, 0.5f), kBlocksPerSecond / 5, true);
    printf(" %.2f", s.peak);
    check(s.peak > 0.25f && s.peak < 0.75f, "strike peak roughly 0.3..0.7");
  }
  printf("\n");

  // 3. Decay follows MORPH: short dies, long rings.
  for (float h : {0.0f, 3.0f / 7.0f, 6.0f / 7.0f}) {
    ModalEngine a, b;
    const EngineParams pa = params(60.0f, h, 0.5f, 0.0f, 0.5f), pb = params(60.0f, h, 0.5f, 1.0f, 0.5f);
    const Stats a0 = run(a, pa, kBlocksPerSecond / 5, true), a1 = run(a, pa, kBlocksPerSecond, false);
    const Stats b0 = run(b, pb, kBlocksPerSecond / 5, true);
    run(b, pb, 2 * kBlocksPerSecond, false);
    const Stats b1 = run(b, pb, kBlocksPerSecond / 5, false);   // 2.2 .. 2.4 s
    const double da = 10.0 * std::log10(a1.energy / a0.energy + 1e-30);
    const double db = 10.0 * std::log10(b1.energy / b0.energy + 1e-30);
    printf("decay h=%.2f: MORPH 0 next 1 s vs first 0.2 s %.1f dB (live modes %d); MORPH 1 at 2.2 s %.1f dB (live %d)\n",
           h, da, a.liveModes(), db, b.liveModes());
    check(da < -30.0, "MORPH low decays");
    check(db > -20.0, "MORPH high rings long");
    check(a.liveModes() == 0, "a dead note costs nothing");
  }

  // 4. Re-strikes while ringing add energy without blowing up: a bell under
  // a 60 ms ratchet for 3 s.
  {
    ModalEngine e;
    const EngineParams p = params(60.0f, 6.0f / 7.0f, 1.0f, 1.0f, 0.5f);
    run(e, p, kBlocksPerSecond / 2, true);
    const Stats before = run(e, p, 3, false);
    const Stats after = run(e, p, 3, true);
    check(after.energy > before.energy, "a re-strike adds energy");
    float peak = 0.0f;
    for (int i = 0; i < 50; ++i) peak = std::max(peak, run(e, p, 60 * kBlocksPerSecond / 1000, true).peak);
    printf("ratchet: energy before/after one re-strike %.3g/%.3g, peak over 50 re-strikes %.3f\n",
           before.energy, after.energy, peak);
    check(peak < 0.95f, "ratchet stays bounded");
  }

  // 5. Retune mid-ring: the level carried through a bend (against the same
  // note unbent), then a glide, a jump of a fifth, HARMONICS swept through
  // every set and MORPH swept, all under one ringing note.
  {
    ModalEngine e, ref;
    EngineParams p = params(60.0f, 0.0f, 0.7f, 0.9f, 0.5f);
    run(e, p, kBlocksPerSecond / 4, true);
    run(ref, p, kBlocksPerSecond / 4, true);
    const Stats same = run(ref, p, kBlocksPerSecond / 10, false);
    p.note = 61.0f;
    const Stats bent = run(e, p, kBlocksPerSecond / 10, false);
    printf("retune: a semitone bend mid-ring keeps %.2f dB of the unbent level\n",
           10.0 * std::log10(bent.energy / same.energy));
    check(bent.energy < 1.3 * same.energy && bent.energy > 0.7 * same.energy, "a bend keeps the level");
    for (int b = 0; b < kBlocksPerSecond; ++b) { p.note = 60.0f + 12.0f * b / kBlocksPerSecond; run(e, p, 1, false); }
    p.note = 79.0f; run(e, p, kBlocksPerSecond / 4, false);
    for (int b = 0; b < 2 * kBlocksPerSecond; ++b) {
      p.harmonics = (float)b / (2 * kBlocksPerSecond);
      p.morph = 0.5f + 0.5f * std::sin(0.01f * b);
      run(e, p, 1, false);
    }
    for (int b = 0; b < kBlocksPerSecond; ++b) { p.note = 0.0f + 127.0f * b / kBlocksPerSecond; run(e, p, 1, false); }
    printf("retune: glide, jump, HARMONICS/MORPH sweep, 0..127 sweep ok\n");
  }

  // 6. Steals: a new note on a ringing voice leaves a tail; rapid steals stay
  // bounded and stable.
  {
    ModalEngine e;
    run(e, params(48.0f, 6.0f / 7.0f, 1.0f, 1.0f, 0.0f), kBlocksPerSecond / 2, true);
    run(e, params(55.0f, 6.0f / 7.0f, 1.0f, 1.0f, 0.0f), 1, true);
    check(e.tailModes() > 0, "the old note rings on as a tail");
    float peak = 0.0f;
    for (int i = 0; i < 100; ++i)
      peak = std::max(peak, run(e, params(40.0f + (i * 7) % 60, (i % 8) / 7.0f, 1.0f, 1.0f, (i % 3) * 0.5f), 4, true).peak);
    run(e, params(60.0f, 0.5f, 0.5f, 1.0f, 0.5f), kBlocksPerSecond / 10, false);
    check(e.tailModes() == 0, "the tail ends");
    printf("steals: 100 strikes 4 blocks apart, peak %.3f\n", peak);
  }

  // 7. Poles at the extremes: lowest and highest notes, shortest and longest
  // decays, every set, mid-choke and mid-tail.
  {
    float worst = 0.0f;
    for (float n : {-128.0f, 0.0f, 127.0f}) for (int s = 0; s < 8; ++s) for (float m : {0.0f, 1.0f}) {
      ModalEngine e;
      const EngineParams p = params(n, s / 7.0f, 1.0f, m, 0.5f);
      run(e, p, 10, true);
      run(e, p, 1, true);                              // a re-strike: the choke
      worst = std::max(worst, e.maxPoleRadius());
      run(e, params(n + 7.0f, s / 7.0f, 1.0f, m, 0.5f), 1, true);   // a steal: the tail
      worst = std::max(worst, e.maxPoleRadius());
    }
    printf("poles: worst radius %.8f\n", worst);
    check(worst < 1.0f, "stable");
  }

  // 8. Pickup bark rises with MACRO: second harmonic over the fundamental on
  // the tine, by correlation with the bank's own pitch.
  for (float x : {0.55f, 0.75f, 1.0f}) {
    ModalEngine e;
    static float buf[kBlocksPerSecond * kBlock / 2];
    run(e, params(48.0f, 4.0f / 7.0f, 0.3f, 0.7f, x), kBlocksPerSecond / 2, true, buf);
    const int n0 = (int)(0.1f * kSampleRate), n1 = (int)(0.5f * kSampleRate) - 1;
    auto mag = [&](float hz) {
      double re = 0, im = 0;
      for (int i = n0; i < n1; ++i) { re += buf[i] * std::cos(6.283185307 * hz * i / kSampleRate); im += buf[i] * std::sin(6.283185307 * hz * i / kSampleRate); }
      return std::sqrt(re * re + im * im);
    };
    const double f0 = 440.0 * std::pow(2.0, (48.0 - 69.0) / 12.0);
    printf("pickup MACRO %.2f: h2 re h1 %+.1f dB\n", x, 20.0 * std::log10(mag(2 * f0) / mag(f0)));
  }

  // 9. A voice reset and struck again sounds exactly as a new one would: the
  // coefficients and caches reset() keeps are only ever what the controls
  // build. Both the rebuild path (another note, other controls) and the
  // no-rebuild path (the same note and controls) — and a NaN control is
  // harmless.
  {
    float oa[kBlock], aa[kBlock], ob[kBlock], ab[kBlock];
    const EngineParams first = params(72.0f, 0.2f, 0.9f, 0.3f, 0.5f);
    for (const EngineParams& second : {params(48.0f, 6.0f / 7.0f, 0.3f, 0.8f, 0.7f), first}) {
      ModalEngine used, fresh;
      run(used, first, 40, true);
      used.reset();
      float worst = 0.0f;
      for (int b = 0; b < kBlocksPerSecond / 2; ++b) {
        EngineParams p = second;
        p.trigger = b == 0 ? kTriggerRising : 0;
        used.render(p, oa, aa, kBlock);
        fresh.render(p, ob, ab, kBlock);
        for (size_t i = 0; i < kBlock; ++i) worst = std::max(worst, std::fabs(oa[i] - ob[i]) + std::fabs(aa[i] - ab[i]));
      }
      check(worst < 1e-6f, "a reset voice strikes as a new one");
    }
    ModalEngine e;
    EngineParams p = params(60.0f, 0.5f, 0.5f, 0.5f, 0.5f);
    run(e, p, 10, true);
    p.harmonics = p.timbre = p.morph = p.macro = p.note = std::nanf("");
    run(e, p, 10, true);
    printf("reset reuse matches a new voice; NaN controls stay finite\n");
  }

  // 10. Cost proxy: a strike's setup (rebuild + strike, one-sample render)
  // against a steady 16-mode block, on the host. Printed, not asserted: the
  // ratio, not the time, is what carries to the LX7 (where a steady 16-mode
  // voice-block bench at ~8k cycles), and only roughly.
  {
    using clk = std::chrono::steady_clock;
    static ModalEngine v[8];
    float o[kBlock], a[kBlock];
    EngineParams p = params(50.0f, 6.0f / 7.0f, 1.0f, 1.0f, 0.5f);
    for (int i = 0; i < 8; ++i) { p.trigger = kTriggerRising; v[i].render(p, o, a, kBlock); }
    p.trigger = 0;
    for (int b = 0; b < 50; ++b) for (int i = 0; i < 8; ++i) v[i].render(p, o, a, kBlock);
    auto time = [&](auto&& f) {
      const int n = 40000;
      const auto t0 = clk::now();
      for (int i = 0; i < n; ++i) f(i);
      return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / n;
    };
    const double steady = time([&](int i) { v[i & 7].render(p, o, a, kBlock); });
    const double idle = time([&](int i) { v[i & 7].reset(); p.trigger = 0; v[i & 7].render(p, o, a, 1); });
    const double fresh = time([&](int i) {
      v[i & 7].reset(); p.note = 40.0f + (float)(i & 15); p.trigger = kTriggerRising; v[i & 7].render(p, o, a, 1); });
    for (int i = 0; i < 8; ++i) { p.note = 50.0f; p.trigger = kTriggerRising; v[i].render(p, o, a, kBlock); }
    const double again = time([&](int i) { p.note = 50.0f; p.trigger = kTriggerRising; v[i & 7].render(p, o, a, 1); });
    const double steal = time([&](int i) {
      p.note = 40.0f + 12.0f * (float)((i >> 3) & 1); p.trigger = kTriggerRising; v[i & 7].render(p, o, a, 1); });
    p.trigger = 0;
    const double glide = time([&](int i) { p.note = 50.0f + 0.01f * (float)(i & 63); v[i & 7].render(p, o, a, 1); });
    printf("cost proxy (host, in steady 16-mode blocks): fresh strike %.2f, re-strike %.2f, steal %.2f, glide rebuild %.2f\n",
           (fresh - idle) / steady, (again - idle) / steady, (steal - idle) / steady, (glide - idle) / steady);
  }

  // 11. Optional WAVs.
  if (argc > 1) {
    struct Patch { const char* name; float note, h, t, m, x; };
    const Patch patches[] = {
      {"string_pluck", 48, 0.0f, 0.6f, 0.25f, 0.5f},
      {"marimba", 60, 2 / 7.0f, 0.35f, 0.4f, 0.5f},
      {"glock", 84, 3 / 7.0f, 0.9f, 0.75f, 0.5f},
      {"kalimba", 60, 4 / 7.0f, 0.55f, 0.5f, 0.5f},
      {"rhodes", 52, 4 / 7.0f, 0.3f, 0.7f, 0.85f},
      {"tom", 43, 5 / 7.0f, 0.4f, 0.2f, 0.5f},
      {"bell", 67, 6 / 7.0f, 0.8f, 0.9f, 0.5f},
      {"gong_wash", 36, 1.0f, 0.7f, 0.95f, 0.1f},
      {"morph_sweep", 55, 0.0f, 0.8f, 1.0f, 0.5f},
    };
    static float buf[3 * kBlocksPerSecond * kBlock];
    for (const Patch& q : patches) {
      ModalEngine e;
      EngineParams p = params(q.note, q.h, q.t, q.m, q.x);
      float* w = buf;
      for (int b = 0; b < 3 * kBlocksPerSecond; ++b) {
        const bool sweep = !strcmp(q.name, "morph_sweep");
        if (sweep) p.harmonics = (float)b / (3 * kBlocksPerSecond);
        const bool hit = b == 0 || (sweep && b % (kBlocksPerSecond / 2) == 0);
        run(e, p, 1, hit, w);
        w += kBlock;
      }
      writeWav(argv[1], q.name, buf, 3 * kBlocksPerSecond * (int)kBlock);
    }
    printf("wavs written to %s\n", argv[1]);
  }

  printf("modal ok\n");
  return 0;
}
