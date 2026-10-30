// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// BrocotFm — parallel true FM with Stern-Brocot frequency ratios, ported from
// Brocot (Combust). One sine
// carrier; three modulators at carrier × p/q each add I·f_mod·osc to the
// carrier's instantaneous FREQUENCY (true FM, not DX phase modulation), never
// nested, so the spectrum is a product lattice of Bessel combs at
// f_c(1 + Σ n_k·r_k). Ratios are prefixes of a shared Stern-Brocot trunk
// (Stern 1858 / Brocot 1861; see dsp/stern_brocot.h), so the three ops sit on
// successive convergents of one number and their combs are related.
//
// Controls:
//   HARMONICS  steps (with hysteresis) through nine trunk/position sets, from
//              harmonic 1:2:3 at 0 (its 1:1 op 3 cents sharp, a slow DX-style
//              timbral drift) through the "Avatar" RLLLR default
//              (2, 4/3, 9/7) at noon to the deep, near-coincident "Golden"
//              convergents (13/8, 34/21, 55/34) at 1. Ratios glide 20 ms
//              between steps, so sweeping it plays Brocot's convergent zipper.
//   TIMBRE     FM index, 0..6 squared-law (0 = pure sine), per-op weighted.
//              Key-scaled by 2^(-oct/2) above C4, then capped so the
//              Carson-band top stays under 19.2 kHz (high notes thin out
//              toward a sine instead of folding back to mush).
//   MORPH      modulator wave, sine at noon. CCW: DX-style 2-sample-averaged
//              self-feedback on every op (sine → saw-like, all harmonics).
//              CW: crossfade to Ian Fritz's double pulse at full even-harmonic
//              suppression (odd-only, hollow) by 3/4, then let the even
//              harmonics back in to a plain 25 % pulse at 1 — the saw family
//              one side, the square/pulse family the other, continuous.
//   macro      index envelope on trigger, steady at noon. CW: bell/EP strike,
//              index jumps up and decays (1.2 s → 75 ms) to a lower floor;
//              works even at TIMBRE 0 (strike decays to a pure sine). CCW:
//              brass-style swell, index rises from low over 40 ms → 0.9 s.
//   aux        the modulator bus (weighted op sum, ~0.5 peak): the additive
//              Stern-Brocot chord the carrier is being modulated by.
//
// True-FM DC caveat: a modulator with DC shifts the carrier's mean frequency.
// The pulse at duty d has mean (2d-1)(1-β)/(1+β) after Fritz's construction,
// subtracted analytically; what is left, and the DX feedback sine's own large
// DC, is measured per op over whole cycles and removed (see kDcMinSamples),
// so no MORPH setting detunes the carrier. Phases free-run across notes, as
// in Brocot.
//
// Cost per voice-sample, 3 active ops: 4 LUT sines (3 when the pulse is fully
// crossfaded in), ~8 float mul/add per op (ramps, deviation and aux
// accumulation, DC tracking) plus 2 compares + a 4-entry LUT read in pulse
// mode or a mul + float→int in feedback, and ~5 for the carrier. Ops render
// op-major into out[] (as the deviation buffer) and aux[] with their state in
// registers; coefficients are computed per block and ramped linearly across
// it; libm runs only when the note, macro or block size changes.
#pragma once
#include "../dsp/core.h"
#include "../dsp/stern_brocot.h"
#include "../dsp/tables.h"

namespace pd {

class BrocotFmEngine : public Engine {
 public:
  BrocotFmEngine() { reset(); }

  void reset() override {
    for (auto& o : ops_) o = Op();
    carrier_phase_ = 0;
    dt_ = 0.0f;
    index_ = 0.0f;
    env_ = 0.0f;
    slot_ = -1;
    glide_left_ = 0;
    fresh_ = true;
  }

