// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Attack/decay/sustain/release envelope evaluated once per block and ramped
// linearly across it: one exp-style step per block instead of per sample.
// Attack is linear (clickless onsets without softening transients); decay
// and release are exponential, so tails die the way speakers and ears
// expect. An engine's own envelope is the A/R case (sustain 1, no decay);
// the ENV page's ADSR fills in the rest. The libm lives in shape(), which
// callers run when a setting changes, never per note or per block.
#pragma once
#include "core.h"

namespace pd {

// The per-block coefficients of an envelope.
struct EnvShape {
  float attackStep = 1.0f;
  float decayMul = 1.0f;     // 1 = no decay: hold at full level
  float sustain = 1.0f;
  float releaseMul = 0.9f;
};

class Envelope {
 public:
  // Attack over attackMs, decay to the sustain level over decayMs (0 = none),
  // release to -80 dB over releaseMs.
  static EnvShape shape(float attackMs, float decayMs, float sustain, float releaseMs, size_t block) {
    const float blocksPerMs = kSampleRate / (1000.0f * (float)block);
    EnvShape s;
    s.attackStep = 1.0f / std::max(1.0f, attackMs * blocksPerMs);
    s.sustain = clamp(sustain, 0.0f, 1.0f);
    // Kept even at sustain 1, so a held note can follow a live change of
    // sustain level without a jump.
    s.decayMul = decayMs > 0.0f
        ? std::pow(1e-4f, 1.0f / std::max(1.0f, decayMs * blocksPerMs)) : 1.0f;
    s.releaseMul = std::pow(1e-4f, 1.0f / std::max(1.0f, releaseMs * blocksPerMs));
    return s;
  }

  void setTimes(float attackMs, float releaseMs, size_t block) {
    setShape(shape(attackMs, 0.0f, 1.0f, releaseMs, block), block);
  }

  // Takes effect at once, also on a sounding note: a held note heads for the
  // new sustain level, a released one fades at the new rate.
  void setShape(const EnvShape& s, size_t block) {
    attackStep_ = s.attackStep;
    releaseMul_ = s.releaseMul;
    holdMul_ = s.decayMul;
    sustain_ = s.sustain;
    blocksPerMs_ = kSampleRate / (1000.0f * (float)block);
    if (!ratchet_) { decayMul_ = holdMul_; target_ = sustain_; }
    if (stage_ == Stage::Hold && level_ != target_ && decayMul_ < 1.0f) stage_ = Stage::Decay;
  }

  void gateOn() { stage_ = Stage::Attack; ratchet_ = false; decayMul_ = holdMul_; target_ = sustain_; }
  // A ratchet hit: duck to silence over one block, attack again, then with
  // decayMs > 0 fall to -60 dB over decayMs while still gated. decayMs = 0
  // re-attacks to a normal held note.
  void retrigger(float decayMs) {
    if (stage_ == Stage::Idle || stage_ == Stage::Release) return;
    ratchet_ = decayMs > 0.0f;
    if (ratchet_) {
      decayMul_ = std::pow(1e-3f, 1.0f / std::max(1.0f, decayMs * blocksPerMs_));
      target_ = 0.0f;
    } else {
      decayMul_ = holdMul_;
      target_ = sustain_;
    }
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
      case Stage::Decay: {
        const float d = level_ - target_;
        level_ = d > -1e-4f && d < 1e-4f ? target_ : target_ + d * decayMul_;
        break;
      }
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
  bool ratchet_ = false;     // decaying to silence for a ratchet hit
  float level_ = 0.0f;
  float attackStep_ = 1.0f;
  float releaseMul_ = 0.9f;
  float holdMul_ = 1.0f;     // the shape's decay toward sustain_
  float sustain_ = 1.0f;
  float decayMul_ = 1.0f;    // the decay running now (the shape's, or a ratchet's)
  float target_ = 1.0f;
  float blocksPerMs_ = 1.0f;
};

}  // namespace pd
