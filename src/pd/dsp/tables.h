// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared lookup tables and fast math. Pure C++, no Arduino — host-buildable.
// Phases are uint32 accumulators (one cycle = 2^32) throughout the framework:
// exact, wrap for free, and index tables with a shift.
#pragma once
#include <cmath>
#include <cstdint>

namespace pd {

constexpr int   kSineBits = 10;
constexpr int   kSineSize = 1 << kSineBits;
constexpr float kTwoPi    = 6.28318530717958647692f;
constexpr float kPhaseToFloat = 1.0f / 4294967296.0f;   // uint32 phase -> 0..1

struct SineTable {
  float v[kSineSize + 1];   // guard point so interpolation never wraps
  SineTable() {
    for (int i = 0; i <= kSineSize; ++i) v[i] = std::sin(kTwoPi * i / kSineSize);
  }
};

inline const SineTable& sineTable() {
  static const SineTable t;
  return t;
}

// sin(2π · phase/2^32), linear interpolation on a 1024-point table
// (worst-case error ~5e-6, well under 16-bit resolution).
// From a table pointer the caller hoisted out of its loop: sineTable() is a
// function-local static, guard-checked on every call.
inline float sineLut(const float* t, uint32_t phase) {
  const uint32_t i = phase >> (32 - kSineBits);
  const float frac = (float)(phase & ((1u << (32 - kSineBits)) - 1)) *
                     (1.0f / (float)(1u << (32 - kSineBits)));
  return t[i] + (t[i + 1] - t[i]) * frac;
}

inline float sinePhase(uint32_t phase) { return sineLut(sineTable().v, phase); }

// Frequency in Hz -> uint32 phase increment at sample rate sr.
inline uint32_t phaseIncrement(float hz, float sr) {
  return (uint32_t)(hz / sr * 4294967296.0f);
}

}  // namespace pd
