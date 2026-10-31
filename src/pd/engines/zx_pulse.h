// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ZxPulse — ZX Spectrum beeper-style pulse-frequency-modulation click synthesis,
// ported from the plaits-lab ZxPulse48k package. HARMONICS selects the chord,
// MORPH builds the stack (low half) or runs the arp with continuous rate (high
// half), TIMBRE is pin width (the level control), and macro is echo (CW) /
// exaggerated pitch pull (CCW), neutral at noon with the authentic <=3-cent
// pull always on. Voiced two octaves below the played pitch: pin trains carry
// weak fundamentals, so the register is chosen by ear (hardware pass,
// 2026-08-14).
#pragma once
#include "../dsp/chords.h"
#include "../dsp/pulse_core.h"

namespace pd {

class ZxPulseEngine : public Engine {
 public:
  ZxPulseEngine() { reset(); }

  void reset() override {
    for (int v = 0; v < kNumVoices; ++v) {
      // Staggered phases so a strike never fires all pins on one sample.
      channels_[v].reset();
      for (int k = 0; k < v; ++k) {
        float dummy;
        channels_[v].next(0.13f, 0.13f, &dummy);
      }
    }
    tick_.reset();
    tick_count_ = 0;
    arp_index_ = 0;
    history_pos_ = 0;
    for (int i = 0; i < kHistorySize; ++i) dt_history_[i] = width_history_[i] = 0.0f;
  }

