// BREAK: the take as a breakbeat, cut up live by Audiothurgist's Classic
// chop grammar (synth/chop.h) on a 16th grid at the instrument's tempo.
// After Audiothurgist's varispeed slice voice (Combust): the take is one bar, every step plays at
// rate = step material / step time, so tempo is pitch, the record way; the
// key adds its pitch on top (C4 = none). A cell that continues where the last
// one ended keeps reading (verbatim runs stay one voice); every real cut
// bridges its jump with a decaying offset; cells play on past their
// material, like a record (Zoom).
//   HARMONICS  chop: 0 verbatim -> DJ edit -> chops -> rolls -> micro bursts
//   TIMBRE     which pattern: 32 seeded arrangements of the same chop
//   MORPH      gate: each cell sounds for 15% .. 100% of its slot
//   MACRO      tape-dull (CCW) / SP grit (CW), as SAMPLE
// A trigger (key strike, ratchet) restarts the bar at step 1.
// out: the break; aux: the same before MACRO.
#pragma once
#include "../dsp/core.h"
#include "../dsp/lofi.h"
#include "../dsp/rng.h"
#include "../synth/chop.h"
#include "../synth/sample_store.h"

namespace pd {

class BreakEngine : public Engine {
 public:
  static constexpr int kSteps = 16, kStepsPerBeat = 4;

  void reset() override {
    step_ = 0; stepPhase_ = 0.0f; cellAge_ = 0.0f; cellLenOut_ = 1.0f;
    pos_ = 0; dir_ = 1; level_ = 0.0f; resting_ = true; ratDone_ = true;
    comp_ = 0.0f; last_ = 0.0f;
    chopQ_ = -1; seedQ_ = -1;
    lofi_.reset();
  }

  void render(const EngineParams& p, float* out, float* aux, size_t n) override {
    const SampleStore& s = sampleStore();
    if (!s.ready() || s.length() < 2 * kSteps) {
      for (size_t i = 0; i < n; ++i) out[i] = aux[i] = 0.0f;
      return;
    }
    // Re-cut when the chop or pattern control crosses a step.
    const int chopQ = (int)(clamp(p.harmonics, 0.0f, 1.0f) * 32.0f + 0.5f);
    const int seedQ = std::min(31, (int)(clamp(p.timbre, 0.0f, 1.0f) * 32.0f));
    if (chopQ != chopQ_ || seedQ != seedQ_) {
      chopQ_ = chopQ; seedQ_ = seedQ;
      pattern_.Generate(kSteps, kStepsPerBeat, 1.0f - (float)chopQ / 32.0f,
                        rng::hash32(0xb4ea0000u + seedQ), rng::hash32(0xf111u + 977u * seedQ));
    }

    const uint32_t len = s.length();
    const float stepSrc = (float)len / kSteps;                       // take samples a step
    const float stepOut = 60.0f * kSampleRate / (clamp(p.tempo, 20.0f, 400.0f) * kStepsPerBeat);
    const float rate = stepSrc / stepOut * semitonesToRatio(p.note - 60.0f);
    const int64_t inc = (int64_t)(rate * 4294967296.0f);
    const float gate = 0.15f + 0.85f * clamp(p.morph, 0.0f, 1.0f);
    lofi_.set(p.macro);

    if (p.trigger & kTriggerRising) { step_ = 0; stepPhase_ = 0.0f; startCell(s, stepSrc, stepOut, true); }

    const int16_t* d = s.data();
    const float scale = s.gain() * (1.0f / 32768.0f) * kLevel;
    const int64_t maxPos = (int64_t)(len - 2) << 32;
    constexpr float kFadeRecip = 1.0f / (0.003f * kSampleRate);   // 3 ms
    for (size_t i = 0; i < n; ++i) {
      if (stepPhase_ >= stepOut) {
        stepPhase_ -= stepOut;
        step_ = (step_ + 1) % kSteps;
        if (pattern_.src[step_] != chop::Pattern::kInterior) startCell(s, stepSrc, stepOut, false);
      }
      // 32nd double: one-step cells flagged rat re-strike at half a step.
      if (!ratDone_ && cellAge_ >= 0.5f * stepOut) { ratDone_ = true; jump(s, cellStart_); }

      // Gate: full inside its share of the cell, then a 3 ms fade.
      const float target = resting_ || cellAge_ > gate * cellLenOut_ ? 0.0f : 1.0f;
      level_ += target > level_ ? std::min(kFadeRecip, target - level_) : std::max(-kFadeRecip, target - level_);

      float x = 0.0f;
      if (pos_ >= 0 && pos_ <= maxPos) {
        const uint32_t idx = (uint32_t)(pos_ >> 32);
        const float a = (float)d[idx], b = (float)d[idx + 1];
        x = (a + (float)((uint32_t)pos_ >> 8) * (1.0f / 16777216.0f) * (b - a)) * scale;
      }
      x = clamp(x * level_ + comp_, -1.0f, 1.0f);
      comp_ *= 0.93f;
      last_ = x;
      aux[i] = x;
      out[i] = lofi_.process(x);
      pos_ += dir_ * inc;
      stepPhase_ += 1.0f;
      cellAge_ += 1.0f;
    }
  }

 private:
  static constexpr float kLevel = 0.6f;   // the take is normalized to 0.9

  void startCell(const SampleStore& s, float stepSrc, float stepOut, bool fresh) {
    const int8_t src = pattern_.src[step_];
    cellAge_ = 0.0f;
    cellLenOut_ = stepOut * (float)std::max<int8_t>(1, pattern_.len[step_]);
    ratDone_ = !(src >= 0 && pattern_.rat[step_]);
    if (src == chop::Pattern::kRest) { resting_ = true; return; }
    resting_ = false;
    const bool rev = pattern_.rev[step_];
    const float startF = rev ? ((float)(src + pattern_.len[step_]) * stepSrc - 1.0f) : (float)src * stepSrc;
    const uint32_t whole = (uint32_t)startF;
    const int64_t at = ((int64_t)whole << 32) | (uint32_t)((startF - (float)whole) * 4294967296.0f);
    cellStart_ = at;
    // Verbatim continuation: same direction and the head is already there
    // (within a quarter step) — keep reading, no cut.
    const int newDir = rev ? -1 : 1;
    const int64_t near = (int64_t)(0.25f * stepSrc) << 32;
    const int64_t gap = at - pos_;
    if (!fresh && newDir == dir_ && gap < near && gap > -near) return;
    dir_ = newDir;
    jump(s, at);
  }

  void jump(const SampleStore& s, int64_t to) {
    const float next = s.read((uint32_t)(to >> 32), (float)(uint32_t)to * (1.0f / 4294967296.0f)) * kLevel;
    comp_ = last_ - next * level_;
    pos_ = to;
  }

  chop::Pattern pattern_;
  int chopQ_ = -1, seedQ_ = -1;
  int step_ = 0;
  float stepPhase_ = 0.0f;   // output samples into the current step
  float cellAge_ = 0.0f;     // output samples into the current cell
  float cellLenOut_ = 1.0f;
  int64_t pos_ = 0, cellStart_ = 0;   // 32.32 in take samples
  int dir_ = 1;
  float level_ = 0.0f;
  bool resting_ = true, ratDone_ = true;
  float comp_ = 0.0f, last_ = 0.0f;
  Lofi lofi_;
};

}  // namespace pd
