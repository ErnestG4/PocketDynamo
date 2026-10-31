// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tape echo on the master bus. One mono loop of "tape": 1.5 s at 16 kHz and
// 12 bits (dsp/pack12.h, 36 KB). The whole echo runs at that rate, one step
// per three output samples (low-passed and averaged in, interpolated out),
// which cuts its cost to a third; the repeats keep everything under ~6 kHz,
// and the feedback path darkens them further anyway. What makes it tape:
//   - the head glides to a new time instead of jumping (~80 ms), so a tempo
//     change bends the repeats in pitch;
//   - a slow wow (0.55 Hz, ~0.1 % in pitch) on the read head;
//   - the feedback path darkens (one-pole low-pass, ~3.2 kHz), thins (high-
//     pass, ~120 Hz) and saturates (cubic), so repeats wear down;
//   - switched off, the input stops reaching the tape but the loop keeps
//     turning, so the tail rings out.
// SPACE turns it toward a reverb: three more read heads at incommensurate
// fractions of the delay thicken each pass, and four allpass diffusers (1.1
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
  static constexpr uint32_t kRate = 16000;
  static constexpr uint32_t kDecim = 3;                      // 48 kHz output samples per tape step
  static constexpr uint32_t kLength = 24000;                 // 1.5 s
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

  // Adds the echo to io in place (48 kHz, any n). Out of line, so it keeps its
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
    // Everything runs at the tape's 16 kHz: one read, one trip round the
    // feedback path and one write per three output samples. The input is
    // low-passed (6 kHz, 12 dB/oct) and averaged over the three, so what's
    // above the tape's 8 kHz doesn't fold back down; the wet signal goes back
    // up to 48 kHz by linear interpolation from the last step's value. 32
    // samples aren't a whole number of steps, so the step phase (sub_) and
    // its half-done input carry from block to block.
    // The wow (0.55 Hz) moves 0.002 rad a block: its sine is taken at the
    // block's first and last tape steps and ramped between (off from the
    // per-step sine by ~1e-6 samples of delay). The glide stays per step: a
    // ramped one would wobble the delay at the block rate mid-glide.
    const float* sine = sineTable().v;
    const uint32_t first = (kDecim - sub_) % kDecim;   // where this block's first step starts
    const uint32_t steps = first < n ? (uint32_t)((n - 1 - first) / kDecim + 1) : 0;
    const float wowFirst = sineLut(sine, wow_ + kWowInc);
    const float wowLast = sineLut(sine, wow_ + kWowInc * steps);
    const float wowStep = steps > 1 ? (wowLast - wowFirst) / (float)(steps - 1) : 0.0f;
    float wow = wowFirst;
    wow_ += kWowInc * steps;
    float z1 = aa1_, z2 = aa2_, acc = acc_;
    for (size_t i = 0; i < n; ++i) {
      if (sub_ == 0) {   // a tape step: read, and the feedback for this step's write
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
        feedback_ = f;
        prevWet_ = lastWet_;
        lastWet_ = wet;
      }

      // The input, anti-aliased (biquad, transposed direct form II) and
      // summed over the step.
      const float x = feeding ? io[i] : 0.0f;
      const float y = kAaB0 * x + z1;
      z1 = kAaB1 * x - kAaA1 * y + z2;
      z2 = kAaB0 * x - kAaA2 * y;
      acc += y;

      io[i] += mix * (prevWet_ + kOut[sub_] * (lastWet_ - prevWet_));

      if (++sub_ == kDecim) {   // the step's write
        sub_ = 0;
        tape_.put(w_, (int)(clamp(acc * (1.0f / kDecim) + feedback_, -1.0f, 1.0f) * 32767.0f));
        if (++w_ == kLength) w_ = 0;
        acc = 0.0f;
        quietFor_ = std::fabs(lastWet_) > 1e-4f || feeding ? 0 : quietFor_ + kDecim;
      }
    }
    // Flush denormals so a long silence can't slow the loop.
    aa1_ = std::fabs(z1) < 1e-20f ? 0.0f : z1;
    aa2_ = std::fabs(z2) < 1e-20f ? 0.0f : z2;
    acc_ = acc;
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



  // Per tape sample (16 kHz).
  static constexpr float kGlide = 1.0f / (0.08f * 16000.0f);
  static constexpr uint32_t kWowInc = (uint32_t)(0.55 / 16000.0 * 4294967296.0);
  static constexpr float kWowDepth = 4.667f;        // tape samples: ~0.1 % pitch at 0.55 Hz
  static constexpr float kLowPass = 0.7154f;        // ~3.2 kHz at 16 kHz
  static constexpr float kHighPass = 0.0460f;       // ~120 Hz
  static constexpr uint32_t kQuietSkip = kDecim * kLength + 64;   // one loop, in output samples

  // Per output sample (48 kHz): the input's anti-alias low-pass, Butterworth
  // at 6 kHz (b2 = b0), and the output's interpolation from the last step.
  static constexpr float kAaB0 = 0.097630761f, kAaB1 = 0.195261522f;
  static constexpr float kAaA1 = -0.942806028f, kAaA2 = 0.333329071f;
  static constexpr float kOut[kDecim] = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f};

  // Diffusers: 4.6 .. 14 ms, all prime: the nearest primes to the 24 kHz
  // tape's 113 / 162 / 241 / 337 samples, so the room sounds the same.
  static constexpr int kDiffusers = 4;
  static constexpr uint16_t kDiffLen[kDiffusers] = {73, 109, 163, 223};
  static constexpr uint16_t kDiffStart[kDiffusers] = {0, 73, 182, 345};
  static constexpr float kDiffGain = 0.6f;

  Pack12<kLength> tape_;
  int16_t diffBuf_[73 + 109 + 163 + 223] = {};
  uint16_t diffPos_[kDiffusers] = {};
  float space_ = 0.0f;
  uint32_t w_ = 0, wow_ = 0, quietFor_ = kQuietSkip + 1;
  float target_ = 0.25f * kRate, time_ = 0.0f;
  float fb_ = 0.45f, mix_ = 0.35f;
  float lp_ = 0.0f, hp_ = 0.0f, lastWet_ = 0.0f, prevWet_ = 0.0f, feedback_ = 0.0f;
  float aa1_ = 0.0f, aa2_ = 0.0f, acc_ = 0.0f;
  uint32_t sub_ = 0;   // output samples into the current tape step
  bool on_ = false;
};

}  // namespace pd
