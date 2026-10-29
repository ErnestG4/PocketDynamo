// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ZxPhase — ZX Spectrum beeper-style boolean interference synthesis (Shiru
// Phaser1 / utz PhaserX family), ported from the plaits-lab ZxPhase48k
// package. Two square oscillators run on an internal 1-bit bus at 4x the
// sample rate and are combined bitwise (XOR = boolean ring mod, OR/AND =
// Squeeker-style decouplings), then decimated by a plain 4-sample box average
// so partial fold-down survives — that is the timbre, not a defect.
#pragma once
#include "../dsp/pulse_core.h"

namespace pd {

class ZxPhaseEngine : public Engine {
 public:
  ZxPhaseEngine() { reset(); }

  void reset() override {
    phase1_ = 0;
    phase2_ = 0x40000000u;  // quarter-cycle offset: strikes never start locked
    ghost_phase_ = 0;
    lfsr_ = 0xACE1u;
    tick_.reset();
    duty2_index_ = -1;      // resynced to the base duty on next render
    sweep_direction_ = 1;
    sweep_count_ = 0;
  }

  void render(const EngineParams& p, float* out, float* aux, size_t size) override {
    const bool struck = (p.trigger & kTriggerRising) != 0;
    if (struck) reset();

    // Mix-method zones with hysteresis.
    const float raw_zone = p.harmonics * 3.0f;
    const int candidate = clamp((int)raw_zone, 0, 2);
    if (candidate != zone_ &&
        (raw_zone > (float)zone_ + 1.08f || raw_zone < (float)zone_ - 0.08f))
      zone_ = candidate;
    const float zone_position = clamp(raw_zone - (float)zone_, 0.0f, 1.0f);

    // 1-bit flanger: XOR the bus with itself N internal samples ago. Position
    // within each mix zone sweeps the comb from off, through slow metallic
    // (~375 Hz), up to supersonic shimmer.
    const bool feedback_on = zone_position > 0.15f;
    uint32_t feedback_delay = 2;
    if (feedback_on) {
      const float q = (zone_position - 0.15f) * (1.0f / 0.85f);
      feedback_delay = (uint32_t)(512.0f * semitonesToRatio(-72.0f * q));  // 512 .. 8
      if (feedback_delay < 2) feedback_delay = 2;
    }

    // TIMBRE is PWM motion on osc2: CCW static fat squares, then the
    // SID-style tick-clocked sweep engages, deeper and faster clockwise.
    // Static duty is barely audible under boolean mixing; duty MOTION is
    // what the ear tracks (hardware pass, 2026-08-14).
    const float pwm = p.timbre;
    const bool sweep_on = pwm > 0.08f;
    const float ps = sweep_on ? clamp((pwm - 0.08f) * (1.0f / 0.92f), 0.0f, 1.0f) : 0.0f;
    const int sweep_min = 14 - (int)(ps * 12.99f);   // 14 .. 2
    const int sweep_period = 8 - (int)(ps * 7.0f);   // 8 .. 1 ticks
    const int base_index = 14;  // both oscillators rest at fat 4-bit squares
    const uint32_t duty1 = (uint32_t)((float)base_index * (1.0f / 32.0f) * kTwo32);
    if (duty2_index_ < 0 || !sweep_on) duty2_index_ = base_index;

    // MORPH: fine beating at unison, then a hysteresis-quantized interval
    // ladder (each step keeps a slow beat so the interference stays alive),
    // then LFSR noise.
    const float morph = p.morph;
    if (!noise_mode_ && morph > 0.92f) noise_mode_ = true;
    else if (noise_mode_ && morph < 0.88f) noise_mode_ = false;
    float detune_semitones;
    if (morph < 0.25f) {
      detune_semitones = morph * (1.0f / 0.25f) * 0.30f;
    } else {
      static constexpr float kIntervals[6] = {0.0f, 5.0f, 7.0f, 12.0f, 19.0f, 24.0f};
      const float x = clamp((morph - 0.25f) * (1.0f / 0.65f), 0.0f, 1.0f);
      const float raw_step = x * 5.99f;
      const int step = clamp((int)raw_step, 0, 5);
      if (step != interval_ &&
          (raw_step > (float)interval_ + 1.08f || raw_step < (float)interval_ - 0.08f))
        interval_ = step;
      detune_semitones = kIntervals[interval_] + 0.06f;
    }

    // macro: CCW arms the TRIG kick-slide (beeper click-drums: a strike slams
    // pitch up to +24 semitones, falling back in ~15 ms). Noon neutral. CW
    // adds the GHOST parasite carrier.
    const float macro = p.macro;
    const float kick_depth = macro < 0.5f ? (0.5f - macro) * 2.0f * 24.0f : 0.0f;
    if (struck && kick_depth > 0.5f) kick_ = kick_depth;

    // Two octaves below the played pitch: XOR interference reads high.
    // Pitch is read per block with no smoothing, on purpose.
    float dt1 = clamp(noteToDt(p.note - 24.0f + kick_), 0.0f, 0.2f);
    kick_ *= std::pow(kKickDecay, (float)size * (1.0f / kPlaitsBlock));
    if (kick_ < 0.05f) kick_ = 0.0f;
    float dt2 = dt1 * semitonesToRatio(detune_semitones);
    if (noise_mode_) dt2 *= kNoiseClockRatio;
    dt2 = clamp(dt2, 0.0f, 0.24f);
    const uint32_t increment1 = (uint32_t)(dt1 * kTwo30);
    const uint32_t increment2 = (uint32_t)(dt2 * kTwo30);

    const uint32_t ghost_duty = macro > 0.5f
        ? (uint32_t)((macro * 2.0f - 1.0f) * 0.35f * kTwo32)
        : 0u;

    for (size_t i = 0; i < size; ++i) {
      if (tick_.next() && sweep_on) {
        ++sweep_count_;
        if ((sweep_count_ % (uint32_t)sweep_period) == 0) {
          duty2_index_ += sweep_direction_;
          if (duty2_index_ >= 14) { duty2_index_ = 14; sweep_direction_ = -1; }
          else if (duty2_index_ <= sweep_min) { duty2_index_ = sweep_min; sweep_direction_ = 1; }
        }
      }
      const uint32_t duty2 = (uint32_t)((float)duty2_index_ * (1.0f / 32.0f) * kTwo32);
      int acc = 0, acc2 = 0;
      for (int k = 0; k < 4; ++k) {
        phase1_ += increment1;
        const uint32_t previous2 = phase2_;
        phase2_ += increment2;
        if (phase2_ < previous2) {
          const uint16_t fb = (lfsr_ ^ (lfsr_ >> 1)) & 1u;
          lfsr_ = (uint16_t)((lfsr_ >> 1) | (fb << 14));
        }
        const int b1 = phase1_ < duty1 ? 1 : 0;
        const int b2 = noise_mode_ ? (int)(lfsr_ & 1u) : (phase2_ < duty2 ? 1 : 0);
        int mixed = zone_ == 0 ? (b1 ^ b2) : zone_ == 1 ? (b1 | b2) : (b1 & b2);
        if (feedback_on) {
          const uint32_t tap = (feedback_pos_ - feedback_delay) & 511u;
          mixed ^= (int)((feedback_bits_[tap >> 5] >> (tap & 31u)) & 1u);
        }
        const uint32_t wp = feedback_pos_ & 511u;
        feedback_bits_[wp >> 5] =
            (feedback_bits_[wp >> 5] & ~(1u << (wp & 31u))) | ((uint32_t)mixed << (wp & 31u));
        ++feedback_pos_;
        if (ghost_duty) {
          ghost_phase_ += kGhostIncrement;
          mixed ^= ghost_phase_ < ghost_duty ? 1 : 0;
        }
        acc += mixed;
        acc2 += b2;
      }
      const float v = (float)acc * 0.5f - 1.0f;
      const float v2 = (float)acc2 * 0.5f - 1.0f;
      dc_main_ += 0.002f * (v - dc_main_);
      dc_aux_ += 0.002f * (v2 - dc_aux_);
      out[i] = 0.6f * (v - dc_main_);
      aux[i] = 0.6f * (v2 - dc_aux_);
    }
  }

