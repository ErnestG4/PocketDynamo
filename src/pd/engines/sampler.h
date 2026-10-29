// SAMPLE: the recorded take played as a record — pitch is speed, no
// time-stretch, C4 plays it as recorded. After Audiothurgist's slice voice
// (Combust): varispeed
// with linear interpolation (it aliases a little pitched up, which is period-
// correct) and its click fabric — every jump holds the step it made as an
// offset that decays x0.93 a sample (~1 ms), and one-shots fade into their
// end over 3 ms of material.
//   HARMONICS  start: picks a slice when the take has onsets, else anywhere
//   TIMBRE     length from the start: 20 ms stabs .. the rest of the take
//   MORPH      play mode: forward, forward loop, ping-pong loop, reverse
//   MACRO      0.5 clean; down to tape-dull (one-pole lowpass to ~100 Hz);
//              up to SP grit (sample-and-hold decimation and bit crush)
// out: the voice; aux: the same before MACRO's colouring.
#pragma once
#include "../dsp/core.h"
#include "../synth/sample_store.h"

namespace pd {

class SamplerEngine : public Engine {
 public:
  enum Mode : uint8_t { OneShot, Loop, PingPong, Reverse };

  void reset() override {
    pos_ = 0; dir_ = 1; done_ = true;
    comp_ = 0.0f; last_ = 0.0f; lp_ = 0.0f; held_ = 0.0f; holdCount_ = 0;
  }

  void render(const EngineParams& p, float* out, float* aux, size_t n) override {
    const SampleStore& s = sampleStore();
    if (!s.ready()) {
      for (size_t i = 0; i < n; ++i) out[i] = aux[i] = 0.0f;
      done_ = true;
      return;
    }

    // Region, refreshed every block so the controls play live.
    const uint32_t len = s.length();
    uint32_t start;
    if (s.slices() > 1) start = s.sliceStart(std::min((int)(p.harmonics * s.slices()), s.slices() - 1));
    else start = (uint32_t)(clamp(p.harmonics, 0.0f, 1.0f) * 0.95f * (float)len);
    const float remaining = (float)(len - 1 - start);
    constexpr float kMinLen = 0.02f * SampleStore::kRate;
    const float span = remaining <= kMinLen ? remaining
        : kMinLen * std::pow(remaining / kMinLen, clamp(p.timbre, 0.0f, 1.0f));
    const uint32_t end = start + std::max<uint32_t>(2, (uint32_t)span);
    const Mode mode = (Mode)std::min(3, (int)(clamp(p.morph, 0.0f, 1.0f) * 4.0f));

    // Speed: C4 = as recorded; the take is at half the output rate.
    const float ratio = semitonesToRatio(p.note - 60.0f) * ((float)SampleStore::kRate / kSampleRate);
    const int64_t inc = (int64_t)(ratio * 4294967296.0f);
    const int64_t s0 = (int64_t)start << 32, s1 = (int64_t)end << 32;

    if (p.trigger & kTriggerRising) {
      jump(s, mode == Reverse ? s1 - (1ll << 32) : s0);
      dir_ = mode == Reverse ? -1 : 1;
      done_ = false;
    }

    constexpr float kFade = 0.003f * SampleStore::kRate;   // 3 ms of material
    const float fadeRecip = 1.0f / kFade;
    const Colour c = colour(p.macro);

    // The loop runs on locals; the region guarantees idx + 1 < length, so
    // samples are read raw and the take's gain is applied once at the end.
    const int16_t* d = s.data();
    const float scale = s.gain() * (1.0f / 32768.0f);
    const int64_t one = 1ll << 32;
    int64_t pos = pos_;
    int64_t step = dir_ > 0 ? inc : -inc;
    bool done = done_;
    float comp = comp_, last = last_;
    auto sampleAt = [d](int64_t at) {
      const uint32_t idx = (uint32_t)(at >> 32);
      const float a = (float)d[idx], b = (float)d[idx + 1];
      return a + (float)((uint32_t)at >> 8) * (1.0f / 16777216.0f) * (b - a);
    };
    for (size_t i = 0; i < n; ++i) {
      float x = 0.0f;
      if (!done) {
        // Region edges (the region can also move under a running voice).
        if (pos >= s1) {
          if (step < 0) pos = s1 - one;
          else if (mode == Loop) {
            pos -= s1 - s0;
            if (pos >= s1) pos = s0;
            comp = last - sampleAt(pos) * scale;   // bridge the seam
          } else if (mode == PingPong) { pos = std::max(s0, 2 * s1 - pos - inc); step = -inc; }
          else done = true;
        } else if (pos < s0) {
          if (step > 0) pos = s0;
          else if (mode == PingPong) { pos = std::min(s1 - one, 2 * s0 - pos + inc); step = inc; }
          else done = true;
        }
        if (!done) {
          x = sampleAt(pos) * scale;
          if (mode == OneShot || mode == Reverse) {
            const uint32_t idx = (uint32_t)(pos >> 32);
            const float toEdge = mode == OneShot ? (float)(end - idx) : (float)(idx - start);
            if (toEdge < kFade) x *= toEdge * fadeRecip;
          }
          pos += step;
        }
      }
      x = clamp(x + comp, -1.0f, 1.0f);   // a seam on a full-scale take can double the swing
      comp *= 0.93f;
      last = x;
      aux[i] = x * kLevel;
      out[i] = shade(x, c) * kLevel;
    }
    pos_ = pos; dir_ = step < 0 ? -1 : 1; done_ = done; comp_ = comp; last_ = last;
  }

 private:
  // The take is normalized to 0.9 peak; sit it with the synth engines.
  static constexpr float kLevel = 0.6f;

  struct Colour { float lpCoef; int hold; float crushQ; };   // crushQ: levels per unit

  static Colour colour(float macro) {
    Colour c = {1.0f, 1, 32768.0f};
    if (macro < 0.49f) {
      const float fc = 12000.0f * std::exp2(-(0.5f - macro) * 14.0f);   // 12 kHz .. ~94 Hz
      c.lpCoef = 1.0f - std::exp(-kTwoPi * fc / kSampleRate);
    } else if (macro > 0.51f) {
      const float g = (macro - 0.5f) * 2.0f;
      c.hold = 1 + (int)(g * 7.0f);                       // 48 kHz .. 6 kHz
      c.crushQ = std::exp2(15.0f - 11.0f * g);            // 16 bits .. 5 bits
    }
    return c;
  }

  float shade(float x, const Colour& c) {
    if (c.lpCoef < 1.0f) { lp_ += c.lpCoef * (x - lp_); return lp_; }
    if (c.hold > 1) {
      if (holdCount_ <= 0) {
        const float q = x * c.crushQ;
        held_ = (float)(int32_t)(q + (q >= 0.0f ? 0.5f : -0.5f)) / c.crushQ;
        holdCount_ = c.hold;
      }
      --holdCount_;
      return held_;
    }
    lp_ = x;
    return x;
  }

  // Move the read head, bridging the step with a decaying offset.
  void jump(const SampleStore& s, int64_t to) {
    const float next = s.read((uint32_t)(to >> 32), (float)(uint32_t)to * kPhaseToFloat);
    comp_ = done_ ? 0.0f : last_ - next;
    pos_ = to;
  }

  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kPhaseToFloat = 1.0f / 4294967296.0f;

  int64_t pos_ = 0;      // 32.32 fixed point, in take samples
  int dir_ = 1;
  bool done_ = true;
  float comp_ = 0.0f, last_ = 0.0f;
  float lp_ = 0.0f, held_ = 0.0f;
  int holdCount_ = 0;
};

}  // namespace pd
