// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Every control the player can move, in one table: its name (on screen, in
// patches' tilt and LFO routing), the page bar that shows it, the CCs that
// move it, and whether tilt and the LFO can drive it. A new control is a new
// row here. Patches keep their own keys (ui/patch.h): they're a file format.
// Pure — host-testable (ui/controls_test.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../dsp/lfo.h"

namespace pd {
namespace controls {

// The first sixteen are the pages' bars, in page order (four per page), so
// the page/bar <-> control mapping is arithmetic.
enum class Ctl : uint8_t {
  Harmonics, Timbre, Morph, Macro,          // SOUND, in pd::Param order
  Tone, Grit, Shape, Space,                 // FX
  Attack, Decay, Sustain, Release,          // ENV
  LfoRate, LfoDepth, LfoShape, LfoDelay,    // LFO
  EchoFb, EchoMix, EchoTime, Volume,        // no bar: FN keys and CCs
  Pitch,                                    // the LFO's alone
  Count,
};
constexpr int kCount = (int)Ctl::Count;
constexpr Ctl kNone = Ctl::Count;   // routed nowhere

enum : uint8_t { kTilt = 1, kLfo = 2 };   // what can drive a control
struct Info { const char* name; uint8_t drivenBy; LfoDest lfo; };
constexpr Info kInfo[kCount] = {
    {"HARMONICS", kTilt | kLfo, LfoDest::Harmonics}, {"TIMBRE", kTilt | kLfo, LfoDest::Timbre},
    {"MORPH", kTilt | kLfo, LfoDest::Morph},         {"MACRO", kTilt | kLfo, LfoDest::Macro},
    {"TONE", kTilt | kLfo, LfoDest::Tone},           {"GRIT", kTilt | kLfo, LfoDest::Grit},
    {"SHAPE", kTilt | kLfo, LfoDest::Shape},         {"SPACE", kTilt | kLfo, LfoDest::Space},
    {"ATTACK", 0, LfoDest::None},    {"DECAY", 0, LfoDest::None},
    {"SUSTAIN", 0, LfoDest::None},   {"RELEASE", 0, LfoDest::None},
    {"LFO RATE", 0, LfoDest::None},  {"LFO DEPTH", 0, LfoDest::None},
    {"LFO SHAPE", 0, LfoDest::None}, {"LFO DELAY", 0, LfoDest::None},
    {"ECHO FB", kTilt | kLfo, LfoDest::EchoFb},      {"ECHO MIX", kTilt | kLfo, LfoDest::EchoMix},
    {"ECHO TIME", 0, LfoDest::None}, {"VOLUME", 0, LfoDest::None},
    {"PITCH", kLfo, LfoDest::Pitch},
};
inline const Info& info(Ctl c) { return kInfo[(int)c]; }
inline const char* name(Ctl c) { return c == kNone ? "NONE" : kInfo[(int)c].name; }
inline bool drivenBy(Ctl c, uint8_t who) { return c != kNone && (kInfo[(int)c].drivenBy & who); }

// The control named `n` that `who` can drive, or kNone.
inline Ctl byName(const char* n, uint8_t who) {
  for (int i = 0; i < kCount; ++i)
    if ((kInfo[i].drivenBy & who) && !strcmp(n, kInfo[i].name)) return (Ctl)i;
  return kNone;
}

enum Page : uint8_t { kSoundPage, kFxPage, kEnvPage, kLfoPage, kPages };
constexpr const char* kPageNames[kPages] = {"SOUND", "FX", "ENV", "LFO"};
constexpr int kBars = 4;
inline Ctl pageBar(int page, int bar) { return (Ctl)(page * kBars + bar); }
inline int pageOf(Ctl c) { return (int)c < kPages * kBars ? (int)c / kBars : -1; }

// The fixed CC map. 16..31 are the sixteen bars in page order (16-19 SOUND,
// 20-23 FX, 24-27 ENV, 28-31 LFO); the common performance and General MIDI 2
// sound controllers land where they'd be expected.
inline Ctl ccControl(uint8_t cc) {
  if (cc >= 16 && cc <= 31) return (Ctl)(cc - 16);
  switch (cc) {
    case 71: return Ctl::Harmonics;   // resonance
    case 74: return Ctl::Timbre;      // brightness
    case 2:  return Ctl::Macro;       // breath
    case 73: return Ctl::Attack;
    case 75: return Ctl::Decay;
    case 72: return Ctl::Release;
    case 76: return Ctl::LfoRate;     // vibrato rate
    case 77: case 1: return Ctl::LfoDepth;   // vibrato depth, the mod wheel
    case 78: return Ctl::LfoDelay;    // vibrato delay
    case 7:  return Ctl::Volume;
    case 12: return Ctl::EchoFb;      // effect control 1
    case 13: return Ctl::EchoTime;    // effect control 2
    case 91: return Ctl::EchoMix;     // the usual effects send
    default: return kNone;
  }
}

// Page knobs: four CCs that move the four bars of whichever page is on
// screen, and two that change the page (one by value, one as a button).
constexpr uint8_t kPageKnobCc = 102;     // 102..105
constexpr uint8_t kPageSelectCc = 106;   // four equal zones: SOUND, FX, ENV, LFO
constexpr uint8_t kPageNextCc = 107;     // 64 or more: the next page
inline int pageKnob(uint8_t cc) { return cc >= kPageKnobCc && cc < kPageKnobCc + kBars ? cc - kPageKnobCc : -1; }

// Pickup ("soft takeover") for a knob that sends its own idea of the value:
// it moves the control only once it has reached it, either starting within
// a step of it or crossing it, so changing page (or nudging the bar from the
// keys) never makes the first turn jump. Moved by anything else after
// that, it has to pick it up again.
class Pickup {
 public:
  void reset() { last_ = -1.0f; caught_ = false; }
  // The knob sent v (0..1) while the control stands at `current`: true if
  // the knob now has it.
  bool feed(float v, float current) {
    if (caught_ && std::fabs(current - mine_) > 1e-4f) caught_ = false;   // moved by something else
    if (!caught_) {
      const bool near = std::fabs(v - current) <= 1.5f / 127.0f;
      const bool crossed = last_ >= 0.0f && (last_ - current) * (v - current) <= 0.0f;
      caught_ = near || crossed;
    }
    last_ = v;
    return caught_;
  }
  // Where the control ended up once the knob moved it (after rounding, as a
  // stepped control rounds), so feed() can tell its own moves from others'.
  void moved(float actual) { mine_ = actual; }

 private:
  float last_ = -1.0f, mine_ = -1.0f;
  bool caught_ = false;
};

}  // namespace controls
}  // namespace pd