 private:
  static constexpr float kTwo32 = 4294967296.0f;
  static constexpr float kTwo30 = 1073741824.0f;  // one internal (4x) step per sample
  // Supersonic GHOST carrier, ~26.5 kHz at the internal 4x rate: audible only
  // as the HF sheen it folds down, authentic to the parasite-tone trick.
  static constexpr uint32_t kGhostIncrement =
      (uint32_t)(26500.0 / (4.0 * (double)kSampleRate) * 4294967296.0);
  // LFSR clocked 32x above the played pitch so it reads as pitched noise.
  static constexpr float kNoiseClockRatio = 32.0f;
  // ~15 ms kick fall: 0.9835 per Plaits block of 12 samples, scaled in render
  // to whatever block size we're called with.
  static constexpr float kKickDecay = 0.9835f;
  static constexpr float kPlaitsBlock = 12.0f;

  pulse::FrameTick tick_;
  uint32_t phase1_ = 0, phase2_ = 0, ghost_phase_ = 0;
  uint16_t lfsr_ = 0xACE1u;
  int zone_ = 0;            // hysteretic mix-method zone: 0 XOR, 1 OR, 2 AND
  int interval_ = 0;        // hysteretic MORPH interval-ladder step
  bool noise_mode_ = false; // hysteretic MORPH extreme: osc2 becomes an LFSR
  int duty2_index_ = -1;
  int sweep_direction_ = 1;
  uint32_t sweep_count_ = 0;
  float dc_main_ = 0.0f, dc_aux_ = 0.0f;
  uint32_t feedback_bits_[16] = {};
  uint32_t feedback_pos_ = 0;
  float kick_ = 0.0f;       // TRIG kick-slide, semitones above played pitch
};

}  // namespace pd