  PD_HOT void render(const EngineParams& p, float* out, float* aux, size_t size) override {
    if (size == 0) return;
    const bool struck = (p.trigger & kTriggerRising) != 0;
    const float n = (float)size;

    // HARMONICS: trunk/position set, hysteresis-quantized.
    const float raw_slot = clamp(p.harmonics, 0.0f, 1.0f) * ((float)kNumSlots - 0.01f);
    const int candidate = clamp((int)raw_slot, 0, kNumSlots - 1);
    if (slot_ < 0 || (candidate != slot_ &&
        (raw_slot > (float)slot_ + 1.08f || raw_slot < (float)slot_ - 0.08f)))
      loadSlot(candidate, fresh_);
    advanceGlide((int)size);

    // Pitch, snapped on a strike so a reused voice never chirps from its
    // last note.
    const float note = clamp(p.note, 0.0f, 127.0f);
    if (note != cached_note_) {
      cached_note_ = note;
      cached_dt_ = noteToDt(note);
      key_scale_ = std::exp2(-kKeyScale * std::max(0.0f, (note - 60.0f) * (1.0f / 12.0f)));
    }
    const float dt_end = cached_dt_;
    if (fresh_ || struck) dt_ = dt_end;
    const float macro = clamp(p.macro, 0.0f, 1.0f);
    if (size != cached_size_ || macro != cached_macro_) {
      cached_size_ = size;
      cached_macro_ = macro;
      index_coef_ = 1.0f - std::exp(-n / (kIndexSmoothSec * kSampleRate));
      const float tau = macro > 0.5f ? kStrikeTauSec * std::exp2(-8.0f * (macro - 0.5f))
                                     : kSwellTauSec * std::exp2(9.0f * (0.5f - macro));
      env_mul_ = std::exp(-n / (tau * kSampleRate));
    }

    // TIMBRE: FM index, smoothed ~10 ms (the strike envelope is not).
    const float t = clamp(p.timbre, 0.0f, 1.0f);
    const float index_target = kMaxIndex * t * t;
    index_ = fresh_ ? index_target : index_ + index_coef_ * (index_target - index_);

    // macro: index envelope, evaluated at block end.
    if (struck) env_ = 1.0f;
    env_ *= env_mul_;
    float index = index_;
    if (macro > 0.5f) {
      const float a = (macro - 0.5f) * 2.0f;
      index = index_ * (1.0f - 0.8f * a * (1.0f - env_)) + kStrikeIndex * a * env_;
    } else if (macro < 0.5f) {
      const float b = (0.5f - macro) * 2.0f;
      index = (index_ + kSwellIndex * b) * (1.0f - std::min(1.0f, 2.0f * b) * env_);
    }
    if (env_ < 1e-5f) env_ = 0.0f;
    index *= key_scale_;

    // MORPH: modulator wave.
    const float morph = clamp(p.morph, 0.0f, 1.0f);
    Mode mode = Mode::Sine;
    Shape shape;
    float peak = 1.0f;   // bound on |osc| after DC removal, for the band cap
    if (morph < 0.5f) {
      mode = Mode::Feedback;
      const float u = (0.5f - morph) * 2.0f;
      // Self-PM index in radians, as a uint32 phase offset per unit of
      // (y1 + y2), the 1/2 of the average folded in. Its DC (removed) can
      // reach ~0.4, so the output can too beyond unit peak.
      shape.fb_scale = u * kMaxFeedback * 0.5f * (4294967296.0f / kTwoPi);
      peak = 1.0f + 0.45f * u;
    } else if (morph > 0.5f) {
      mode = Mode::Pulse;
      const float v = (morph - 0.5f) * 2.0f;
      const float x = std::min(1.0f, 2.0f * v);                    // sine → double pulse
      const float beta = v < 0.5f ? 1.0f : 1.0f - (v - 0.5f) * 2.0f;  // parity 1 → 0
      // Fritz double pulse (p(t) - β·p(t-T/2)) / (1+β), minus its mean.
      // Index bit 1: p(t) high; bit 0: p(t-T/2) high.
      const float g = 1.0f / (1.0f + beta);
      const float dc = (2.0f * kDuty - 1.0f) * (1.0f - beta) * g;
      float lut_peak = 0.0f;
      for (int i = 0; i < 4; ++i) {
        const float s1 = (i & 2) ? 1.0f : -1.0f;
        const float s2 = (i & 1) ? 1.0f : -1.0f;
        shape.lut[i] = x * ((s1 - beta * s2) * g - dc);
        lut_peak = std::max(lut_peak, std::fabs(shape.lut[i]));
      }
      shape.sine_gain = 1.0f - x;
      peak = shape.sine_gain + lut_peak;
    }

    // Per-op index, band-limited: an op whose own frequency nears Nyquist
    // fades out, and the summed deviation is capped so the Carson top
    // f_c(1 + r_max + peak·Σ I_k r_k) stays under kBandLimit.
    float depth[kOps], sum_a = 0.0f, r_max = 0.0f, w_sum = 0.0f;
    for (int k = 0; k < kOps; ++k) {
      Op& o = ops_[k];
      const float dt_mod = o.ratio * dt_end;
      const float band = clamp((kBandLimit - dt_mod) * (1.0f / kBandFade), 0.0f, 1.0f);
      o.band = o.weight * band;
      depth[k] = index * o.band * o.ratio;
      sum_a += depth[k];
      if (o.band > 0.0f) r_max = std::max(r_max, o.ratio);
      w_sum += o.weight;
    }
    const float budget = std::max(0.0f, kBandLimit / dt_end - 1.0f - r_max);
    const float cap = sum_a * peak > budget ? budget / (sum_a * peak) : 1.0f;
    const float bus_norm = w_sum > 0.0f ? kAuxLevel / w_sum : 0.0f;

    for (size_t i = 0; i < size; ++i) out[i] = aux[i] = 0.0f;   // out = deviation
    const float inv_n = 1.0f / n;
    for (int k = 0; k < kOps; ++k) {
      Op& o = ops_[k];
      const float a_end = depth[k] * cap;
      const float bus = o.band * bus_norm;
      const uint32_t inc_end =
          (uint32_t)(std::min(o.ratio * dt_end, 0.45f) * 4294967296.0f);
      if (fresh_) { o.a = a_end; o.inc = inc_end; }
      if (o.a > 0.0f || a_end > 0.0f || bus > 0.0f) {
        const int32_t inc_step = (int32_t)(inc_end - o.inc) / (int32_t)size;
        const float a_step = (a_end - o.a) * inv_n;
        switch (mode) {
          case Mode::Sine:     renderOp<Mode::Sine>(o, inc_step, a_step, bus, shape, out, aux, size); break;
          case Mode::Feedback: renderOp<Mode::Feedback>(o, inc_step, a_step, bus, shape, out, aux, size); break;
          case Mode::Pulse:    renderOp<Mode::Pulse>(o, inc_step, a_step, bus, shape, out, aux, size); break;
        }
      }
      o.a = a_end;
      o.inc = inc_end;
    }

    // Carrier: integrate f_c(1 + deviation). The cap keeps |f| under
    // kBandLimit; the clamp only guards the float→int conversion across the
    // ramps' cross terms.
    float dt = dt_;
    const float dt_step = (dt_end - dt_) * inv_n;
    uint32_t phase = carrier_phase_;
    for (size_t i = 0; i < size; ++i) {
      const float f = clamp(dt + dt * out[i], -0.45f, 0.45f);
      out[i] = kLevel * sinePhase(phase);
      phase += (uint32_t)(int32_t)(f * 4294967296.0f);
      dt += dt_step;
    }
    carrier_phase_ = phase;
    dt_ = dt_end;
    fresh_ = false;
  }

