// Master-bus effects: three slots, each holding one effect from its own
// short menu, with one amount. Amount 0 bypasses a slot exactly. They run
// once on the summed bus (not per voice), in a fixed order:
//   SHAPE  FOLD (sine wavefolder) / RING (ring mod) / TREM (tempo tremolo)
//   GRIT   CRUSH (bits + rate) / PWM (1-bit pulse-width re-encode, the
//          ZX beeper's way of playing samples) / DRIVE (saturation)
//   TONE   LP / BP / HP (state-variable filter, Simper's TPT form)
// The fourth FX bar, SPACE, lives in the echo (dsp/echo.h). Amounts glide
// (~4 ms) so key steps and tilt don't zipper. While any slot is engaged, a
// peak limiter (instant attack, 80 ms release, transparent below 0.95)
// ends the chain: a high-passed square spikes to twice its height, and
// clipping those is what made the filter sound crushed. No buffers: the
// whole thing is a few dozen floats. Pure — host-testable (fx_test.cpp).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core.h"
#include "tables.h"

namespace pd {

class MasterFx {
 public:
  enum Slot : uint8_t { Tone, Grit, Shape, kSlots };
  static constexpr int kTypes = 3;
  // Effect names, by slot and type.
  static const char* name(int slot, int type) {
    static const char* const kNames[kSlots][kTypes] = {
        {"LP", "BP", "HP"}, {"CRUSH", "PWM", "DRIVE"}, {"FOLD", "RING", "TREM"}};
    return kNames[slot][type];
  }

  void set(int slot, int type, float amount) {
    if (slot < 0 || slot >= kSlots) return;
    if (type_[slot] != (uint8_t)clamp(type, 0, kTypes - 1)) {   // a new effect starts clean
      type_[slot] = (uint8_t)clamp(type, 0, kTypes - 1);
      amount_[slot] = 0.0f;
    }
    target_[slot] = clamp(amount, 0.0f, 1.0f);
    dirty_ = true;
  }
  void setTempo(float bpm) {
    bpm_ = clamp(bpm, 20.0f, 400.0f);
    preparedAmount_[Shape] = -1.0f;   // TREM's rate follows the tempo
    dirty_ = true;
  }

  PD_HOT __attribute__((noinline)) void process(float* io, size_t n) {
    if (dirty_) glide();
    const bool engaged = amount_[Shape] > 0.0f || amount_[Grit] > 0.0f || amount_[Tone] > 0.0f;
    if (!engaged) { env_ = 0.0f; return; }   // all off: the dry bus, untouched
    if (amount_[Shape] > 0.0f) shape(io, n);
    if (amount_[Grit] > 0.0f) grit(io, n);
    if (amount_[Tone] > 0.0f) tone(io, n);
    limit(io, n);
  }

 private:
  // Amounts glide to their targets a block at a time (~4 ms), and the
  // coefficients follow (libm per block while gliding, never per sample).
  void glide() {
    bool moving = false;
    for (int s = 0; s < kSlots; ++s) {
      const float d = target_[s] - amount_[s];
      if (std::fabs(d) < 1e-4f) amount_[s] = target_[s];
      else { amount_[s] += 0.15f * d; moving = true; }
    }
    prepare();
    dirty_ = moving;
  }

  // Each slot's coefficients are recomputed only when its amount or type
  // has moved since they were last made (a tilt sweep moves one slot).
  void prepare() {
    for (int s = 0; s < kSlots; ++s) {
      if (amount_[s] == preparedAmount_[s] && type_[s] == preparedType_[s]) continue;
      preparedAmount_[s] = amount_[s];
      preparedType_[s] = type_[s];
      if (s == Tone) prepareTone();
      else if (s == Grit) prepareGrit();
      else prepareShape();
    }
  }

  void prepareTone() {
    const float a = amount_[Tone];
    const int t = type_[Tone];
    // LP closes from 18 kHz down to 80 Hz, HP opens from 20 Hz up to 6 kHz,
    // BP sweeps its centre 100 Hz .. 8 kHz; resonance rises with the sweep.
    float fc;
    if (t == 0) fc = 18000.0f * std::pow(80.0f / 18000.0f, a);
    else if (t == 2) fc = 20.0f * std::pow(6000.0f / 20.0f, a);
    else fc = 100.0f * std::pow(80.0f, a);
    const float q = t == 1 ? 1.5f : 0.707f + 0.5f * a;   // gentle: square edges ring
    svfG_ = std::tan(3.14159265f * clamp(fc, 10.0f, 0.45f * kSampleRate) / kSampleRate);
    svfK_ = 1.0f / q;
    svfA1_ = 1.0f / (1.0f + svfG_ * (svfG_ + svfK_));
    svfA2_ = svfG_ * svfA1_;
    svfA3_ = svfG_ * svfA2_;
  }

  void prepareGrit() {
    const float g = amount_[Grit];
    // CRUSH: 12 bits -> 3, and a hold of 1 -> 24 samples (48 kHz -> 2 kHz).
    crushQ_ = std::exp2(11.0f - 9.0f * g);
    crushHold_ = 1u + (uint32_t)(23.0f * g * g);
    // PWM: the carrier falls from 16 kHz to 1.2 kHz.
    pwmInc_ = (uint32_t)(16000.0f * std::pow(1200.0f / 16000.0f, g) / kSampleRate * 4294967296.0f);
    // DRIVE: up to 30 dB into the curve.
    driveGain_ = std::exp2(5.0f * g);
  }

