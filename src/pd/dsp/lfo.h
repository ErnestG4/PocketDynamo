// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The instrument's one LFO, advanced once per block (1.5 kHz at 32-sample
// blocks: smooth enough for anything up to its 20 Hz top rate). Free-running
// in Hz or synced to the tempo in beats per cycle. DELAY fades its depth in
// after a note starts from silence, the way vibrato comes in late on a held
// note. The phase never resets, so chords and the looper don't restart it.
// Pure — host-testable (synth/instrument_test.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

#include "core.h"
#include "rng.h"
#include "tables.h"

namespace pd {

enum class LfoShape : uint8_t { Sine, Tri, Saw, Square, Random, Count };

inline const char* lfoShapeName(LfoShape s) {
  switch (s) {
    case LfoShape::Tri: return "TRI";
    case LfoShape::Saw: return "SAW";
    case LfoShape::Square: return "SQR";
    case LfoShape::Random: return "S&H";
    default: return "SINE";
  }
}

// What the LFO drives. The four controls, pitch, the four FX bars and the
// echo's feedback and mix.
enum class LfoDest : uint8_t { None, Harmonics, Timbre, Morph, Macro, Pitch, Tone, Grit, Shape, Space, EchoFb, EchoMix, Count };

class Lfo {
 public:
  explicit Lfo(size_t block) : block_(block) { update(); }

  void setRate(float hz) { hz_ = clamp(hz, 0.01f, 50.0f); update(); }
  void setSync(float beats) { beats_ = beats > 0.0f ? clamp(beats, 1.0f / 64.0f, 64.0f) : 0.0f; update(); }
  void setTempo(float bpm) { bpm_ = clamp(bpm, 20.0f, 400.0f); update(); }
  void setDepth(float d) { depth_ = clamp(d, 0.0f, 1.0f); }
  void setShape(int s) { shape_ = (LfoShape)clamp(s, 0, (int)LfoShape::Count - 1); }
  void setDelay(float ms) {
    const float blocks = ms * kSampleRate / (1000.0f * (float)block_);
    delayStep_ = blocks > 1.0f ? 1.0f / blocks : 1.0f;
  }
  // A note started from silence: fade the depth in again.
  void restartDelay() { if (delayStep_ < 1.0f) ramp_ = 0.0f; }
  float depth() const { return depth_; }

  // One block on: the LFO's value, -1..1, scaled by depth and the delay.
  float advance() {
    const uint32_t prev = phase_;
    phase_ += inc_;
    if (phase_ < prev) held_ = (float)(rng::hash32(++draws_) >> 8) * (2.0f / 16777216.0f) - 1.0f;
    ramp_ = ramp_ + delayStep_ >= 1.0f ? 1.0f : ramp_ + delayStep_;
    const float u = (float)phase_ * kPhaseToFloat;
    float v;
    switch (shape_) {
      case LfoShape::Tri:    v = u < 0.5f ? 4.0f * u - 1.0f : 3.0f - 4.0f * u; break;
      case LfoShape::Saw:    v = 2.0f * u - 1.0f; break;
      case LfoShape::Square: v = u < 0.5f ? 1.0f : -1.0f; break;
      case LfoShape::Random: v = held_; break;
      default:               v = sinePhase(phase_); break;
    }
    return v * depth_ * ramp_;
  }

 private:
  void update() {
    const float hz = beats_ > 0.0f ? bpm_ / 60.0f / beats_ : hz_;
    inc_ = phaseIncrement(hz * (float)block_, kSampleRate);
  }

  size_t block_;
  float hz_ = 1.0f, beats_ = 0.0f, bpm_ = 120.0f;
  float depth_ = 0.0f;
  LfoShape shape_ = LfoShape::Sine;
  uint32_t phase_ = 0, inc_ = 0, draws_ = 0;
  float held_ = 0.0f;
  float ramp_ = 1.0f, delayStep_ = 1.0f;
};

}  // namespace pd
