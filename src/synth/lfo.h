// Low-frequency oscillator. Pure math, no Arduino/M5 includes — host-testable.
// Phase-accumulator LFO with sine, triangle, saw, and square waveforms.
// Runs at audio rate but controls sub-audio parameters (0.1–20 Hz typical).
// The phase is a uint32 accumulator (one cycle = 2^32): exact, wraps for free,
// and keeps slow rates on pitch where a float phase would drift by percent.
// No double: it is soft-float on the ESP32-S3.
#pragma once
#include <cmath>
#include <cstdint>

namespace synth {

enum class LfoShape { Sine, Tri, Saw, Square };

struct Lfo {
  uint32_t phase    = 0;     // 0..2^32 = one cycle
  uint32_t phaseInc = 0;     // cycle fraction per sample, scaled by 2^32
  float    depth    = 1.0f;  // output scale (0..1)
  LfoShape shape    = LfoShape::Sine;

  // Set frequency in Hz at the given sample rate.
  void setFreq(float hz, float sample_rate) {
    if (hz < 0.0f) hz = 0.0f;
    phaseInc = (uint32_t)(hz / sample_rate * CYCLE + 0.5f);
  }

  // Advance one sample and return the LFO value in -depth..+depth.
  // When depth is zero, skip the waveform evaluation but still advance
  // phase so the LFO stays in sync if depth is later raised mid-stream.
  float step() {
    if (depth == 0.0f) {
      phase += phaseInc;
      return 0.0f;
    }
    float t = (float)phase * (1.0f / CYCLE);   // 0..1 through the cycle
    float val = 0.0f;
    switch (shape) {
      case LfoShape::Sine:
        val = std::sin(TWO_PI_CONST * t);
        break;
      case LfoShape::Tri:
        // first half -1..+1, second half +1..-1
        val = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t);
        break;
      case LfoShape::Saw:
        val = 2.0f * t - 1.0f;  // -1..+1 over the cycle
        break;
      case LfoShape::Square:
        val = (t < 0.5f) ? 1.0f : -1.0f;
        break;
    }
    phase += phaseInc;
    return val * depth;
  }

  void reset() { phase = 0; }

private:
  static constexpr float CYCLE        = 4294967296.0f;  // 2^32
  static constexpr float TWO_PI_CONST = 6.28318530717958647692f;
};

}  // namespace synth
