// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ARS grain scheduling: inter-onset gaps drawn from a rigidity ladder of
// spacing statistics, replacing the stock scheduler's two points (metronome
// and dice) with the whole space between. Zones: 0 rigid lattice, 1 jittered
// lattice (hyperuniform), 2 quasiperiodic three-distance, 3 GUE-like,
// 4 GOE-like, 5 Poisson, 6 clustered. Freerun v0 — deterministic replayable
// realizations arrive with the panel work.
//
// Ported unchanged from Lenticular (clouds/dsp/ars_scheduler.h); only the
// namespace moved. The 65535-sample gap cap is ~1.4 s at 48 kHz here.

#pragma once
#include <cstdint>

#include "ars_luts.h"

namespace pd {

class ArsGapGenerator {
 public:
  void Init(uint32_t seed) {
    state_ = seed ? seed : 1u;
    for (int i = 0; i < kLoopGaps; ++i) {
      uniforms_[i] = static_cast<uint8_t>(RawUniform() * 255.0f);
    }
    ring_index_ = 0;
    loop_length_ = kLoopGaps;
    fray_ = 0.0f;
    lattice_u_ = 0.5f;
    rotation_phase_ = 0.0f;
    voice_u_ = 0.5f;
    round_robin_ = 0;
  }

  // Performable realizations: TRIG replays the stored gap sequence from the
  // top — the same cloud every strike. loop in [0,1]: CCW tightens the ring
  // (8..64 gaps at noon); CW frays it — fresh ensemble draws with rising
  // probability, full freerun at the end.
  void Replay() {
    ring_index_ = 0;
    lattice_u_ = 0.5f;
    rotation_phase_ = 0.0f;
    round_robin_ = 0;
  }

  void set_loop(float loop) {
    if (loop < 0.5f) {
      loop_length_ = 8 + static_cast<int8_t>(loop * 2.0f * 24.0f);
      fray_ = 0.0f;
    } else {
      loop_length_ = kLoopGaps;
      fray_ = (loop - 0.5f) * 2.0f;
    }
  }

  // Restart the chip-arp voice cycle at the chord root — called on V/Oct
  // note changes so sequenced riffs arpeggiate phrase-locked, the way the
  // beeper drivers restarted their arps per note.
  void ResetArp() { round_robin_ = 3; }

  void Reseed() {
    for (int i = 0; i < kLoopGaps; ++i) {
      uniforms_[i] = static_cast<uint8_t>(RawUniform() * 255.0f);
    }
    Replay();
  }

  // Voice quantile of the last gap, uniform on [0,1): the exact percentile
  // for LUT zones (long gap = high voice — the gap melody), round-robin in
  // the rigid zone (a strict arpeggio), distance-indexed for three-distance
  // (each of the <=3 gap values owns a voice — the motif).
  float last_voice_u() const { return voice_u_; }

  // Next inter-onset interval in samples; mean is the target mean spacing.
  float NextGap(int zone, float character, float mean) {
    float gap;
    if (zone <= 0) {
      gap = 1.0f;  // rigid lattice
      round_robin_ = static_cast<int8_t>((round_robin_ + 1) & 3);
      voice_u_ = 0.125f + 0.25f * static_cast<float>(round_robin_);
    } else if (zone == 1) {
      // Jittered lattice: bounded displacement, truly hyperuniform. Gaps
      // chain successive uniforms so displacement stays bounded.
      const float u = Uniform();
      gap = 1.0f + 0.9f * character * (u - lattice_u_);
      lattice_u_ = u;
      voice_u_ = u;
    } else if (zone == 2) {
      // Three-distance return times of the rotation by alpha; character
      // walks alpha along the Stern-Brocot path from the 1/2 lock to 1/phi.
      const float alpha = BrocotAlpha(character);
      int k = 0;
      do {
        rotation_phase_ += alpha;
        if (rotation_phase_ >= 1.0f) {
          rotation_phase_ -= 1.0f;
        }
        ++k;
      } while (rotation_phase_ >= alpha && k < 64);
      gap = static_cast<float>(k) * alpha;
      voice_u_ = 0.125f + 0.25f * static_cast<float>((k - 1) & 3);
    } else {
      const float u = Uniform();
      gap = GammaGap(u, ZoneK(zone, character));
      voice_u_ = u;
    }
    float samples = gap * mean;
    // Cap the wait: a near-zero density means a huge mean spacing, and an
    // unbounded (or inf) gap would poison the caller's accumulator.
    if (!(samples >= 1.0f)) samples = 1.0f;      // also catches NaN
    if (samples > 65535.0f) samples = 65535.0f;  // ~2 s at 32 kHz
    return samples;
  }

 private:
  // 32: the F405's last spare bytes are the constraint; an 8..32-gap
  // realization still loops as a graspable phrase at grain rates.
  static const int kLoopGaps = 32;

  inline float RawUniform() {
    uint32_t x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state_ = x;
    return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
  }

  // Realization draw: the stored ring (deterministic, replayable), frayed
  // into fresh ensemble draws with probability fray_.
  inline float Uniform() {
    if (fray_ > 0.0f && RawUniform() < fray_) {
      return RawUniform();
    }
    // 8-bit quantiles: exactly the 256-entry LUT resolution, 1/4 the RAM.
    const float u = static_cast<float>(uniforms_[ring_index_]) * (1.0f / 255.0f);
    ring_index_ = static_cast<int8_t>((ring_index_ + 1) % loop_length_);
    return u;
  }

  static inline float InterpTable(const float* table, float u) {
    float x = u * 255.0f;
    if (x < 0.0f) x = 0.0f;
    if (x > 255.0f) x = 255.0f;
    const int i = static_cast<int>(x);
    const float f = x - static_cast<float>(i);
    const int j = i < 255 ? i + 1 : 255;
    return table[i] + (table[j] - table[i]) * f;
  }

  // Quantile interpolation between k tables — a Wasserstein-2 geodesic
  // between gap distributions: the character knob IS the morph.
  static inline float GammaGap(float u, float k) {
    if (k < 0.5f) k = 0.5f;
    if (k > 3.0f) k = 3.0f;
    int t;
    float blend;
    if (k <= 1.0f) {
      t = 0;
      blend = (k - 0.5f) * 2.0f;
    } else if (k <= 2.0f) {
      t = 1;
      blend = k - 1.0f;
    } else {
      t = 2;
      blend = k - 2.0f;
    }
    const float a = InterpTable(ars::kGammaQuantiles[t], u);
    const float b = InterpTable(ars::kGammaQuantiles[t + 1], u);
    return a + (b - a) * blend;
  }

  static inline float ZoneK(int zone, float character) {
    switch (zone) {
      case 3: return 2.2f + 0.8f * character;   // GUE-like
      case 4: return 1.6f + 0.8f * character;   // GOE-like
      case 5: return 1.0f;                      // Poisson
      default: return 1.0f - 0.55f * character; // clustered
    }
  }

  static inline float BrocotAlpha(float character) {
    float x = character * static_cast<float>(ars::kBrocotPathSize - 1);
    const int i = static_cast<int>(x);
    const int j = i < ars::kBrocotPathSize - 1 ? i + 1 : i;
    const float f = x - static_cast<float>(i);
    return ars::kBrocotPath[i] + (ars::kBrocotPath[j] - ars::kBrocotPath[i]) * f;
  }

  uint32_t state_;
  uint8_t uniforms_[kLoopGaps];
  int8_t ring_index_;
  int8_t loop_length_;
  int8_t round_robin_;
  float fray_;
  float lattice_u_;
  float rotation_phase_;
  float voice_u_;
};

}  // namespace pd