  PD_HOT void render(const EngineParams& p, float* out, float* aux, size_t size) override {
    chords_.set(p.harmonics);
    const float* ratios = chords_.ratios();

    if (p.trigger & kTriggerRising) reset();

    // Pin width in samples, exponential: 0.5 (whisper clicks) to ~48 (full
    // body). Width is the level control, as on the hardware that inspired it.
    const float width = 0.5f * semitonesToRatio(79.2f * p.timbre);

    // MORPH low half builds the stack; high half is the arp, its rate rising
    // continuously across the span (tick/6 .. tick/1 at 50 Hz).
    const float morph = p.morph;
    if (!arp_mode_ && morph > 0.52f) arp_mode_ = true;
    else if (arp_mode_ && morph < 0.48f) arp_mode_ = false;
    int arp_divider = 1;
    if (arp_mode_) {
      const float q = clamp((morph - 0.52f) * (1.0f / 0.48f), 0.0f, 1.0f);
      arp_divider = 6 - (int)(q * 5.49f);
    } else {
      const float raw = 1.0f + morph * (1.0f / 0.48f) * 4.49f;
      if (raw > (float)stack_count_ + 0.6f || raw < (float)stack_count_ - 0.6f)
        stack_count_ = clamp((int)raw, 1, kNumVoices);
    }

    // macro: CW = beeper echo, delay 2..11 ticks (~40..220 ms). Noon =
    // neutral (authentic pull stays on). CCW = exaggerated dying-speaker pull.
    const float macro = p.macro;
    const bool echo_on = macro > 0.55f;
    const float echo_amount = echo_on ? (macro - 0.55f) * (1.0f / 0.45f) : 0.0f;
    const int echo_delay = 2 + (int)(echo_amount * 9.99f);
    const float echo_width_scale = 0.75f - 0.4f * echo_amount;
    const float pull_depth = macro < 0.45f
        ? 0.03f + (0.45f - macro) * (1.0f / 0.45f) * 0.09f
        : 0.03f;

    // Assemble voices from the chord. Stack mode: root, chord tones, root
    // octave on top. Arp mode: root + fifth drone under the arp voice.
    const float f0 = noteToDt(p.note + kTwoOctavesDown);
    float dts[kNumVoices], duties[kNumVoices], amps[kNumVoices];
    int n = 0, arp_slot = -1;
    if (arp_mode_) {
      dts[n] = f0;        amps[n++] = kVoiceAmp;
      dts[n] = f0 * 1.5f; amps[n++] = kVoiceAmp;
      arp_slot = n;
      dts[n] = f0 * ratios[arp_index_ & 3] * 2.0f; amps[n++] = kVoiceAmp;
    } else {
      int count = stack_count_;
      if (echo_on && count == kNumVoices) --count;  // the last slot yields to the echo.
      for (int v = 0; v < count; ++v) {
        dts[n] = v < 4 ? f0 * ratios[v] : f0 * ratios[0] * 2.0f;
        amps[n++] = kVoiceAmp;
      }
    }
    // Echo ghosts the melody: the arp voice, or the TOP stack voice — a root
    // echo would be a near-inaudible unison.
    const int echo_src = arp_mode_ ? arp_slot : (n > 0 ? n - 1 : 0);
    int echo_slot = -1;
    if (echo_on && n < kNumVoices) echo_slot = n++;

    // Pitch pull: total pin traffic drags the whole bus flat.
    float traffic = 0.0f;
    for (int v = 0; v < n; ++v) {
      if (v == echo_slot) continue;
      dts[v] = clamp(dts[v], 0.0f, kMaxDt);
      duties[v] = clampDuty(width * dts[v], dts[v]);
      traffic += duties[v];
    }
    const float load = clamp(traffic * (1.0f / 0.15f), 0.0f, 1.5f);
    // 0.02 per Plaits block of 12 samples, scaled to this block size.
    const float pull_coef = 1.0f - std::pow(0.98f, (float)size * (1.0f / 12.0f));
    pull_ += pull_coef * (pull_depth * load - pull_);
    const float pull_ratio = semitonesToRatio(-pull_);
    for (int v = 0; v < n; ++v)
      if (v != echo_slot) dts[v] *= pull_ratio;

    if (echo_slot >= 0) {
      const int read = (history_pos_ - echo_delay) & (kHistorySize - 1);
      dts[echo_slot] = dt_history_[read];
      duties[echo_slot] = clampDuty(width_history_[read] * echo_width_scale, dts[echo_slot]);
      amps[echo_slot] = kEchoAmp;
    }

    // Channel by channel, a block at a time (each channel's phase stays in a
    // register); the block splits at the 50 Hz tick, where the echo history
    // records and the arp may re-pitch. Each sample still sums its channels
    // in order from 0, so the output is unchanged.
    for (size_t i = 0; i < size; ++i) out[i] = aux[i] = 0.0f;
    const int32_t tickAt = tick_.pending();
    const size_t split = tickAt < (int32_t)size ? (size_t)tickAt : size;
    for (int v = 0; v < n; ++v) channels_[v].run(dts[v], duties[v], amps[v], out, aux, 0, split);
    if (split < size) {
      dt_history_[history_pos_] = dts[echo_src];
      width_history_[history_pos_] = duties[echo_src];
      history_pos_ = (history_pos_ + 1) & (kHistorySize - 1);
      ++tick_count_;
      if (arp_mode_ && (tick_count_ % (uint32_t)arp_divider) == 0) {
        arp_index_ = (arp_index_ + 1) & 3;
        if (arp_slot >= 0) {
          const float dt = clamp(f0 * ratios[arp_index_ & 3] * 2.0f * pull_ratio, 0.0f, kMaxDt);
          dts[arp_slot] = dt;
          duties[arp_slot] = clampDuty(width * dt, dt);
        }
      }
      for (int v = 0; v < n; ++v) channels_[v].run(dts[v], duties[v], amps[v], out, aux, split, size);
    }
    tick_.advance((int32_t)size);
  }

 private:
  static constexpr int   kNumVoices = 5;
  static constexpr int   kHistorySize = 16;
  static constexpr float kVoiceAmp = 0.3f;
  static constexpr float kEchoAmp = 0.24f;
  static constexpr float kMaxDt = 0.2f;
  static constexpr float kTwoOctavesDown = -24.0f;

  static float clampDuty(float duty, float dt) { return std::min(std::max(duty, dt), 0.45f); }

  ChordBank chords_;
  pulse::PinChannel channels_[kNumVoices];
  pulse::FrameTick tick_;

  bool arp_mode_ = false;   // hysteretic MORPH halves: stack vs arp
  int stack_count_ = 1;     // hysteretic voice count in stack mode
  uint32_t tick_count_ = 0;
  int arp_index_ = 0;

  // Per-tick history of the lead voice's frequency/width, read by the echo.
  float dt_history_[kHistorySize];
  float width_history_[kHistorySize];
  int history_pos_ = 0;

  float pull_ = 0.0f;       // smoothed pitch-pull amount, semitones
};

}  // namespace pd
