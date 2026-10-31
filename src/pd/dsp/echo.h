// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
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
// SPACE turns it toward a reverb: three more read heads at incommensurate
// fractions of the delay thicken each pass, and four allpass diffusers (1.7
// KB of int16) smear it, in the feedback and in what you hear. With the echo
// off, SPACE alone runs the tape as a small room: a 30-80 ms loop, feedback
// and level rising with the amount.
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
  void setSpace(float space) { space_ = clamp(space, 0.0f, 1.0f); }
  bool on() const { return on_; }

  // Adds the echo to io in place (48 kHz, n even). Out of line, so it keeps its
  // IRAM section instead of being inlined into the instrument's render (a
  // template member, which stays in flash).
  PD_HOT __attribute__((noinline)) void process(float* io, size_t n) {
    const bool room = !on_ && space_ > 0.0f;   // SPACE with the echo off
    const bool feeding = on_ || room;
    if (!feeding && quietFor_ > kQuietSkip) return;   // a whole silent loop: skip it
    const float space = space_;
    const float roomTime = (0.03f + 0.05f * space) * kRate;   // a room has no tape bend: no glide
    const float fb = room ? 0.5f + 0.4f * space : fb_;
    const float mix = room ? 0.15f + 0.35f * space : mix_;
    // Everything runs at the tape's 24 kHz: one read, one trip round the
    // feedback path and one write per output pair; the wet signal goes back
    // up to 48 kHz by linear interpolation (the first of each pair half-way
    // from the last pair's value).
    // The wow (0.55 Hz) moves 0.002 rad a block: its sine is taken at the
    // block's first and last tape steps and ramped between (off from the
    // per-step sine by ~1e-6 samples of delay). The glide stays per step: a
    // ramped one would wobble the delay at the block rate mid-glide.
    const float* sine = sineTable().v;
    const uint32_t steps = (uint32_t)(n / 2);
    const float wowFirst = sineLut(sine, wow_ + kWowInc);
    const float wowLast = sineLut(sine, wow_ + kWowInc * steps);
    const float wowStep = steps > 1 ? (wowLast - wowFirst) / (float)(steps - 1) : 0.0f;
    float wow = wowFirst;
    wow_ += kWowInc * steps;
    for (size_t i = 0; i + 1 < n; i += 2) {
      time_ += kGlide * (target_ - time_);
      const float delay = (room ? roomTime : time_) + kWowDepth * (1.0f + wow);
      wow += wowStep;
      float wet = readAt(delay);
      if (space > 0.0f) {
        // More heads, then diffusion: the repeat blurs into a wash.
        const float taps = (float)(tapAt(delay * 0.77f) + tapAt(delay * 0.59f) + tapAt(delay * 0.45f)) *
                           (1.0f / (3.0f * 32768.0f));
        float x = wet + space * (taps - wet) * 0.6f;
        for (int k = 0; k < kDiffusers; ++k) x = diffuse(k, x);
        wet += std::min(1.0f, 1.5f * space) * (x - wet);   // all diffused by 2/3
      }

      // Feedback: darker, thinner, saturated.
      lp_ += kLowPass * (wet - lp_);
      hp_ += kHighPass * (lp_ - hp_);
      float f = clamp((lp_ - hp_) * fb, -1.5f, 1.5f);
      f -= (4.0f / 27.0f) * f * f * f;

      const float in = feeding ? 0.5f * (io[i] + io[i + 1]) : 0.0f;   // pair average
      tape_.put(w_, (int)(clamp(in + f, -1.0f, 1.0f) * 32767.0f));
      if (++w_ == kLength) w_ = 0;

      io[i] += mix * 0.5f * (lastWet_ + wet);
      io[i + 1] += mix * wet;
      lastWet_ = wet;
      quietFor_ = std::fabs(wet) > 1e-4f || feeding ? 0 : quietFor_ + 2;
    }
  }

 private:
  // The tape `delay` tape-samples behind the writer, interpolated.
  inline float readAt(float delay) const {
    float r = (float)w_ - delay;
    if (r < 0.0f) r += (float)kLength;
    const uint32_t ri = (uint32_t)r;
    const float frac = r - (float)ri;
    int ia, ib;
    if (ri + 1 < kLength) tape_.pair(ri, &ia, &ib);
    else { ia = tape_.at(ri); ib = tape_.at(0); }   // the loop's seam
    const float a = (float)ia, b = (float)ib;
    return (a + frac * (b - a)) * (1.0f / 32768.0f);
  }

  // The nearest tape sample `delay` behind the writer: the extra heads feed
  // the diffusers, which smear away anything interpolation would add.
  inline int tapAt(float delay) const {
    int r = (int)w_ - (int)delay;
    if (r < 0) r += (int)kLength;
    return tape_.at((uint32_t)r);
  }

  // Schroeder allpass k: y = -g x + z, z <- x + g y. int16 holds +-2.
  inline float diffuse(int k, float x) {
    int16_t* buf = diffBuf_ + kDiffStart[k];
    uint16_t& pos = diffPos_[k];
    const float z = (float)buf[pos] * (1.0f / 16384.0f);
    const float y = -kDiffGain * x + z;
    buf[pos] = (int16_t)clamp((x + kDiffGain * y) * 16384.0f, -32768.0f, 32767.0f);
    if (++pos == kDiffLen[k]) pos = 0;
    return y;
  }



  // Per tape sample (24 kHz).
  static constexpr float kGlide = 1.0f / (0.08f * 24000.0f);
  static constexpr uint32_t kWowInc = (uint32_t)(0.55 / 24000.0 * 4294967296.0);
  static constexpr float kWowDepth = 7.0f;          // tape samples: ~0.1 % pitch at 0.55 Hz
  static constexpr float kLowPass = 0.567f;         // ~3.2 kHz at 24 kHz
  static constexpr float kHighPass = 0.031f;        // ~120 Hz
  static constexpr uint32_t kQuietSkip = 2 * kLength + 64;   // one loop, in output samples

  // Diffusers: 4.7 .. 14 ms at 24 kHz, mutually prime.
  static constexpr int kDiffusers = 4;
  static constexpr uint16_t kDiffLen[kDiffusers] = {113, 162, 241, 337};
  static constexpr uint16_t kDiffStart[kDiffusers] = {0, 113, 275, 516};
  static constexpr float kDiffGain = 0.6f;

  Pack12<kLength> tape_;
  int16_t diffBuf_[113 + 162 + 241 + 337] = {};
  uint16_t diffPos_[kDiffusers] = {};
  float space_ = 0.0f;
  uint32_t w_ = 0, wow_ = 0, quietFor_ = kQuietSkip + 1;
  float target_ = 0.25f * kRate, time_ = 0.0f;
  float fb_ = 0.45f, mix_ = 0.35f;
  float lp_ = 0.0f, hp_ = 0.0f, lastWet_ = 0.0f;
  bool on_ = false;
};

}  // namespace pd
