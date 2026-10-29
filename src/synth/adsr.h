// Linear ADSR envelope + monophonic gate. Pure math, NO Arduino/M5 includes —
// host-testable with plain g++ (see adsr_test.cpp). Hardware glue (speaker
// streaming, keyboard gate) lives in main.cpp.
//
// Model: gate on -> Attack (ramp 0->1) -> Decay (ramp 1->sustain) -> Sustain
// (hold while key held) -> gate off -> Release (ramp current->0) -> Idle.
// step() advances one output sample and returns amplitude 0..1.
// float throughout: the ESP32-S3 FPU is single-precision (double is soft-float),
// and per-stage increments are computed at gate time so step() never divides.
#pragma once

namespace synth {

enum class EnvStage { Idle, Attack, Decay, Sustain, Release };

struct Adsr {
  // Config in samples (attack/decay/release) and level (sustain, 0..1).
  int    attack  = 1;     // samples to ramp 0 -> 1      (>= 1)
  int    decay   = 1;     // samples to ramp 1 -> sustain (>= 1)
  float  sustain = 1.0f;  // hold level 0..1
  int    release = 1;     // samples to ramp current -> 0 (>= 1)

  EnvStage stage   = EnvStage::Idle;
  float    level   = 0.0f; // current amplitude 0..1
  float    atkStep = 1.0f; // per-sample attack increment, fixed at gate-on
  float    decStep = 0.0f; // per-sample decay decrement, fixed at gate-on
  float    relStep = 0.0f; // per-sample release decrement, fixed at gate-off

  static int clampPos(int n) { return n < 1 ? 1 : n; }

  // Set ADSR from milliseconds at sample rate sr.
  void configMs(int sr, float aMs, float dMs, float sLevel, float rMs) {
    attack  = clampPos((int)(aMs * sr / 1000.0f));
    decay   = clampPos((int)(dMs * sr / 1000.0f));
    release = clampPos((int)(rMs * sr / 1000.0f));
    sustain = sLevel < 0.0f ? 0.0f : (sLevel > 1.0f ? 1.0f : sLevel);
  }

  // Gate on: (re)trigger from the current level — Attack always rises to 1,
  // so a retrigger mid-release clicks up cleanly rather than jumping.
  void gateOn() {
    atkStep = 1.0f / attack;
    decStep = (1.0f - sustain) / decay;
    stage = EnvStage::Attack;
  }

  // Gate off: ramp from wherever we are now down to 0 over `release` samples.
  void gateOff() {
    if (stage == EnvStage::Idle) return;
    relStep = level / release;
    stage = EnvStage::Release;
  }

  bool active() const { return stage != EnvStage::Idle; }

  // Advance one sample, return amplitude 0..1.
  float step() {
    switch (stage) {
      case EnvStage::Idle:
        level = 0.0f;
        break;
      case EnvStage::Attack:
        level += atkStep;
        if (level >= 1.0f) { level = 1.0f; stage = EnvStage::Decay; }
        break;
      case EnvStage::Decay:
        level -= decStep;
        if (level <= sustain) { level = sustain; stage = EnvStage::Sustain; }
        break;
      case EnvStage::Sustain:
        level = sustain;
        break;
      case EnvStage::Release:
        level -= relStep;
        // 1e-6 not 0: float accumulation can leave a dangling ~1e-8 sample.
        if (level <= 1e-6f) { level = 0.0f; stage = EnvStage::Idle; }
        break;
    }
    return level;
  }
};

}  // namespace synth
