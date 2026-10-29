// The instrument: a voice pool, each voice an engine plus an envelope, all
// sharing one set of controls. Pure C++ — the device render callback and the
// host tests drive the same object.
#pragma once
#include "../dsp/envelope.h"
#include "../engines/brocot_fm.h"
#include "../engines/cloud.h"
#include "../engines/sampler.h"
#include "../engines/zx_phase.h"
#include "../engines/zx_pulse.h"
#include "events.h"

namespace pd {

enum class EngineId : uint8_t { ZxPulse, ZxPhase, BrocotFm, Sample, Cloud, Count };

inline const char* engineName(EngineId e) {
  switch (e) {
    case EngineId::ZxPulse: return "ZX PULSE";
    case EngineId::ZxPhase: return "ZX PHASE";
    case EngineId::BrocotFm: return "BROCOT FM";
    case EngineId::Sample:  return "SAMPLE";
    case EngineId::Cloud:   return "CLOUD";
    default: return "?";
  }
}

// Engines that play the recorded take (synth/sample_store.h).
inline bool playsTake(EngineId e) { return e == EngineId::Sample || e == EngineId::Cloud; }

// Amp release after key-up, per engine: buzzers stop like buzzers, a stab
// gates with the key, a cloud thins out.
inline float releaseMs(EngineId e) {
  switch (e) {
    case EngineId::BrocotFm: return 350.0f;
    case EngineId::Sample: return 60.0f;
    case EngineId::Cloud:  return 500.0f;
    default:               return 250.0f;
  }
}

template <int kVoices, size_t kBlock>
class Instrument {
 public:
  Instrument() {
    for (auto& v : voices_) v.env.setTimes(2.0f, releaseMs(EngineId::ZxPulse), kBlock);
    for (int i = 0; i < (int)Param::Count; ++i) params_[i] = 0.5f;
    params_[(int)Param::Harmonics] = 0.0f;   // single pin per key: Follin poly
    params_[(int)Param::Morph] = 0.0f;
    params_[(int)Param::Timbre] = 0.75f;     // ~15-sample pins: one key has body
  }

  void handle(const Event& e) {
    switch (e.type) {
      case EventType::NoteOn:    noteOn(e.a, e.b, e.value); break;
      case EventType::NoteOff:   noteOff(e.a); break;
      case EventType::Retrigger: retrigger(e.a, e.value); break;
      case EventType::Pitch:
        for (auto& v : voices_) if (v.gated && v.tag == e.a) v.note = e.value;
        break;
      case EventType::Param:
        if (e.a < (uint8_t)Param::Count) params_[e.a] = clamp(e.value, 0.0f, 1.0f);
        break;
      case EventType::Engine:
        if (e.a < (uint8_t)EngineId::Count) engine_ = (EngineId)e.a;
        break;
      case EventType::AllOff:
        for (auto& v : voices_) { v.gated = false; v.env.gateOff(); }
        break;
    }
  }

  // Render one block of mono output.
  void render(float* out) {
    for (size_t i = 0; i < kBlock; ++i) out[i] = 0.0f;
    float main[kBlock], aux[kBlock];
    active_ = 0;
    for (auto& v : voices_) {
      if (!v.env.active()) continue;
      ++active_;
      EngineParams p;
      p.note = v.note;
      p.harmonics = params_[(int)Param::Harmonics];
      p.timbre = params_[(int)Param::Timbre];
      p.morph = params_[(int)Param::Morph];
      p.macro = params_[(int)Param::Macro];
      p.trigger = v.trigger ? kTriggerRising : 0;
      v.trigger = false;
      Engine& eng = v.engineFor();
      eng.render(p, main, aux, kBlock);
      float g0, g1;
      v.env.advance(&g0, &g1);
      g0 *= v.gain; g1 *= v.gain;
      const float dg = (g1 - g0) * (1.0f / kBlock);
      for (size_t i = 0; i < kBlock; ++i) { out[i] += main[i] * g0; g0 += dg; }
    }
    // Soft clip: cubic, unity slope at 0, reaching +-1 with zero slope at +-1.5.
    for (size_t i = 0; i < kBlock; ++i) {
      float x = clamp(out[i] * master_, -1.5f, 1.5f);
      out[i] = x - (4.0f / 27.0f) * x * x * x;
    }
  }

  float param(Param p) const { return params_[(int)p]; }
  EngineId engine() const { return engine_; }
  int activeVoices() const { return active_; }

 private:
  struct Voice {
    ZxPulseEngine pulse;
    ZxPhaseEngine phase;
    BrocotFmEngine fm;
    SamplerEngine sample;
    CloudEngine cloud;
    Envelope env;
    EngineId engine = EngineId::ZxPulse;
    float note = 60.0f;
    uint8_t tag = 0;
    bool gated = false;
    bool trigger = false;
    float gain = 1.0f;
    uint32_t age = 0;

    Engine& engineFor() {
      switch (engine) {
        case EngineId::ZxPhase: return phase;
        case EngineId::BrocotFm: return fm;
        case EngineId::Sample:  return sample;
        case EngineId::Cloud:   return cloud;
        default:                return pulse;
      }
    }
  };

  void noteOn(uint8_t tag, uint8_t vel, float note) {
    Voice* v = nullptr;
    for (auto& c : voices_) if (!c.env.active()) { v = &c; break; }   // free
    if (!v) for (auto& c : voices_)                                    // oldest released
      if (c.env.released() && (!v || c.age < v->age)) v = &c;
    if (!v) for (auto& c : voices_) if (!v || c.age < v->age) v = &c;  // oldest
    v->note = note;
    v->tag = tag;
    v->gated = true;
    v->trigger = true;
    if (v->engine != engine_ || !v->env.active()) v->env.setTimes(2.0f, releaseMs(engine_), kBlock);
    v->engine = engine_;
    v->gain = 0.35f + 0.65f * (float)vel / 127.0f;
    v->age = ++clock_;
    v->env.gateOn();
  }

  void noteOff(uint8_t tag) {
    for (auto& v : voices_)
      if (v.gated && v.tag == tag) { v.gated = false; v.env.gateOff(); }
  }

  void retrigger(uint8_t tag, float decayMs) {
    for (auto& v : voices_)
      if (v.gated && v.tag == tag) { v.env.retrigger(decayMs); v.trigger = true; }
  }

  Voice voices_[kVoices];
  float params_[(int)Param::Count];
  EngineId engine_ = EngineId::ZxPulse;
  float master_ = 1.0f;
  uint32_t clock_ = 0;
  int active_ = 0;
};

}  // namespace pd
