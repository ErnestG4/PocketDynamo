// Framework-wide DSP basics: sample rate, pitch math, the engine interface.
// Pure C++, no Arduino — host-buildable. Engines are Plaits-shaped: four
// normalized controls plus pitch and a trigger, rendered a block at a time.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef PD_SAMPLE_RATE
#define PD_SAMPLE_RATE 48000.0f   // the host harness overrides this to 47872.34f
#endif                            // to compare against Plaits-lab renders

namespace pd {

constexpr float kSampleRate = PD_SAMPLE_RATE;

template <typename T>
inline T clamp(T x, T lo, T hi) { return std::min(std::max(x, lo), hi); }

inline float semitonesToRatio(float st) { return std::exp2(st * (1.0f / 12.0f)); }

// MIDI note -> normalized frequency (cycles per sample), A4 = 69 = 440 Hz.
inline float noteToDt(float note) {
  return (440.0f / kSampleRate) * semitonesToRatio(clamp(note, -128.0f, 127.0f) - 69.0f);
}

enum : uint8_t { kTriggerRising = 1 };

struct EngineParams {
  float   note      = 60.0f;  // MIDI note, fractional
  float   harmonics = 0.5f;   // the four Plaits controls, 0..1
  float   timbre    = 0.5f;
  float   morph     = 0.5f;
  float   macro     = 0.5f;   // plaits-lab's fourth control; 0.5 = neutral
  float   tempo     = 120.0f; // bpm, for engines that play on a grid
  uint8_t trigger   = 0;      // kTriggerRising on the block a note starts
};

class Engine {
 public:
  virtual ~Engine() {}
  virtual void reset() = 0;
  // out: main, aux: alternate output. Both n samples, roughly -1..1.
  virtual void render(const EngineParams& p, float* out, float* aux, size_t n) = 0;
};

}  // namespace pd
