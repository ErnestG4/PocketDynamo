// CLOUD: the recorded take as a granular cloud. Clouds-style windowed grains
// (the grain idea after Émilie Gillet's Clouds; this code is new) whose
// onsets come from Lenticular's ARS gap statistics (dsp/ars.h, Combust): the same grain rate can be machine-even, even-but-alive, a
// quasiperiodic motif or a free cloud. Each key strike replays the same
// realization — the same cloud every strike — so a held key is a riff.
//   HARMONICS  where in the take the grains read from
//   TIMBRE     grain size, 15 ms .. 400 ms
//   MORPH      timing character, the ARS ladder: CHIP lattice, SILK
//              (hyperuniform), MOTIF (three-distance), LOOSE (Poisson),
//              CLUMP (clustered); position within each fifth = its character
//   MACRO      density, 2 .. 120 grains a second
// In CHIP and MOTIF each grain takes its chord tone from the gap statistics
// (Lenticular's gap melody): root, fifth, octave, octave + fifth — a strict
// arpeggio in CHIP, a self-similar melody in MOTIF.
// Pitch: grains play at the key's pitch, C4 = as recorded.
// out: the cloud; aux: the cloud without the chord tones (all at the root).
#pragma once
#include "../dsp/ars.h"
#include "../dsp/core.h"
#include "../synth/sample_store.h"

namespace pd {

class CloudEngine : public Engine {
 public:
  CloudEngine() { gaps_.Init(0x5eed1234u); }

  void reset() override {
    for (auto& g : grains_) g.on = false;
    countdown_ = 0.0f;
    gain_ = 0.0f;
    gaps_.Replay();
  }

  PD_HOT void render(const EngineParams& p, float* out, float* aux, size_t n) override {
    for (size_t i = 0; i < n; ++i) out[i] = aux[i] = 0.0f;
    const SampleStore& s = sampleStore();
    if (!s.ready()) { reset(); return; }

    // Per-block settings.
    const float sizeOut = 0.015f * kSampleRate * std::pow(400.0f / 15.0f, clamp(p.timbre, 0.0f, 1.0f));
    const float rate = 2.0f * std::pow(60.0f, clamp(p.macro, 0.0f, 1.0f));   // grains/s
    const float meanGap = kSampleRate / rate;
    const float m = clamp(p.morph, 0.0f, 0.9999f) * 5.0f;
    static constexpr int kZones[5] = {0, 1, 2, 5, 6};
    const int band = (int)m;
    const int zone = kZones[band];
    const float character = m - (float)band;
    const bool melodic = zone == 0 || zone == 2;
    const float root = semitonesToRatio(p.note - 60.0f) * ((float)SampleStore::kRate / kSampleRate);
    const float centre = clamp(p.harmonics, 0.0f, 1.0f);

    if (p.trigger & kTriggerRising) { gaps_.Replay(); countdown_ = 0.0f; }

    // Grain budget: the instrument's voices share kGrainBudget grains, so a
    // dense cloud on many voices thins instead of overrunning the core (64
    // full-size grains need ~115 % of it). Running grains finish; only new
    // ones wait.
    const int cap = clamp(kGrainBudget / std::max(1, (int)p.voices), 2, kGrains);
    int live = 0;
    for (const auto& g : grains_) live += g.on;
    for (size_t i = 0; i < n; ++i) {
      countdown_ -= 1.0f;
      if (countdown_ <= 0.0f) {
        countdown_ += gaps_.NextGap(zone, character, meanGap);
        if (live < cap && spawn(s, sizeOut, root, centre, melodic)) ++live;
      }
    }

    // Normalize by the overlap — expected from density, or actual when the
    // statistics clump — so density doesn't swing the level.
    // (Expected overlap can't exceed the budget's cap, or a capped cloud
    // would also go quiet.)
    int active = 0;
    for (const auto& g : grains_) active += g.on;
    const float expected = std::min(sizeOut / meanGap, (float)cap);
    const float overlap = std::max(1.0f, std::max(expected, (float)active));
    const float target = 1.6f / std::sqrt(overlap);
    // Fall fast (a clump arriving), rise slowly (it thinning out).
    const float g0 = gain_, g1 = gain_ = gain_ + (target < gain_ ? 0.7f : 0.05f) * (target - gain_);
    const float dg = (g1 - g0) / (float)n;
    // Render grains a block at a time, one tight loop per grain on locals.
    // Root-pitched grains accumulate in aux, the chord tones in out; out
    // gets aux added at the end. Spawns land at the block start (0.67 ms of
    // jitter). spawn() keeps every grain's whole read inside the take, so
    // the loop reads raw samples with no bounds check.
    for (auto& g : grains_) {
      if (!g.on) continue;
      float* dst = g.root ? aux : out;
      uint32_t idx = g.idx, frac = g.frac;
      const uint32_t incI = g.incI, incF = g.incF;
      float ph = g.phase;
      const float dph = g.phaseInc;
      for (size_t i = 0; i < n; ++i) {
        // Window: (4p(1-p))^2, a smooth bump, zero with zero slope at both ends.
        const float q = 4.0f * ph * (1.0f - ph);
        const float a = (float)s.at(idx), b = (float)s.at(idx + 1);
        dst[i] += q * q * (a + (float)(frac >> 8) * (1.0f / 16777216.0f) * (b - a));
        const uint32_t nf = frac + incF;
        idx += incI + (nf < frac);
        frac = nf;
        ph += dph;
        if (ph >= 1.0f) { g.on = false; break; }
      }
      g.idx = idx; g.frac = frac; g.phase = ph;
    }
    // Backstop for in-phase pile-ups: the instrument's cubic, unity at 0,
    // flat at +-1.5 -> +-1.
    auto soft = [](float x) { x = clamp(x, -1.5f, 1.5f); return x - (4.0f / 27.0f) * x * x * x; };
    const float scale = s.gain() * (1.0f / 32768.0f);
    float gg = g0 * scale;
    const float dgg = dg * scale;
    for (size_t i = 0; i < n; ++i) {
      out[i] = soft((out[i] + aux[i]) * gg);
      aux[i] = soft(aux[i] * gg);
      gg += dgg;
    }
  }

