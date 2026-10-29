// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Transcendentals without libm, for control-rate work that runs on the audio
// core (a strike, a retune, a per-block coefficient update). Ported from
// Kyklophoria's core/kyk_resonate.h fastmath (Combust). The ESP32-S3 has a single-precision FPU and
// no fast libm: expf/sinf/cosf are a hundred-odd cycles each, and floorf is a
// call. These are polynomials on the float's own bits, accurate to a fraction
// of a cent where pitch is concerned.
#pragma once
#include <cstdint>
#include <cstring>

namespace pd {
namespace fastmath {

// 2^x for x in [-126, 127]: the integer part through the exponent bits, the
// fraction by seven terms of e^(f ln2), within 2e-8 over [0, 1).
inline float exp2(float x) {
  if (x < -126.0f) return 0.0f;
  if (x > 127.0f) x = 127.0f;
  int k = (int)x;                 // truncates toward zero; fix up to floor
  if ((float)k > x) --k;
  const float f = x - (float)k;
  const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f +
                  f * (0.00961813f + f * (0.00133336f + f * 0.00015403f)))));
  const uint32_t bits = (uint32_t)(127 + k) << 23;
  float s;
  std::memcpy(&s, &bits, sizeof s);
  return p * s;
}

// log2 of a positive, normal float: the exponent from its bits, the mantissa
// by a quintic on [1, 2) within 3e-5.
inline float log2(float x) {
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  const int e = (int)((u >> 23) & 255u) - 127;
  u = (u & 0x007FFFFFu) | 0x3F800000u;
  float m;
  std::memcpy(&m, &u, sizeof m);
  const float y = m - 1.0f;
  return (float)e + y * (1.441825795f + y * (-0.708682101f + y * (0.415421949f +
                         y * (-0.194422672f + y * 0.045885527f))));
}

// e^-x for 0 <= x <= 0.2 (a pole radius from a per-sample decay): the series
// to six terms, exact to ~1e-9 there; past it, exp2.
inline float expNegSmall(float x) {
  if (x > 0.2f) return exp2(-1.44269504f * x);
  return 1.0f - x * (1.0f - x * (0.5f - x * (0.16666667f - x * (0.041666667f - x * 0.0083333333f))));
}

// sin and cos of w, |w| <= pi: the sine to x^13, the cosine to x^14, within
// 1e-7 over the whole range (four terms each left the cosine a fortieth out
// at pi, which in a resonator's 2 r cos w is a pole outside the circle).
inline void sinCos(float w, float* s, float* c) {
  const float x2 = w * w;
  *s = w * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * (0.00019841270f -
            x2 * (0.0000027557319f - x2 * (2.5052108e-8f - x2 * 1.6059044e-10f))))));
  *c = 1.0f - x2 * (0.5f - x2 * (0.041666667f - x2 * (0.0013888889f - x2 * (0.0000248016f -
            x2 * (2.7557319e-7f - x2 * (2.0876757e-9f - x2 * 1.1470746e-11f))))));
}

// 1/x for a positive normal x, without a divide: the ESP32-S3's gcc lowers
// '/' to a __divsf3 call. A bit-trick seed and three Newton steps, within a
// few ulp.
inline float recip(float x) {
  uint32_t u;
  std::memcpy(&u, &x, sizeof u);
  u = 0x7EF311C3u - u;
  float y;
  std::memcpy(&y, &u, sizeof y);
  y = y * (2.0f - x * y);
  y = y * (2.0f - x * y);
  y = y * (2.0f - x * y);
  return y;
}

}  // namespace fastmath
}  // namespace pd
