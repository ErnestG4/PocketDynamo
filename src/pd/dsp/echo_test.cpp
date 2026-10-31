// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test for dsp/echo.h: g++ -std=c++17 -O2 -I src/pd src/pd/dsp/echo_test.cpp
#include "dsp/echo.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pd;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) { ++failures; printf("FAIL %s\n", what); }
}

// Runs n samples of x through e, 32 at a time.
static std::vector<float> run(TapeEcho& e, std::vector<float> x) {
  for (size_t i = 0; i < x.size(); i += 32) e.process(&x[i], std::min<size_t>(32, x.size() - i));
  return x;
}

static size_t loudest(const std::vector<float>& y, size_t from, size_t to) {
  size_t at = from;
  for (size_t i = from; i < to && i < y.size(); ++i) if (std::fabs(y[i]) > std::fabs(y[at])) at = i;
  return at;
}

int main() {
  static TapeEcho e;   // 21.6 KB of tape: not on the stack
  e.setTime(0.25f); e.setFeedback(0.5f); e.setMix(0.35f);

  // A short click: the first repeat lands 0.25 s later (give or take the wow
  // and the half-rate tape), the second 0.5 s, each quieter.
  std::vector<float> x(48000, 0.0f);
  for (int i = 0; i < 48; ++i) x[100 + i] = 0.8f * std::sin(i * 0.4f);
  auto y = run(e, x);
  const size_t r1 = loudest(y, 100 + 2000, 100 + 18000), r2 = loudest(y, 100 + 18000, 100 + 30000);
  const float d1 = (float)(r1 - 100) / 48000.0f, d2 = (float)(r2 - 100) / 48000.0f;
  check(std::fabs(d1 - 0.25f) < 0.002f, "first repeat at the delay time");
  check(std::fabs(d2 - 0.50f) < 0.003f, "second repeat at twice it");
  check(std::fabs(y[r1]) > 0.1f && std::fabs(y[r1]) < 0.35f, "first repeat near the mix level");
  check(std::fabs(y[r2]) < std::fabs(y[r1]) * 0.7f, "repeats wear down");
  printf("repeats at %.4f s (%.3f) and %.4f s (%.3f)\n", d1, y[r1], d2, y[r2]);

  // Full-scale noise at maximum feedback stays bounded (the saturation and
  // the filters keep the loop below unity).
  TapeEcho* hot = new TapeEcho;
  hot->setTime(0.1f); hot->setFeedback(0.95f); hot->setMix(1.0f);
  std::vector<float> n(48000 * 4);
  uint32_t s = 1;
  for (auto& v : n) { s = s * 1664525u + 1013904223u; v = ((s >> 9) * (1.0f / 8388608.0f) - 1.0f) * 0.9f; }
  auto hn = run(*hot, n);
  float peak = 0; bool finite = true;
  for (float v : hn) { peak = std::max(peak, std::fabs(v)); finite = finite && std::isfinite(v); }
  check(finite && peak < 3.0f, "max feedback stays bounded");

  // Switched off: no new input reaches the tape, the tail still rings, then
  // dies away.
  hot->setTime(0.0f);
  std::vector<float> z(48000 * 8, 0.0f);
  auto tail = run(*hot, z);
  float early = 0, late = 0;
  for (size_t i = 0; i < 4800; ++i) early = std::max(early, std::fabs(tail[i]));
  for (size_t i = tail.size() - 4800; i < tail.size(); ++i) late = std::max(late, std::fabs(tail[i]));
  check(early > 0.01f, "the tail rings after switching off");
  check(late < 1e-3f, "and dies away");
  printf("max-feedback peak %.2f, tail %.3f -> %.5f\n", peak, early, late);
  delete hot;

  // A new time glides: no sample-to-sample jump larger than the signal
  // itself could make.
  static TapeEcho g;
  g.setTime(0.2f); g.setFeedback(0.3f); g.setMix(1.0f);
  std::vector<float> sine(48000 * 2);
  for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.5f * std::sin(i * 2.0f * 3.14159265f * 220.0f / 48000.0f);
  auto a = run(g, std::vector<float>(sine.begin(), sine.begin() + 48000));
  g.setTime(0.45f);
  auto b = run(g, std::vector<float>(sine.begin() + 48000, sine.end()));
  float jump = 0;
  for (size_t i = 1; i < b.size(); ++i) jump = std::max(jump, std::fabs(b[i] - b[i - 1]));
  check(jump < 0.2f, "a time change glides instead of clicking");
  printf("largest step after a time change %.3f\n", jump);

  // SPACE with the echo off: a small room. A click comes back as a dense
  // tail (not separate repeats) that decays, and noise at full SPACE stays
  // bounded.
  {
    static TapeEcho r;
    r.setTime(0.0f); r.setSpace(0.8f);
    std::vector<float> click(48000 * 3, 0.0f);
    for (int i = 0; i < 48; ++i) click[200 + i] = 0.8f * std::sin(i * 0.4f);
    auto t = run(r, click);
    // Every 10 ms window from 40 to 300 ms is dense (over half its samples
    // within 20 dB of the window's peak): a wash, not separate repeats.
    int sparse = 0;
    for (int w = 4; w < 30; ++w) {
      float m = 0; int busy = 0;
      for (int i = 0; i < 480; ++i) m = std::max(m, std::fabs(t[200 + w * 480 + i]));
      for (int i = 0; i < 480; ++i) busy += std::fabs(t[200 + w * 480 + i]) > 0.1f * m;
      sparse += busy < 240;
    }
    float late2 = 0;
    for (size_t i = t.size() - 4800; i < t.size(); ++i) late2 = std::max(late2, std::fabs(t[i]));
    check(sparse == 0, "room: a dense tail, not separate repeats");
    check(late2 < 0.01f, "room: the tail decays");
    printf("room: %d sparse windows of 26, tail end %.4f\n", sparse, late2);
    static TapeEcho rn;
    rn.setTime(0.0f); rn.setSpace(1.0f);
    auto big = run(rn, n);
    float pk = 0;
    for (float v : big) pk = std::max(pk, std::fabs(v));
    check(pk < 3.0f && std::isfinite(pk), "room at full SPACE stays bounded");
  }

  printf(failures ? "echo_test: %d FAILED\n" : "echo_test: ok\n", failures);
  return failures != 0;
}
