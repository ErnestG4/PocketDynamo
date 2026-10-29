// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared pulse core for the 1-bit engine family (from the plaits-lab
// ZxPulse48k/ZxPhase48k packages). Renders superposed trains of narrow
// rectangular pulses ("pins"). Band-limiting is applied to pulse *edges* only
// (2-sample polyBLEP pair) — the spectrum is never low-passed; fold-down from
// narrow pins is the intended timbre. Each channel also exposes the raw
// aliased value so an engine can present the un-band-limited bus as its nasty
// alternate output.
#pragma once
#include "core.h"

namespace pd {
namespace pulse {

// PolyBLEP residual for a unit step at phase 0, transition width dt.
inline float polyBlep(float t, float dt) {
  if (t < dt) {
    const float x = t / dt;
    return x + x - x * x - 1.0f;
  } else if (t > 1.0f - dt) {
    const float x = (t - 1.0f) / dt;
    return x * x + x + x + 1.0f;
  }
  return 0.0f;
}

// One pin train: unipolar rectangular pulses of width `duty` (as a fraction
// of the period), DC-removed so superposition stays centered. The return value
// has polyBLEP'd edges; *naive_out is the raw aliased rectangle.
class PinChannel {
 public:
  void reset() { phase_ = 0.0f; }

  // dt: normalized frequency, caller-clamped. duty: caller-clamped to [dt, 0.45].
  inline float next(float dt, float duty, float* naive_out) {
    phase_ += dt;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    const float t = phase_;
    const float naive = t < duty ? 1.0f - duty : -duty;
    float tf = t - duty;
    if (tf < 0.0f) tf += 1.0f;
    *naive_out = naive;
    return naive + polyBlep(t, dt) - polyBlep(tf, dt);
  }

 private:
  float phase_ = 0.0f;
};

// The register-write grid: all internal modulation (arps, echo taps, duty
// animation) quantizes to this tick; external control stays continuous.
class FrameTick {
 public:
  void reset() { counter_ = 1; }
  inline bool next() {
    if (--counter_ <= 0) { counter_ = kPeriod; return true; }
    return false;
  }
  // 50 Hz, the frame rate of the original drivers (957 at Plaits' rate).
  static constexpr int32_t kPeriod = (int32_t)(kSampleRate / 50.0f + 0.5f);

 private:
  int32_t counter_ = 1;
};

}  // namespace pulse
}  // namespace pd
