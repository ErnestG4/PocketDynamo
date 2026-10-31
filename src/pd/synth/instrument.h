// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The instrument: a voice pool, each voice an engine plus an envelope, all
// sharing one set of controls. Pure C++ — the device render callback and the
// host tests drive the same object.
#pragma once
#include <new>

#include "../dsp/echo.h"
#include "../dsp/envelope.h"
#include "../dsp/fx.h"
#include "../engines/break.h"
#include "../engines/brocot_fm.h"
#include "../engines/cloud.h"
#include "../engines/modal.h"
#include "../engines/sampler.h"
#include "../engines/zx_phase.h"
#include "../engines/zx_pulse.h"
#include "events.h"

namespace pd {

enum class EngineId : uint8_t { ZxPulse, ZxPhase, BrocotFm, Modal, Sample, Cloud, Break, Count };

inline const char* engineName(EngineId e) {
  switch (e) {
    case EngineId::ZxPulse: return "ZX PULSE";
    case EngineId::ZxPhase: return "ZX PHASE";
    case EngineId::BrocotFm: return "BROCOT FM";
    case EngineId::Modal:   return "MODAL";
    case EngineId::Sample:  return "SAMPLE";
    case EngineId::Cloud:   return "CLOUD";
    case EngineId::Break:   return "BREAK";
    default: return "?";
  }
}

// Engines that play the recorded take (synth/sample_store.h).
inline bool playsTake(EngineId e) { return e == EngineId::Sample || e == EngineId::Cloud || e == EngineId::Break; }

// Amp release after key-up, per engine: buzzers stop like buzzers, a stab
// gates with the key, a cloud thins out.
inline float releaseMs(EngineId e) {
  switch (e) {
    case EngineId::BrocotFm: return 350.0f;
    case EngineId::Modal:  return ModalEngine::kReleaseMs;   // a struck bell rings out
    case EngineId::Sample: return 60.0f;
    case EngineId::Cloud:  return 500.0f;
    case EngineId::Break:  return 80.0f;
    default:               return 250.0f;
  }
}

// Room for the largest engine: a voice holds only the one it plays.
template <typename... Ts> struct EngineStore {
  static constexpr size_t kSize = std::max({sizeof(Ts)...});
  static constexpr size_t kAlign = std::max({alignof(Ts)...});
};
using AnyEngine = EngineStore<ZxPulseEngine, ZxPhaseEngine, BrocotFmEngine, ModalEngine,
                              SamplerEngine, CloudEngine, BreakEngine>;

template <int kVoices, size_t kBlock>
class Instrument {
 public:
  Instrument() {
    // Shared tables are built here, never in the render task on a voice's
    // first note of an engine.
    (void)modal::tables();
    (void)sineTable();
    for (auto& v : voices_) {
      v.build(EngineId::ZxPulse);
      v.env.setTimes(2.0f, releaseMs(EngineId::ZxPulse), kBlock);
    }
    for (int i = 0; i < (int)Param::Count; ++i) params_[i] = 0.5f;
    params_[(int)Param::Harmonics] = 0.0f;   // single pin per key: beeper poly
    params_[(int)Param::Morph] = 0.0f;
    params_[(int)Param::Timbre] = 0.75f;     // ~15-sample pins: one key has body
  }

  // Handles one event and returns what it costs the render as strikes (0 for
  // anything but a NoteOn): the render task budgets them per block.
  int handle(const Event& e) {
    // Every event's value is a float that some engine turns into an index or
    // a phase step; a NaN or inf one is dropped here, at the boundary.
    if (!isFinite(e.value)) return 0;
    switch (e.type) {
      case EventType::NoteOn:    return noteOn(e.a, e.b, e.value);
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
      case EventType::Tempo:
        tempo_ = clamp(e.value, 20.0f, 400.0f);
        if (fx_) fx_->setTempo(tempo_);
        break;
      case EventType::Fx:
        if (e.a < MasterFx::kSlots) { if (fx_) fx_->set(e.a, e.b, e.value); }
        else if (echo_) echo_->setSpace(e.value);
        break;
      case EventType::Bend:
        bend_ = clamp(e.value, -24.0f, 24.0f);
        break;
      case EventType::Echo:
        if (!echo_) break;
        if (e.a == 0) echo_->setTime(e.value);
        else if (e.a == 1) echo_->setFeedback(e.value);
        else if (e.a == 2) echo_->setMix(e.value);
        break;
      case EventType::AllOff:
        for (auto& v : voices_) { v.gated = false; v.env.gateOff(); }
        break;
    }
    return 0;
  }