 private:
  static constexpr int   kOps = 3;
  static constexpr float kLevel = 0.55f;
  static constexpr float kAuxLevel = 0.5f;
  static constexpr float kMaxIndex = 6.0f;
  static constexpr float kKeyScale = 0.5f;        // index halves every 2 octaves above C4
  static constexpr float kBandLimit = 0.4f;       // cycles/sample: 19.2 kHz at 48 k
  static constexpr float kBandFade = 0.06f;
  static constexpr float kMaxFeedback = 2.6f;     // radians; Brocot goes to π
  static constexpr float kDuty = 0.25f;           // Brocot's default pulse duty
  static constexpr uint32_t kDutyPhase = 0x40000000u;
  static constexpr float kIndexSmoothSec = 0.01f;
  static constexpr float kStrikeTauSec = 1.2f;    // ÷16 at full CW
  static constexpr float kStrikeIndex = 3.0f;     // extra index at the strike
  static constexpr float kSwellTauSec = 0.04f;    // ×22.6 at full CCW
  static constexpr float kSwellIndex = 1.0f;      // swell target lift
  // Measured DC removal. The analytic Fritz term is not enough: the sampled
  // naive pulse's mean is 2d-1 only to within ~1/period, and the DX feedback
  // sine is far from DC-free (its one-sample loop delay and, past 1 rad, its
  // hysteresis bias it: mean -0.06 at 0.8 rad, -0.3 at 2.6 rad), and under
  // true FM any of that times I·f_mod moves the carrier. So each op averages
  // its own output over whole cycles (>= 64 samples, closing on a phase wrap)
  // and subtracts it: exact for a periodic op, settled in one window after a
  // big change (a jump > kDcJump is taken whole), and with no filter corner
  // to bend the comb.
  static constexpr int   kDcMinSamples = 64;
  static constexpr float kDcJump = 0.02f;
  static constexpr float kDcBlend = 0.3f;
  static constexpr int   kGlideSamples = (int)(0.02f * kSampleRate);  // 20 ms ratio glide

