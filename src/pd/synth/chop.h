// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Audiothurgist's Classic chop grammar (the BreakFix generator), ported from
// core/bb_pattern.h (Combust). Same RNG, same draws, same constants — a seed gives the
// same chops as on the module. Only Classic is here, with no drum-class or
// salience data (every swap is class-free, as the module plays an
// unanalysed loop).
//
// The bar tiles into ALIGNED power-of-2 cells (the "Brocot walk": start big,
// halve while the RNG says subdivide), and each cell plays in place, repeats
// the previous cell, or swaps with another, sometimes reversed. One knob
// lerps five anchored zones: verbatim -> DJ edit -> chops -> rolls ->
// micro-madness; below ~9 o'clock an ARS cluster field places burst centres
// (micro-cells, repeats, 32nd ratchet doubles) with rests and single-16th
// ticks between them. Everything tiles, everything lands on the grid.
// Pure — host-testable.
#pragma once
#include <cmath>
#include <cstdint>

namespace pd {
namespace chop {

// xorshift64* (bb_rng.h).
struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  void Seed(uint64_t x) { s = x ? x : 0x9E3779B97F4A7C15ULL; }
  uint32_t Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return (uint32_t)((s * 0x2545F4914F6CDD1DULL) >> 32);
  }
  float Uniform() { return (Next() >> 8) * (1.f / 16777216.f); }
};

struct Pattern {
  static constexpr int kMaxSteps = 64;
  static constexpr int8_t kInterior = -1, kRest = -2;

  int8_t src[kMaxSteps];   // at cell starts: source step; kInterior inside a cell; kRest
  int8_t len[kMaxSteps];   // cell length in steps (at cell starts)
  bool   rev[kMaxSteps];
  bool   rat[kMaxSteps];   // 32nd ratchet double

  // v: 1 = big cells, verbatim ... 0 = micro cells, heavy repeats/reversal.
  void Generate(int nSteps, int stepsPerBeat, float v, uint64_t cellSeed, uint64_t fillSeed) {
    Rng cr, fr;
    cr.Seed(cellSeed);
    fr.Seed(fillSeed);
    for (int i = 0; i < kMaxSteps; i++) { src[i] = kInterior; len[i] = 0; rev[i] = false; rat[i] = false; }

    //                 v=0     .25    .5     .75    1
    const float aSub[]    = {0.70f, 0.60f, 0.50f, 0.10f, 0.05f};
    const float aPlace[]  = {0.10f, 0.25f, 0.45f, 0.60f, 1.00f};
    const float aRep[]    = {0.65f, 0.55f, 0.25f, 0.03f, 0.00f};
    const float aRev[]    = {0.25f, 0.12f, 0.03f, 0.00f, 0.00f};
    const float aCell[]   = {0.90f, 1.40f, 1.90f, 2.40f, 2.90f};
    float vc = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    if (vc > 0.96f) vc = 1.f;   // hard verbatim detent
    int zi = (int)(vc * 4.f);
    if (zi > 3) zi = 3;
    const float zt = vc * 4.f - (float)zi;
    auto lerp = [&](const float* a) { return a[zi] + zt * (a[zi + 1] - a[zi]); };
    const float subdiv   = lerp(aSub);
    const float pInPlace = lerp(aPlace);
    const float pRepeat  = lerp(aRep);
    const float pReverse = lerp(aRev);
    const float cellF    = lerp(aCell);
    // (The module also lerps a class-strictness here; with no drum classes
    // every candidate passes, but the draw it gates is kept below so the
    // RNG stream matches.)
    const int maxCell = cellF >= 2.5f    ? 2 * stepsPerBeat
                      : cellF >= 1.75f ? stepsPerBeat
                      : cellF >= 1.20f ? stepsPerBeat / 2
                                       : 1;
    const float repShare = pRepeat / (1.f - pInPlace + 1e-6f);

    // ARS cluster field below the roll door.
    const float clusterAmt = vc < 0.35f ? (0.35f - vc) / 0.35f : 0.f;
    uint8_t centers[kMaxSteps];
    int nCent = 0;
    const int barSteps = 4 * stepsPerBeat;
    if (clusterAmt > 0.01f) {
      for (int b0 = 0; b0 < nSteps; b0 += barSteps) {
        const int nc = 1 + (cr.Uniform() < 0.5f ? 1 : 0);
        for (int j = 0; j < nc && nCent < kMaxSteps; j++)
          centers[nCent++] = (uint8_t)(b0 + (int)(cr.Uniform() * (float)barSteps));
      }
    }
    auto activity = [&](int p) -> float {
      if (nCent == 0) return 1.f;
      int best = 1 << 30;
      for (int j = 0; j < nCent; j++) {
        int d = p - (int)centers[j];
        if (d < 0) d = -d;
        const int dw = nSteps - d;
        if (dw < d) d = dw;
        if (d < best) best = d;
      }
      const float x = (float)best / 2.5f;
      return std::exp(-x * x);
    };

    int pos = 0, prevSrc = 0;
    while (pos < nSteps) {
      const float a = clusterAmt > 0.01f ? activity(pos) : 1.f;
      const float subdivE = subdiv * (clusterAmt > 0.01f ? (0.2f + 0.8f * a) : 1.f);
      int cl = maxCell;
      while (cl > 1 && cr.Uniform() < subdivE) cl >>= 1;
      int align = pos == 0 ? maxCell : (pos & -pos);
      if (align > maxCell) align = maxCell;
      if (cl > align) cl = align;
      if (pos + cl > nSteps) cl = nSteps - pos;

      // Far from a burst core: rests, and single-16th ticks.
      const float away = clusterAmt > 0.01f ? clusterAmt * (1.f - a) : 0.f;
      if (away > 0.f && fr.Uniform() < away * 0.85f) {
        if (fr.Uniform() < 0.45f) {
          // The module looks for a hat-class step here; class-free, the
          // first candidate wins.
          const int s = (int)(fr.Uniform() * (float)nSteps);
          src[pos] = (int8_t)s;
          len[pos] = 1;
          rev[pos] = fr.Uniform() < 0.10f;
          prevSrc = s;
          pos += 1;
          continue;
        }
        src[pos] = kRest;
        len[pos] = (int8_t)cl;
        pos += cl;
        continue;
      }
      const float repShareE = repShare + clusterAmt * a * (1.f - repShare) * 0.5f;
      const float varP = 1.f - pInPlace;
      int s;
      if (fr.Uniform() >= varP) {
        s = pos;
      } else if (fr.Uniform() < repShareE) {
        s = prevSrc;   // say it again
      } else {
        s = pos;
        (void)fr.Uniform();   // the module's class-strictness draw
        for (int tries = 0; tries < 8; tries++) {
          const int cand = ((int)(fr.Uniform() * (float)nSteps) / cl) * cl;
          if (cand + cl > nSteps || cand == pos) continue;   // no no-op swaps
          s = cand;
          break;
        }
      }
      src[pos] = (int8_t)s;
      len[pos] = (int8_t)cl;
      rev[pos] = fr.Uniform() < pReverse;
      rat[pos] = clusterAmt > 0.01f && a > 0.5f && cl == 1 && fr.Uniform() < 0.5f;
      prevSrc = s;
      pos += cl;
    }
  }
};

}  // namespace chop
}  // namespace pd