  // Render one block of mono output.
  PD_HOT void render(float* out) {
    for (size_t i = 0; i < kBlock; ++i) out[i] = 0.0f;
    float main[kBlock], aux[kBlock];
    int sounding = 0;
    for (const auto& v : voices_) sounding += v.env.active();
    int active = 0;   // published once, at the end: the UI core reads it
    for (auto& v : voices_) {
      if (!v.env.active()) continue;
      ++active;
      EngineParams p;
      p.note = v.note + bend_;
      p.harmonics = params_[(int)Param::Harmonics];
      p.timbre = params_[(int)Param::Timbre];
      p.morph = params_[(int)Param::Morph];
      p.macro = params_[(int)Param::Macro];
      p.tempo = tempo_;
      p.voices = (uint8_t)sounding;
      p.trigger = v.trigger ? kTriggerRising : 0;
      v.trigger = false;
      Engine& eng = v.engineFor();
      eng.render(p, main, aux, kBlock);
      // A voice that went NaN or infinite is dropped and its engine rebuilt,
      // so one bad float can't poison the bus and the effects' state. NaN in
      // an engine's state carries to the end of its block, so the last sample
      // tells; anything shorter-lived is caught on the bus below.
      if (!isFinite(main[kBlock - 1])) {
        v.gated = false;
        v.env.kill();
        v.build(v.engine);
        --active;
        faults_ = faults_ + 1;
        continue;
      }
      float g0, g1;
      v.env.advance(&g0, &g1);
      g0 *= v.gain; g1 *= v.gain;
      const float dg = (g1 - g0) * (1.0f / kBlock);
      for (size_t i = 0; i < kBlock; ++i) { out[i] += main[i] * g0; g0 += dg; }
    }
    active_ = active;
    if (fx_) fx_->process(out, kBlock);
    if (echo_) echo_->process(out, kBlock);
    // The same guard on the bus: the effects start clean, and the block is
    // silence instead of noise.
    float bus = 0.0f;
    for (size_t i = 0; i < kBlock; ++i) bus += out[i];
    if (!isFinite(bus)) {
      if (fx_) fx_->reset();
      if (echo_) echo_->reset();
      for (size_t i = 0; i < kBlock; ++i) out[i] = 0.0f;
      faults_ = faults_ + 1;
    }
    // Soft clip: cubic, unity slope at 0, reaching +-1 with zero slope at +-1.5.
    for (size_t i = 0; i < kBlock; ++i) {
      float x = clamp(out[i] * master_, -1.5f, 1.5f);
      out[i] = x - (4.0f / 27.0f) * x * x * x;
    }
  }

  // The master-bus echo, owned outside the pool (a bench pool has none).
  void setEcho(TapeEcho* echo) { echo_ = echo; }
  void setFx(MasterFx* fx) { fx_ = fx; }
  float param(Param p) const { return params_[(int)p]; }
  EngineId engine() const { return engine_; }
  int activeVoices() const { return active_; }
  // Blocks or voices the NaN/inf guards have caught since boot (never, unless
  // something is wrong).
  uint32_t faults() const { return faults_; }

 private:
  // One engine per voice, built in place when the voice changes engine, so
  // the pool costs the largest engine per voice rather than all seven. An
  // engine keeps its state for as long as its voice keeps playing it.
  struct Voice {
    alignas(AnyEngine::kAlign) unsigned char store[AnyEngine::kSize];
    Engine* eng = nullptr;
    Envelope env;
    EngineId engine = EngineId::ZxPulse;
    float note = 60.0f;
    uint8_t tag = 0;
    bool gated = false;
    bool trigger = false;
    float gain = 1.0f;
    uint32_t age = 0;

    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    ~Voice() { if (eng) eng->~Engine(); }

    Engine& engineFor() { return *eng; }
    void build(EngineId id) {
      if (eng) eng->~Engine();
      switch (id) {
        case EngineId::ZxPhase:  eng = new (store) ZxPhaseEngine(); break;
        case EngineId::BrocotFm: eng = new (store) BrocotFmEngine(); break;
        case EngineId::Modal:    eng = new (store) ModalEngine(); break;
        case EngineId::Sample:   eng = new (store) SamplerEngine(); break;
        case EngineId::Cloud:    eng = new (store) CloudEngine(); break;
        case EngineId::Break:    eng = new (store) BreakEngine(); break;
        default:                 eng = new (store) ZxPulseEngine(); id = EngineId::ZxPulse; break;
      }
      engine = id;
    }
  };

  // A strike costs 1; a MODAL strike builds a resonator bank (+1), and a
  // voice that has to build a new engine pays its cold first block (+1).
  int noteOn(uint8_t tag, uint8_t vel, float note) {
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
    const bool rebuilt = v->engine != engine_;
    if (rebuilt) v->build(engine_);
    // A modal bank left ringing when its voice went idle would come back as
    // a faint tail on the voice's next note: start it clean.
    else if (engine_ == EngineId::Modal && !v->env.active()) v->eng->reset();
    v->gain = 0.35f + 0.65f * (float)vel / 127.0f;
    v->age = ++clock_;
    v->env.gateOn();
    return 1 + (engine_ == EngineId::Modal) + rebuilt;
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
  float tempo_ = 120.0f;
  float bend_ = 0.0f;
  TapeEcho* echo_ = nullptr;
  MasterFx* fx_ = nullptr;
  uint32_t clock_ = 0;
  volatile int active_ = 0;
  volatile uint32_t faults_ = 0;
};

}  // namespace pd