  enum class Mode : uint8_t { Sine, Feedback, Pulse };

  struct Op {
    uint32_t phase = 0, inc = 0;
    float a = 0.0f;                  // I·r: deviation per unit osc, in carrier units
    float band = 0.0f;               // weight × Nyquist fade, this block
    float y1 = 0.0f, y2 = 0.0f;      // last two outputs, for feedback
    float dc = 0.0f, dc_acc = 0.0f;  // DC estimate; running sum this window
    int dc_count = 0;
    float ratio = 1.0f, ratio_target = 1.0f, ratio_step = 0.0f;   // per sample
    float weight = 0.0f, weight_target = 0.0f, weight_step = 0.0f;
  };

  // A trunk and three positions along it (Brocot's shared mode), relative
  // index weights (0 = op off), and optional fine detune in cents.
  struct Slot {
    SbPath trunk;
    uint8_t pos[kOps];
    float weight[kOps];
    float cents[kOps];
  };
  static constexpr int kNumSlots = 9;
  // The Brocot default trunk: RLLLR (Avatar·1) then alternating, which keeps
  // mediant growth Fibonacci-bounded at every depth.
  static constexpr const char* kDefaultTrunk = "RLLLRLRLRLRLRLRLRLRLRLRLRLRLRLRL";
  static const Slot& slot(int i) {
    static const Slot kSlots[kNumSlots] = {
      {"RR",          {0, 1, 2}, {1.0f, 0.6f, 0.35f}, {3.0f, 0, 0}},  // 1, 2, 3 (Truax; 1:1 drifts 3 c)
      {"RRRR",        {1, 2, 3}, {1.0f, 0.8f, 0.6f},  {0, 0, 0}},     // 2, 3, 4 (Trunk R)
      {kDefaultTrunk, {1, 2, 3}, {1.0f, 0.7f, 0.5f},  {0, 0, 0}},     // 2, 3/2, 4/3
      {"LLLL",        {1, 2, 3}, {1.0f, 0.67f, 0.47f},{0, 0, 0}},     // 1/2, 1/3, 1/4 (Trunk L)
      {kDefaultTrunk, {1, 3, 5}, {1.0f, 0.7f, 0.5f},  {0, 0, 0}},     // 2, 4/3, 9/7 (Avatar·1)
      {"RRLRRLRR",    {1, 4, 6}, {1.0f, 0.6f, 0.4f},  {0, 0, 0}},     // 2, 8/3, 19/7 (Silver)
      {"RLRLRLRL",    {1, 3, 5}, {0.9f, 0.6f, 0.4f},  {0, 0, 0}},     // 2, 5/3, 13/8 (Golden)
      {kDefaultTrunk, {5, 7, 9}, {1.0f, 0.7f, 0.5f},  {0, 0, 0}},     // 9/7, 23/18, 60/47
      {"RLRLRLRL",    {5, 7, 8}, {1.0f, 0.7f, 0.5f},  {0, 0, 0}},     // 13/8, 34/21, 55/34
    };
    return kSlots[i];
  }

  void loadSlot(int s, bool snap) {
    slot_ = s;
    const Slot& sl = slot(s);
    for (int k = 0; k < kOps; ++k) {
      Op& o = ops_[k];
      o.ratio_target = sl.trunk.prefix(sl.pos[k]).ratio() *
                       (sl.cents[k] != 0.0f ? std::exp2(sl.cents[k] * (1.0f / 1200.0f)) : 1.0f);
      o.weight_target = sl.weight[k];
      if (snap || o.weight <= 0.0f) o.ratio = o.ratio_target;   // a silent op needn't glide
      if (snap) o.weight = o.weight_target;
      o.ratio_step = (o.ratio_target - o.ratio) * (1.0f / (float)kGlideSamples);
      o.weight_step = (o.weight_target - o.weight) * (1.0f / (float)kGlideSamples);
    }
    glide_left_ = snap ? 0 : kGlideSamples;
  }

