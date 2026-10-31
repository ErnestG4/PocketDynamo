// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Attack/release envelope evaluated once per block and ramped linearly across
// it: one exp-style step per block instead of per sample. Attack is linear
// (clickless onsets without softening transients), release is exponential so
// tails die the way speakers and ears expect.
#pragma once
#include "core.h"

namespace pd {

class Envelope {
 public:
  void setTimes(float attackMs, float releaseMs, size_t block) {
    const float blocksPerMs = kSampleRate / (1000.0f * (float)block);
    attackStep_ = 1.0f / std::max(1.0f, attackMs * blocksPerMs);
    // Release to -80 dB over releaseMs.
    releaseMul_ = std::pow(1e-4f, 1.0f / std::max(1.0f, releaseMs * blocksPerMs));
    blocksPerMs_ = blocksPerMs;
  }

  void gateOn()  { stage_ = Stage::Attack; decayMul_ = 1.0f; }
  // A ratchet hit: duck to silence over one block, attack again, then with
  // decayMs > 0 fall to -60 dB over decayMs while still gated. decayMs = 0
  // re-attacks to a normal held note.
  void retrigger(float decayMs) {
    if (stage_ == Stage::Idle || stage_ == Stage::Release) return;
    decayMul_ = decayMs > 0.0f
        ? std::pow(1e-3f, 1.0f / std::max(1.0f, decayMs * blocksPerMs_)) : 1.0f;
    stage_ = Stage::Duck;
  }
  void gateOff() { if (stage_ != Stage::Idle) stage_ = Stage::Release; }
  void kill()    { stage_ = Stage::Idle; level_ = 0.0f; }
  bool active() const { return stage_ != Stage::Idle; }
  bool released() const { return stage_ == Stage::Release; }
  float level() const { return level_; }

  // Advance one block; writes start/end levels for the caller's ramp.
  void advance(float* start, float* end) {
    *start = level_;
    switch (stage_) {
      case Stage::Duck:
        level_ = 0.0f;
        stage_ = Stage::Attack;
        break;
      case Stage::Attack:
        level_ += attackStep_;
        if (level_ >= 1.0f) { level_ = 1.0f; stage_ = decayMul_ < 1.0f ? Stage::Decay : Stage::Hold; }
        break;
      case Stage::Decay:
        level_ = level_ < 1e-4f ? 0.0f : level_ * decayMul_;
        break;
      case Stage::Release:
        level_ *= releaseMul_;
        if (level_ < 1e-4f) { level_ = 0.0f; stage_ = Stage::Idle; }
        break;
      default:
        break;
    }
    *end = level_;
  }

 private:
  enum class Stage : uint8_t { Idle, Duck, Attack, Hold, Decay, Release };
  Stage stage_ = Stage::Idle;
  float level_ = 0.0f;
  float attackStep_ = 1.0f;
  float releaseMul_ = 0.9f;
  float decayMul_ = 1.0f;
  float blocksPerMs_ = 1.0f;
};

}  // namespace pd
