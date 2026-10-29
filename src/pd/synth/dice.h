// The dice key: roll a new set of the four controls, or mutate the current
// one. The design follows the Model:Cycles randomizer — per-parameter ranges
// and bias, one stream per parameter so a roll is reproducible from its seed.
#pragma once
#include "../dsp/core.h"
#include "../dsp/rng.h"
#include "events.h"

namespace pd {
namespace dice {

// A fresh sound. HARMONICS leans low (few pins: the Follin sound), TIMBRE
// clusters round 0.6 and keeps clear of the sparse-click bottom, MORPH is
// anything, MACRO clusters round neutral.
inline void roll(float* params, uint32_t seed) {
  auto r = [seed](Param p) { return rng::Rng(seed, rng::kPatchRoll, (uint32_t)p); };
  params[(int)Param::Harmonics] = r(Param::Harmonics).lowBiased();
  params[(int)Param::Timbre]    = 0.25f + 0.75f * r(Param::Timbre).triangular();
  params[(int)Param::Morph]     = r(Param::Morph).uniform();
  params[(int)Param::Macro]     = r(Param::Macro).triangular();
}

// A neighbour of the current sound: each control kept with even odds, else
// nudged up to +-0.2, centre-weighted. One control always moves, and nudges
// fold back off the ends, so a mutate is never a no-op at 0 or 1.
inline void mutate(float* params, uint32_t seed) {
  const int always = (int)rng::Rng(seed, rng::kPatchMutate, 255).below((uint32_t)Param::Count);
  for (int p = 0; p < (int)Param::Count; ++p) {
    rng::Rng r(seed, rng::kPatchMutate, (uint32_t)p);
    if (r.chance(0.5f) && p != always) continue;
    float d = 0.4f * (r.triangular() - 0.5f);
    if (std::fabs(d) < 0.03f) d = d < 0.0f ? -0.03f : 0.03f;
    float v = std::fabs(params[p] + d);
    params[p] = v > 1.0f ? 2.0f - v : v;
  }
}

}  // namespace dice
}  // namespace pd