  void advanceGlide(int samples) {
    if (glide_left_ <= 0) return;
    const int m = std::min(samples, glide_left_);
    glide_left_ -= m;
    for (auto& o : ops_) {
      if (glide_left_ == 0) { o.ratio = o.ratio_target; o.weight = o.weight_target; continue; }
      o.ratio += o.ratio_step * (float)m;
      o.weight += o.weight_step * (float)m;
    }
  }

  // Per-block wave settings shared by the op loop.
  struct Shape {
    float fb_scale = 0.0f;           // feedback: uint32 phase offset per unit of y1+y2
    float sine_gain = 1.0f;          // pulse: sine share of the crossfade
    float lut[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // pulse: crossfade-scaled pulse-pair levels
  };

  // One op over the block, op-major so its state lives in registers: adds
  // a·osc into the deviation buffer and bus·osc into aux.
  //   Feedback: DX-style, the op phase-modulates itself by the average of
  //     its last two outputs (the averaging stops the 1-sample loop hunting).
  //   Pulse: sine crossfaded into the double pulse; the half-period-delayed
  //     pulse is the same phase plus 2^31, wrapping for free.
  // Every mode runs the period-synchronous DC tracker (see kDcMinSamples).
  template <Mode kMode>
  static void renderOp(Op& o, int32_t inc_step, float a_step, float bus, const Shape& sh,
                       float* dev, float* aux, size_t size) {
    uint32_t ph = o.phase, inc = o.inc;
    float a = o.a, y1 = o.y1, y2 = o.y2, dc = o.dc, acc = o.dc_acc;
    int count = o.dc_count;
    for (size_t i = 0; i < size; ++i) {
      float raw;
      if constexpr (kMode == Mode::Feedback) {
        raw = sinePhase(ph + (uint32_t)(int32_t)((y1 + y2) * sh.fb_scale));
        y2 = y1;
        y1 = raw;
      } else if constexpr (kMode == Mode::Pulse) {
        raw = sh.lut[(ph < kDutyPhase ? 2 : 0) | (ph + 0x80000000u < kDutyPhase ? 1 : 0)];
        if (sh.sine_gain > 0.0f) raw += sh.sine_gain * sinePhase(ph);
      } else {
        raw = sinePhase(ph);
      }
      const float x = raw - dc;
      dev[i] += a * x;
      aux[i] += bus * x;
      acc += raw;
      ++count;
      const uint32_t prev = ph;
      ph += inc;
      inc += (uint32_t)inc_step;
      a += a_step;
      if (ph < prev && count >= kDcMinSamples) {   // wrapped: whole cycles banked
        const float mean = acc / (float)count;
        dc += (std::fabs(mean - dc) > kDcJump ? 1.0f : kDcBlend) * (mean - dc);
        acc = 0.0f;
        count = 0;
      }
    }
    o.phase = ph;
    o.y1 = y1;
    o.y2 = y2;
    o.dc = dc;
    o.dc_acc = acc;
    o.dc_count = count;
  }

  Op ops_[kOps];
  uint32_t carrier_phase_ = 0;
  float dt_ = 0.0f;           // carrier cycles/sample at the last block end
  float index_ = 0.0f;        // smoothed TIMBRE index
  float env_ = 0.0f;          // macro index envelope, 1 at a strike → 0
  int slot_ = -1;             // hysteretic HARMONICS slot
  int glide_left_ = 0;        // samples left in the ratio/weight glide
  bool fresh_ = true;         // first render after reset: snap every ramp
  float cached_note_ = -1.0f, cached_dt_ = 0.0f, key_scale_ = 1.0f;
  size_t cached_size_ = 0;
  float cached_macro_ = -1.0f;
  float index_coef_ = 0.0f;   // per-block TIMBRE smoothing
  float env_mul_ = 1.0f;      // per-block macro envelope decay
};

}  // namespace pd
