// Tape echo on the master bus. One mono loop of "tape": 0.6 s at 24 kHz and
// 12 bits (dsp/pack12.h, 21.6 KB). The whole echo runs at that rate, one
// step per output pair (pair-averaged in, interpolated out), which halves
// its cost. What makes it tape:
//   - the head glides to a new time instead of jumping (~80 ms), so a tempo
//     change bends the repeats in pitch;
//   - a slow wow (0.55 Hz, ~0.1 % in pitch) on the read head;
//   - the feedback path darkens (one-pole low-pass, ~3.2 kHz), thins (high-
//     pass, ~120 Hz) and saturates (cubic), so repeats wear down;
//   - switched off, the input stops reaching the tape but the loop keeps
//     turning, so the tail rings out.
// Pure — host-testable (echo_test.cpp).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core.h"
#include "pack12.h"
#include "tables.h"

namespace pd {

class TapeEcho {
 public:
  static constexpr uint32_t kRate = 24000;
  static constexpr uint32_t kLength = 14400;                 // 0.6 s
  static constexpr float kMaxSeconds = (float)(kLength - 64) / kRate;

  // Delay in seconds (clamped to the tape), or <= 0 for off.
  void setTime(float seconds) {
    on_ = seconds > 0.0f;
    if (on_) target_ = clamp(seconds, 0.01f, kMaxSeconds) * kRate;
    if (time_ <= 0.0f) time_ = target_;   // first use: no glide from zero
  }
  void setFeedback(float fb) { fb_ = clamp(fb, 0.0f, 0.95f); }
  void setMix(float mix) { mix_ = clamp(mix, 0.0f, 1.0f); }
  bool on() const { return on_; }

  // Adds the echo to io in place (48 kHz, n even). Out of line, so it keeps its
  // IRAM section instead of being inlined into the instrument's render (a
  // template member, which stays in flash).
  PD_HOT __attribute__((noinline)) void process(float* io, size_t n) {
    if (!on_ && quietFor_ > kQuietSkip) return;   // a whole silent loop: skip it
    // Everything runs at the tape's 24 kHz: one read, one trip round the
    // feedback path and one write per output pair; the wet signal goes back
    // up to 48 kHz by linear interpolation (the first of each pair half-way
    // from the last pair's value).
    const float* sine = sineTable().v;
    for (size_t i = 0; i + 1 < n; i += 2) {
      time_ += kGlide * (target_ - time_);
      wow_ += kWowInc;
      const float delay = time_ + kWowDepth * (1.0f + sineLut(sine, wow_));
      float r = (float)w_ - delay;
      if (r < 0.0f) r += (float)kLength;
      const uint32_t ri = (uint32_t)r;
      const float frac = r - (float)ri;
      const uint32_t rj = ri + 1 < kLength ? ri + 1 : 0;
      const float a = tape_.at(ri), b = tape_.at(rj);
      const float wet = (a + frac * (b - a)) * (1.0f / 32768.0f);

      // Feedback: darker, thinner, saturated.
      lp_ += kLowPass * (wet - lp_);
      hp_ += kHighPass * (lp_ - hp_);
      float f = clamp((lp_ - hp_) * fb_, -1.5f, 1.5f);
      f -= (4.0f / 27.0f) * f * f * f;

      const float in = on_ ? 0.5f * (io[i] + io[i + 1]) : 0.0f;   // pair average
      tape_.put(w_, (int)(clamp(in + f, -1.0f, 1.0f) * 32767.0f));
      if (++w_ == kLength) w_ = 0;

      io[i] += mix_ * 0.5f * (lastWet_ + wet);
      io[i + 1] += mix_ * wet;
      lastWet_ = wet;
      quietFor_ = std::fabs(wet) > 1e-4f || on_ ? 0 : quietFor_ + 2;
    }
  }


 private:
  // Per tape sample (24 kHz).
  static constexpr float kGlide = 1.0f / (0.08f * 24000.0f);
  static constexpr uint32_t kWowInc = (uint32_t)(0.55 / 24000.0 * 4294967296.0);
  static constexpr float kWowDepth = 7.0f;          // tape samples: ~0.1 % pitch at 0.55 Hz
  static constexpr float kLowPass = 0.567f;         // ~3.2 kHz at 24 kHz
  static constexpr float kHighPass = 0.031f;        // ~120 Hz
  static constexpr uint32_t kQuietSkip = 2 * kLength + 64;   // one loop, in output samples

  Pack12<kLength> tape_;
  uint32_t w_ = 0, wow_ = 0, quietFor_ = kQuietSkip + 1;
  float target_ = 0.25f * kRate, time_ = 0.0f;
  float fb_ = 0.45f, mix_ = 0.35f;
  float lp_ = 0.0f, hp_ = 0.0f, lastWet_ = 0.0f;
  bool on_ = false;
};

}  // namespace pd