  void prepareShape() {
    const float s = amount_[Shape];
    foldGain_ = 1.0f + 7.0f * s;                     // 1 .. 8 folds
    ringInc_ = (uint32_t)(30.0f * std::pow(2000.0f / 30.0f, s) / kSampleRate * 4294967296.0f);
    tremInc_ = (uint32_t)(bpm_ / 60.0f * 2.0f / kSampleRate * 4294967296.0f);   // 8th notes
  }

  void shape(float* io, size_t n) {
    const float* sine = sineTable().v;
    const float s = amount_[Shape];
    switch (type_[Shape]) {
      case 0:   // FOLD: sin(gain * x * pi/2) folds back past +-1, blended in
        for (size_t i = 0; i < n; ++i) {
          float u = clamp(io[i], -4.0f, 4.0f) * foldGain_ * 0.25f;   // a whole fold cycle per 4
          u -= std::floor(u);
          const float folded = sineLut(sine, (uint32_t)(u * 4294967295.0f));
          io[i] += s * (folded - io[i]);
        }
        break;
      case 1:   // RING: times a sine, 30 Hz .. 2 kHz, fully wet
        for (size_t i = 0; i < n; ++i) {
          ringPhase_ += ringInc_;
          io[i] *= sineLut(sine, ringPhase_);
        }
        break;
      default:  // TREM: 8th-note sine tremolo, amount = depth
        for (size_t i = 0; i < n; ++i) {
          tremPhase_ += tremInc_;
          io[i] *= 1.0f - s * 0.5f * (1.0f + sineLut(sine, tremPhase_));
        }
        break;
    }
  }

  void grit(float* io, size_t n) {
    const float g = amount_[Grit];
    switch (type_[Grit]) {
      case 0:   // CRUSH: sample-and-hold, then requantize
        for (size_t i = 0; i < n; ++i) {
          if (++holdCount_ >= crushHold_) { holdCount_ = 0; held_ = io[i]; }
          io[i] = std::round(held_ * crushQ_) / crushQ_;
        }
        break;
      case 1: { // PWM: pulses whose width carries the level and whose sign
        // carries the polarity (three levels, so silence stays silent and
        // the carrier only sounds under the voice)
        for (size_t i = 0; i < n; ++i) {
          pwmPhase_ += pwmInc_;
          const float x = clamp(io[i], -1.0f, 1.0f);
          const bool high = (float)pwmPhase_ * (1.0f / 4294967296.0f) < std::fabs(x);
          const float bit = high ? (x < 0.0f ? -1.0f : 1.0f) : 0.0f;
          io[i] = g >= 0.15f ? bit : io[i] + g * (1.0f / 0.15f) * (bit - io[i]);
        }
        break;
      }
      default: {  // DRIVE: x / (1 + |x|) at up to 30 dB of gain, full scale at 0.8
        const float makeup = 0.8f * (1.0f + driveGain_) / driveGain_;
        const float wet = g < 0.15f ? g * (1.0f / 0.15f) : 1.0f;
        for (size_t i = 0; i < n; ++i) {
          const float x = io[i] * driveGain_;
          io[i] += wet * (x / (1.0f + std::fabs(x)) * makeup - io[i]);
        }
        break;
      }
    }
  }

  void tone(float* io, size_t n) {
    const int t = type_[Tone];
    float ic1 = ic1_, ic2 = ic2_;
    for (size_t i = 0; i < n; ++i) {
      const float v0 = io[i];
      const float v3 = v0 - ic2;
      const float v1 = svfA1_ * ic1 + svfA2_ * v3;
      const float v2 = ic2 + svfA2_ * ic1 + svfA3_ * v3;
      ic1 = 2.0f * v1 - ic1;
      ic2 = 2.0f * v2 - ic2;
      io[i] = t == 0 ? v2 : t == 1 ? v1 * svfK_ : v0 - svfK_ * v1 - v2;   // BP: unity at its peak
    }
    // Flush denormals so a long silence can't slow the loop.
    ic1_ = std::fabs(ic1) < 1e-20f ? 0.0f : ic1;
    ic2_ = std::fabs(ic2) < 1e-20f ? 0.0f : ic2;
  }

  // Peak limiter: the envelope jumps to a new peak and falls over ~80 ms;
  // gain only drops while it's above the threshold.
  void limit(float* io, size_t n) {
    float env = env_;
    for (size_t i = 0; i < n; ++i) {
      const float a = std::fabs(io[i]);
      env = a > env ? a : env + kRelease * (a - env);
      if (env > kThreshold) io[i] *= kThreshold / env;
    }
    env_ = env;
  }
  static constexpr float kThreshold = 0.95f;
  static constexpr float kRelease = 1.0f / (0.08f * 48000.0f);

  uint8_t type_[kSlots] = {0, 0, 0};
  float amount_[kSlots] = {0.0f, 0.0f, 0.0f};   // gliding
  float target_[kSlots] = {0.0f, 0.0f, 0.0f};
  float env_ = 0.0f;
  float preparedAmount_[kSlots] = {-1.0f, -1.0f, -1.0f};
  uint8_t preparedType_[kSlots] = {255, 255, 255};
  float bpm_ = 120.0f;
  bool dirty_ = true;

  float svfG_ = 0, svfK_ = 1, svfA1_ = 0, svfA2_ = 0, svfA3_ = 0, ic1_ = 0, ic2_ = 0;
  float crushQ_ = 2048, held_ = 0;
  uint32_t crushHold_ = 1, holdCount_ = 0;
  uint32_t pwmInc_ = 0, pwmPhase_ = 0;
  float driveGain_ = 1;
  float foldGain_ = 1;
  uint32_t ringInc_ = 0, ringPhase_ = 0, tremInc_ = 0, tremPhase_ = 0;
};

}  // namespace pd
