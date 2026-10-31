// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tilt -> controls, absolute: the device's attitude gives two readings,
// side-to-side and front/back, which drive whichever controls they're
// routed to (TIMBRE and MORPH unless rerouted). Accel axes as mounted on the Cardputer ADV (BMI270): flat on a
// table reads (0, 0, +1) g; tipping the left edge down drives x negative;
// standing it up facing you drives y to +1. Pure arithmetic — host-testable.
#pragma once
#include <cmath>

#include "../dsp/core.h"

namespace pd {
namespace tilt {

constexpr float kDegPerRad = 57.29578f;

// Side to side: on its left edge 0, flat 0.5, on its right edge 1. Taken
// against the whole gravity vector, so it reads the same flat or standing.
inline float side(float ax, float ay, float az) {
  const float g = std::sqrt(ax * ax + ay * ay + az * az);
  if (g < 0.2f) return 0.5f;   // free fall or a bad read: hold the middle
  const float roll = std::asin(clamp(ax / g, -1.0f, 1.0f)) * kDegPerRad;
  return clamp((roll + 90.0f) / 180.0f, 0.0f, 1.0f);
}

// Front/back, as one continuous rotation from flat: flat 0, standing up 0.8,
// tipped on past upright 1 (at 112.5 degrees). Tilting back below flat stays
// 0; upside down counts as past upright.
constexpr float kFrontStanding = 0.8f;
inline float front(float ax, float ay, float az) {
  (void)ax;
  float pitch = std::atan2(ay, az) * kDegPerRad;   // flat 0, standing 90
  if (pitch < -90.0f) pitch += 360.0f;
  return clamp(pitch * (kFrontStanding / 90.0f), 0.0f, 1.0f);
}

}  // namespace tilt
}  // namespace pd
