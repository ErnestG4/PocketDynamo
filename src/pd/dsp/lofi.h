// The sampler engines' MACRO colour: 0.5 clean; down to tape-dull (one-pole
// lowpass, 12 kHz .. ~94 Hz); up to SP grit (sample-and-hold decimation,
// 48 .. 6 kHz, with a bit crush, 16 .. 5 bits). set() once per block,
// process() per sample.
#pragma once
#include <cmath>
#include <cstdint>

#include "core.h"

namespace pd {

class Lofi {
 public:
  void reset() { lp_ = 0.0f; held_ = 0.0f; count_ = 0; }

  void set(float macro) {
    if (macro == key_) return;   // libm only when MACRO moves
    key_ = macro;
    lpCoef_ = 1.0f; hold_ = 1; crushQ_ = 32768.0f;
    if (macro < 0.49f) {
      const float fc = 12000.0f * std::exp2(-(0.5f - macro) * 14.0f);
      lpCoef_ = 1.0f - std::exp(-6.28318530717958647692f * fc / kSampleRate);
    } else if (macro > 0.51f) {
      const float g = (macro - 0.5f) * 2.0f;
      hold_ = 1 + (int)(g * 7.0f);
      crushQ_ = std::exp2(15.0f - 11.0f * g);
    }
  }

  float process(float x) {
    if (lpCoef_ < 1.0f) { lp_ += lpCoef_ * (x - lp_); return lp_; }
    if (hold_ > 1) {
      if (count_ <= 0) {
        const float q = x * crushQ_;
        held_ = (float)(int32_t)(q + (q >= 0.0f ? 0.5f : -0.5f)) / crushQ_;
        count_ = hold_;
      }
      --count_;
      return held_;
    }
    lp_ = x;
    return x;
  }

 private:
  float key_ = -1.0f;   // the macro the settings below came from
  float lpCoef_ = 1.0f, crushQ_ = 32768.0f;
  int hold_ = 1, count_ = 0;
  float lp_ = 0.0f, held_ = 0.0f;
};

}  // namespace pd
