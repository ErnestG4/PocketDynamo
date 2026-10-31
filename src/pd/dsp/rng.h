// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Seeded PRNG with named streams, ported from the Model:Cycles generator core
// (fw/core/prng.c, Combust). Every random decision is
// a function of (seed, stream, sub), so adding draws to one feature never
// changes another's. lowbias32 hash constants: Chris Wellons (public domain).
// xorshift32: George Marsaglia.
#pragma once
#include <algorithm>
#include <cstdint>

namespace pd {
namespace rng {

inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Stream ids. Cycles used 0..8 for its sequencer; ours start above them.
enum Stream : uint32_t { kPatchRoll = 16, kPatchMutate = 17 };

class Rng {
 public:
  Rng(uint32_t seed, uint32_t stream, uint32_t sub) {
    s_ = hash32(seed ^ hash32(stream * 0x9e3779b9U + sub));
    if (!s_) s_ = 0x6d2b79f5U;
  }
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  // Uniform in [0, n), 1 <= n <= 65536.
  uint32_t below(uint32_t n) { return ((next() >> 16) * n) >> 16; }
  float uniform() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
  // Cycles' shaping: low-biased = min of three ("mostly tame, sometimes
  // weird"), triangular = mean of two (clusters round the middle).
  float lowBiased() { return std::min(uniform(), std::min(uniform(), uniform())); }
  float triangular() { return 0.5f * (uniform() + uniform()); }
  bool chance(float p) { return uniform() < p; }

 private:
  uint32_t s_;
};

}  // namespace rng
}  // namespace pd
