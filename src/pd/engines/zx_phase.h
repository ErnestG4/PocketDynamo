// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ZxPhase — ZX Spectrum beeper-style boolean interference synthesis (Shiru
// Phaser1 / utz PhaserX family), ported from the plaits-lab ZxPhase48k
// package. Two square oscillators run on an internal 1-bit bus at 4x the
// sample rate and are combined bitwise (XOR = boolean ring mod, OR/AND =
// Squeeker-style decouplings), then decimated by a plain 4-sample box average
// so partial fold-down survives — that is the timbre, not a defect.
//
// Brocot treatment (2026-10-29): bitwise mixing only repeats when every ratio
// in the loop is simple, so nothing here is free-running any more. Osc 2 sits
// on exact p/q ratios of osc 1 (integer increments), its beat is a rate in Hz
// rather than a pitch offset, and the 1-bit flanger's delay is a 1/k fraction
// of osc 1's period, read from a phase-indexed bit table. Before this the
// flanger delay was a fixed sample count, and wherever the note's period
// didn't divide by it the recursive XOR never repeated: FM-style noise that
// moved around the keyboard (host/zx_phase_map.cpp draws the plane).
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

  PD_HOT void render(const EngineParams& p, float* out, float* aux, size_t size) override {
    const bool struck = (p.trigger & kTriggerRising) != 0;
    if (struck) reset();

    // Mix-method zones with hysteresis.
    const float raw_zone = p.harmonics * 3.0f;
    const int candidate = clamp((int)raw_zone, 0, 2);
    if (candidate != zone_ &&
        (raw_zone > (float)zone_ + 1.08f || raw_zone < (float)zone_ - 0.08f))
      zone_ = candidate;
    const float zone_position = clamp(raw_zone - (float)zone_, 0.0f, 1.0f);

    // 1-bit flanger: XOR the bus with itself 1/k of an osc-1 period ago.
    // Position within each mix zone steps k up the harmonic series (with
    // hysteresis), so the comb climbs from a hollow octave to a high
    // shimmer, and stays pitched on every note.
    const bool feedback_on = zone_position > 0.15f;
    if (feedback_on) {
      const float raw_comb = (zone_position - 0.15f) * (1.0f / 0.85f) * ((float)kCombs - 0.01f);
      const int c = clamp((int)raw_comb, 0, kCombs - 1);
      if (comb_ < 0 || (c != comb_ &&
          (raw_comb > (float)comb_ + 1.08f || raw_comb < (float)comb_ - 0.08f)))
        comb_ = c;
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
    if (duty2_index_ < 0 || !sweep_on) duty2_index_ = base_index;

    // MORPH: beating at unison (0 → 3 Hz), then a hysteresis-quantized
    // ladder of just ratios, each with a slow beat so the interference stays
    // alive, then LFSR noise. Beats are rates of the whole pattern in Hz, so
    // a position sounds the same speed on every note.
    const float morph = p.morph;
    if (!noise_mode_ && morph > 0.92f) noise_mode_ = true;
    else if (noise_mode_ && morph < 0.88f) noise_mode_ = false;
    float beat_hz;
    if (morph < 0.25f) {
      const float x = morph * (1.0f / 0.25f);
      beat_hz = kUnisonBeatHz * x * x;
      interval_ = 0;
    } else {
      const float x = clamp((morph - 0.25f) * (1.0f / 0.65f), 0.0f, 1.0f);
      const float raw_step = x * ((float)kRatios - 0.01f);
      const int step = clamp((int)raw_step, 0, kRatios - 1);
      if (step != interval_ &&
          (raw_step > (float)interval_ + 1.08f || raw_step < (float)interval_ - 0.08f))
        interval_ = step;
      beat_hz = kLadderBeatHz;
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
    const uint32_t increment1 = (uint32_t)(dt1 * kTwo30);
    // Osc 2 = osc 1 x p/q exactly, plus the beat spread over the q cycles
    // the pattern takes to repeat (capped at 3 % of osc 1 on the lowest
    // notes, where a few Hz would be a detune rather than a beat).
    const Ratio r = kRatioLadder[interval_];
    beat_hz = std::min(beat_hz, 0.03f * dt1 * kSampleRate);
    uint64_t inc2 = (uint64_t)increment1 * r.p / r.q +
                    (uint64_t)(beat_hz / (float)r.q * (kTwo30 / kSampleRate));
    if (noise_mode_) inc2 *= (uint64_t)kNoiseClockRatio;
    const uint32_t increment2 = (uint32_t)std::min<uint64_t>(inc2, (uint64_t)(0.24f * kTwo30));

    // The flanger's tap, 1/k of osc 1's period back, read straight off the
    // phase accumulators (osc 2 has moved its own share of that time), so
    // the delay is exact at any pitch and needs no buffer. In noise mode the
    // tap counts osc 2's wraps back into the LFSR's output history.
    const uint32_t tap1 = (uint32_t)(4294967296.0 / kCombHarmonic[comb_ < 0 ? 0 : comb_]);
    const uint64_t tap2_long = (uint64_t)tap1 * increment2 / (increment1 ? increment1 : 1u);
    const uint32_t tap2 = (uint32_t)tap2_long;
    const uint32_t tap2_wraps = (uint32_t)(tap2_long >> 32);

    const uint32_t ghost_duty = macro > 0.5f
        ? (uint32_t)((macro * 2.0f - 1.0f) * 0.35f * kTwo32)
        : 0u;

    // The 1-bit loop runs on locals (members through `this` would be loaded
    // and stored every step), mixes through a truth table instead of
    // branching on the zone, and keeps duty integer: index/32 of a cycle is
    // exactly index << 27.
    const uint32_t duty1 = zone_ == 2 ? kAndDuty1 : kDuty1;
    const uint32_t table = kMixTable[zone_];   // bit (b1 << 1 | b2) = mix(b1, b2)
    const bool noise = noise_mode_, ghost = ghost_duty != 0;
    // Most playing has neither the LFSR nor the ghost in the mix: a lean
    // kernel then, with few enough live values to stay in registers.
    if (!noise && !ghost) {
      if (feedback_on) fastLoop<true>(out, aux, size, sweep_on, sweep_min, sweep_period, increment1, increment2, duty1, table, tap1, tap2);
      else fastLoop<false>(out, aux, size, sweep_on, sweep_min, sweep_period, increment1, increment2, duty1, table, tap1, tap2);
      return;
    }
    uint32_t ph1 = phase1_, ph2 = phase2_, gph = ghost_phase_;
    uint32_t lfsr = lfsr_;
    uint64_t history = lfsr_history_;
    float dcMain = dc_main_, dcAux = dc_aux_;
    for (size_t i = 0; i < size; ++i) {
      sweepTick(sweep_on, sweep_min, sweep_period);
      const uint32_t duty2 = (uint32_t)duty2_index_ << 27;
      int acc = 0, acc2 = 0;
      for (int k = 0; k < 4; ++k) {
        ph1 += increment1;
        const uint32_t previous2 = ph2;
        ph2 += increment2;
        if (ph2 < previous2) {
          history = (history << 1) | (lfsr & 1u);
          const uint32_t fb = (lfsr ^ (lfsr >> 1)) & 1u;
          lfsr = (lfsr >> 1) | (fb << 14);
        }
        const uint32_t b1 = ph1 < duty1;
        const uint32_t b2 = noise ? (lfsr & 1u) : (uint32_t)(ph2 < duty2);
        uint32_t mixed = (table >> (b1 << 1 | b2)) & 1u;
        if (feedback_on) {
          const uint32_t d1 = ph1 - tap1 < duty1;
          uint32_t d2;
          if (noise) {
            const uint32_t back = tap2_wraps + (ph2 < tap2 ? 1u : 0u);
            d2 = back == 0 ? (lfsr & 1u) : (uint32_t)(history >> (back < 64 ? back - 1 : 63)) & 1u;
          } else {
            d2 = ph2 - tap2 < duty2;
          }
          mixed ^= (table >> (d1 << 1 | d2)) & 1u;
        }
        if (ghost) {
          gph += kGhostIncrement;
          mixed ^= gph < ghost_duty;
        }
        acc += (int)mixed;
        acc2 += (int)b2;
      }
      const float v = (float)acc * 0.5f - 1.0f;
      const float v2 = (float)acc2 * 0.5f - 1.0f;
      dcMain += 0.002f * (v - dcMain);
      dcAux += 0.002f * (v2 - dcAux);
      out[i] = 0.6f * (v - dcMain);
      aux[i] = 0.6f * (v2 - dcAux);
    }
    phase1_ = ph1; phase2_ = ph2; ghost_phase_ = gph;
    lfsr_ = (uint16_t)lfsr;
    lfsr_history_ = history;
    dc_main_ = dcMain; dc_aux_ = dcAux;
  }

 private:
  // The PWM sweep's tick, once per output sample.
  inline void sweepTick(bool sweep_on, int sweep_min, int sweep_period) {
    if (tick_.next() && sweep_on) {
      ++sweep_count_;
      if ((sweep_count_ % (uint32_t)sweep_period) == 0) {
        duty2_index_ += sweep_direction_;
        if (duty2_index_ >= 14) { duty2_index_ = 14; sweep_direction_ = -1; }
        else if (duty2_index_ <= sweep_min) { duty2_index_ = sweep_min; sweep_direction_ = 1; }
      }
    }
  }

  // The lean kernel: no LFSR in the mix and no ghost. The LFSR still clocks
  // on osc 2's wraps (it must be where it would be when noise mode comes
  // in), so the wraps are counted and it catches up once per block.
  // Bit-identical to the generic loop.
  template <bool kFeedback>
  __attribute__((always_inline)) inline void fastLoop(float* out, float* aux, size_t size, bool sweep_on,
      int sweep_min, int sweep_period, uint32_t inc1, uint32_t inc2, uint32_t duty1, uint32_t table,
      uint32_t tap1, uint32_t tap2) {
    uint32_t ph1 = phase1_, ph2 = phase2_, wraps = 0;
    float dcMain = dc_main_, dcAux = dc_aux_;
    for (size_t i = 0; i < size; ++i) {
      sweepTick(sweep_on, sweep_min, sweep_period);
      const uint32_t duty2 = (uint32_t)duty2_index_ << 27;
      uint32_t acc = 0, acc2 = 0;
#define PD_ZX_STEP                                                         \
      {                                                                    \
        ph1 += inc1;                                                       \
        const uint32_t prev = ph2;                                         \
        ph2 += inc2;                                                       \
        wraps += ph2 < prev;                                               \
        const uint32_t b1 = ph1 < duty1, b2 = ph2 < duty2;                 \
        uint32_t m = table >> (b1 << 1 | b2);                              \
        if (kFeedback) {                                                   \
          const uint32_t d1 = ph1 - tap1 < duty1, d2 = ph2 - tap2 < duty2; \
          m ^= table >> (d1 << 1 | d2);                                    \
        }                                                                  \
        acc += m & 1u;                                                     \
        acc2 += b2;                                                        \
      }
      PD_ZX_STEP PD_ZX_STEP PD_ZX_STEP PD_ZX_STEP
#undef PD_ZX_STEP
      const float v = (float)(int)acc * 0.5f - 1.0f;
      const float v2 = (float)(int)acc2 * 0.5f - 1.0f;
      dcMain += 0.002f * (v - dcMain);
      dcAux += 0.002f * (v2 - dcAux);
      out[i] = 0.6f * (v - dcMain);
      aux[i] = 0.6f * (v2 - dcAux);
    }
    phase1_ = ph1; phase2_ = ph2;
    dc_main_ = dcMain; dc_aux_ = dcAux;
    uint32_t lfsr = lfsr_;
    uint64_t history = lfsr_history_;
    for (uint32_t w = 0; w < wraps; ++w) {
      history = (history << 1) | (lfsr & 1u);
      lfsr = (lfsr >> 1) | (((lfsr ^ (lfsr >> 1)) & 1u) << 14);
    }
    lfsr_ = (uint16_t)lfsr;
    lfsr_history_ = history;
  }

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

  // Mix truth tables, indexed by (b1 << 1 | b2): XOR 0110, OR 1110, AND 1000.
  static constexpr uint32_t kMixTable[3] = {0x6u, 0xEu, 0x8u};

  struct Ratio { uint32_t p, q; };
  // MORPH's ladder: unison, 4/3, 3/2, 2, 3, 4 (the old 0/5/7/12/19/24
  // semitones, made just), short Stern-Brocot paths all.
  static constexpr int kRatios = 6;
  static constexpr Ratio kRatioLadder[kRatios] = {{1, 1}, {4, 3}, {3, 2}, {2, 1}, {3, 1}, {4, 1}};
  static constexpr float kUnisonBeatHz = 3.0f;
  static constexpr float kLadderBeatHz = 0.4f;
  // Flanger comb positions: the delay is 1/k of osc 1's period.
  static constexpr int kCombs = 11;
  static constexpr uint8_t kCombHarmonic[kCombs] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32};
  static constexpr uint32_t kDuty1 = (uint32_t)(14.0 / 32.0 * 4294967296.0);
  // AND needs overlap: a wider osc 1 keeps unison drift from gating to
  // silence (14/32 + 14/32 < 1 left an eighth of every beat dead).
  static constexpr uint32_t kAndDuty1 = (uint32_t)(20.0 / 32.0 * 4294967296.0);

  pulse::FrameTick tick_;
  uint32_t phase1_ = 0, phase2_ = 0, ghost_phase_ = 0;
  uint16_t lfsr_ = 0xACE1u;
  uint64_t lfsr_history_ = 0;  // past LFSR outputs, newest at bit 0
  int zone_ = 0;            // hysteretic mix-method zone: 0 XOR, 1 OR, 2 AND
  int interval_ = 0;        // hysteretic MORPH ratio-ladder step
  int comb_ = -1;           // hysteretic flanger comb position
  bool noise_mode_ = false; // hysteretic MORPH extreme: osc2 becomes an LFSR
  int duty2_index_ = -1;
  int sweep_direction_ = 1;
  uint32_t sweep_count_ = 0;
  float dc_main_ = 0.0f, dc_aux_ = 0.0f;
  float kick_ = 0.0f;       // TRIG kick-slide, semitones above played pitch
};

}  // namespace pd