 private:
  static constexpr int kGrains = 8;
  static constexpr int kGrainBudget = 36;   // across the instrument: ~1.7 % of the core each
  struct Grain {
    bool on = false;
    bool root = true;
    uint32_t idx = 0, frac = 0;     // read head in take samples, 32-bit fraction
    uint32_t incI = 0, incF = 0;    // per output sample
    float phase = 0.0f, phaseInc = 0.0f;   // through the window, 0..1
  };

  bool spawn(const SampleStore& s, float sizeOut, float root, float centre, bool melodic) {
    Grain* g = nullptr;
    for (auto& c : grains_) if (!c.on) { g = &c; break; }
    if (!g) return false;   // pool full: the cloud thins rather than steals
    static constexpr float kTone[4] = {0.0f, 7.0f, 12.0f, 19.0f};
    const int tone = melodic ? std::min(3, (int)(gaps_.last_voice_u() * 4.0f)) : 0;
    const float ratio = root * semitonesToRatio(kTone[tone]);
    const float srcLen = sizeOut * ratio;
    const float len = (float)s.length();
    if (srcLen >= len - 4.0f * ratio - 8.0f) return false;
    // A little positional scatter (+-10 ms) so a frozen cloud still breathes.
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    const float jitter = ((float)(rng_ >> 8) * (1.0f / 16777216.0f) - 0.5f) * 0.02f * SampleStore::kRate;
    // Margin: the window can run a few samples long from float rounding.
    const float at = clamp(centre * (len - srcLen) + jitter, 0.0f, len - srcLen - 4.0f * ratio - 4.0f);
    g->on = true;
    g->root = tone == 0;
    g->idx = (uint32_t)at;
    g->frac = (uint32_t)((at - (float)g->idx) * 4294967296.0f);
    g->incI = (uint32_t)ratio;
    g->incF = (uint32_t)((ratio - (float)g->incI) * 4294967296.0f);
    g->phase = 0.0f;
    g->phaseInc = 1.0f / sizeOut;
    return true;
  }

  Grain grains_[kGrains];
  ArsGapGenerator gaps_;
  float countdown_ = 0.0f;
  float gain_ = 0.0f;
  uint32_t rng_ = 0x9e3779b9u;
};

}  // namespace pd
