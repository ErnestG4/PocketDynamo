// Host test for dsp/fx.h: g++ -std=c++17 -O2 -I src/pd src/pd/dsp/fx_test.cpp
#include "dsp/fx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

using namespace pd;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) { ++failures; printf("FAIL %s\n", what); }
}

static std::vector<float> sine(float hz, float amp, size_t n) {
  std::vector<float> x(n);
  for (size_t i = 0; i < n; ++i) x[i] = amp * std::sin(6.2831853f * hz * (float)i / kSampleRate);
  return x;
}
static std::vector<float> run(MasterFx& fx, std::vector<float> x) {
  for (size_t i = 0; i < x.size(); i += 32) fx.process(&x[i], 32);
  return x;
}
static float rms(const std::vector<float>& x, size_t from) {
  double e = 0; for (size_t i = from; i < x.size(); ++i) e += x[i] * x[i];
  return (float)std::sqrt(e / (x.size() - from));
}

int main() {
  const size_t N = 48000 / 2;
  // Every slot at 0: bit-exact dry.
  {
    MasterFx fx;
    auto x = sine(440, 0.7f, N);
    auto y = run(fx, x);
    check(memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0, "amount 0 bypasses exactly");
  }
  // Filters: LP at 0.6 cuts 8 kHz far more than 200 Hz; HP the reverse; BP
  // passes its centre and cuts an octave-plus away.
  {
    MasterFx lp; lp.set(MasterFx::Tone, 0, 0.6f);
    const float lo = rms(run(lp, sine(200, 0.5f, N)), 4800), hi = rms(run(lp, sine(8000, 0.5f, N)), 4800);
    check(lo > 0.3f && hi < 0.05f, "LP keeps lows, cuts highs");
    MasterFx hp; hp.set(MasterFx::Tone, 2, 0.6f);
    const float lo2 = rms(run(hp, sine(60, 0.5f, N)), 4800), hi2 = rms(run(hp, sine(8000, 0.5f, N)), 4800);
    check(lo2 < 0.05f && hi2 > 0.3f, "HP cuts lows, keeps highs");
    MasterFx bp; bp.set(MasterFx::Tone, 1, 0.5f);   // centre 100 * 80^0.5 = 894 Hz
    const float c = rms(run(bp, sine(894, 0.5f, N)), 4800), off = rms(run(bp, sine(7000, 0.5f, N)), 4800);
    check(c > 0.3f && off < 0.1f, "BP peaks at its centre");
    printf("LP 200/8k %.3f/%.3f  HP 60/8k %.3f/%.3f  BP c/off %.3f/%.3f\n", lo, hi, lo2, hi2, c, off);
  }
  // Once the amount has glided in: CRUSH at full uses few levels; PWM is
  // three-level (+1, 0, -1), and on silence it is silent (no carrier tone).
  {
    MasterFx cr; cr.set(MasterFx::Grit, 0, 1.0f);
    auto y = run(cr, sine(220, 0.5f, N));   // under the limiter, so its levels are the crush's
    std::set<float> levels(y.begin() + 4800, y.end());
    check(levels.size() <= 9, "CRUSH at full: a handful of levels");
    MasterFx pw; pw.set(MasterFx::Grit, 1, 1.0f);
    auto z = run(pw, sine(220, 0.6f, N));
    std::set<float> three(z.begin() + 4800, z.end());
    check(three.size() <= 3, "PWM at full: pulses of +-1 or nothing");
    MasterFx ps; ps.set(MasterFx::Grit, 1, 1.0f);
    auto q = run(ps, std::vector<float>(N, 0.0f));
    float qp = 0; for (float v : q) qp = std::max(qp, std::fabs(v));
    check(qp == 0.0f, "PWM on silence is silent");
    printf("CRUSH levels %zu, PWM levels %zu, PWM on silence %.3f\n", levels.size(), three.size(), qp);
  }
  // The filter on a hot square (a ZX voice) never pushes past full scale:
  // the limiter turns the ringing edges down instead of the clip crushing
  // them.
  {
    std::vector<float> sq(N);
    for (size_t i = 0; i < N; ++i) sq[i] = ((i / 120) & 1) ? 0.95f : -0.95f;
    float worst = 0;
    for (int t = 0; t < 3; ++t)
      for (float a : {0.2f, 0.5f, 0.8f}) {
        MasterFx f; f.set(MasterFx::Tone, t, a);
        auto y = run(f, sq);
        for (size_t i = 2400; i < y.size(); ++i) worst = std::max(worst, std::fabs(y[i]));
      }
    check(worst <= 0.951f, "filtered squares stay under the limiter's 0.95");
    printf("filtered square peak %.3f\n", worst);
  }
  // Everything at full on a hot, noisy bus stays finite and bounded.
  {
    std::vector<float> hot(N);
    uint32_t s = 7;
    for (auto& v : hot) { s = s * 1664525u + 1013904223u; v = ((s >> 9) * (1.0f / 8388608.0f) - 1.0f) * 1.4f; }
    for (int t0 = 0; t0 < 3; ++t0) for (int t1 = 0; t1 < 3; ++t1) for (int t2 = 0; t2 < 3; ++t2) {
      MasterFx fx;
      fx.set(MasterFx::Tone, t0, 1.0f); fx.set(MasterFx::Grit, t1, 1.0f); fx.set(MasterFx::Shape, t2, 1.0f);
      auto y = run(fx, hot);
      float pk = 0; bool fin = true;
      for (float v : y) { pk = std::max(pk, std::fabs(v)); fin = fin && std::isfinite(v); }
      if (!(fin && pk < 8.0f)) { printf("  %s %s %s peak %.2f\n", MasterFx::name(0, t0), MasterFx::name(1, t1), MasterFx::name(2, t2), pk); check(false, "bounded at full"); }
    }
  }
  printf(failures ? "fx_test: %d FAILED\n" : "fx_test: ok\n", failures);
  return failures != 0;
}
