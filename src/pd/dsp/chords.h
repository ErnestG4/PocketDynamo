// Chord table + hysteretic selector, after Plaits' ChordBank and stmlib's
// HysteresisQuantizer2.
//
// Copyright 2021 Emilie Gillet (chord table and quantizer behaviour).
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions: The above
// copyright notice and this permission notice shall be included in all copies
// or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
// WITHOUT WARRANTY OF ANY KIND. See the LICENSE notice in Plaits for the full
// text: http://creativecommons.org/licenses/MIT/
//
// This adaptation: Copyright 2026 Combust, AGPL-3.0-or-later (the table
// and quantizer above keep their MIT notice).
#pragma once
#include "core.h"

namespace pd {

class ChordBank {
 public:
  static constexpr int kNumChords = 11;
  static constexpr int kNumNotes = 4;

  ChordBank() {
    for (int c = 0; c < kNumChords; ++c)
      for (int n = 0; n < kNumNotes; ++n)
        ratios_[c][n] = semitonesToRatio(kChords[c][n]);
  }

  // 0..1 control -> chord, with Plaits' 0.075-step hysteresis.
  void set(float parameter) {
    float v = parameter * 1.02f * kNumChords - 0.5f;
    const float sign = v > (float)index_ ? -1.0f : 1.0f;
    index_ = clamp((int)(v + sign * 0.075f + 0.5f), 0, kNumChords - 1);
  }

  const float* ratios() const { return ratios_[index_]; }
  int index() const { return index_; }

 private:
  // Plaits' default (non-JON) set: OCT 5 sus4 m m7 m9 m11 69 M9 M7 M.
  static constexpr float kChords[kNumChords][kNumNotes] = {
    {0.00f, 0.01f, 11.99f, 12.00f}, {0.00f, 7.00f, 7.01f, 12.00f},
    {0.00f, 5.00f, 7.00f, 12.00f},  {0.00f, 3.00f, 7.00f, 12.00f},
    {0.00f, 3.00f, 7.00f, 10.00f},  {0.00f, 3.00f, 10.00f, 14.00f},
    {0.00f, 3.00f, 10.00f, 17.00f}, {0.00f, 2.00f, 9.00f, 16.00f},
    {0.00f, 4.00f, 11.00f, 14.00f}, {0.00f, 4.00f, 7.00f, 11.00f},
    {0.00f, 4.00f, 7.00f, 12.00f},
  };
  float ratios_[kNumChords][kNumNotes];
  int index_ = 0;
};

}  // namespace pd
