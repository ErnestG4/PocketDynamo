// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/ui/controls_test.cpp -o t && ./t
#include "controls.h"
#include "../synth/events.h"

#include <cassert>
#include <cstdio>

using namespace pd::controls;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }
}  // namespace

int main() {
  // The table and its arithmetic.
  check(pageBar(kSoundPage, 1) == Ctl::Timbre && pageBar(kFxPage, 3) == Ctl::Space &&
        pageBar(kEnvPage, 0) == Ctl::Attack && pageBar(kLfoPage, 2) == Ctl::LfoShape, "page bars");
  for (int p = 0; p < kPages; ++p)
    for (int b = 0; b < kBars; ++b) check(pageOf(pageBar(p, b)) == p, "a bar's page");
  check(pageOf(Ctl::EchoFb) == -1 && pageOf(Ctl::Pitch) == -1, "no page for the rest");
  check((int)Ctl::Harmonics == (int)pd::Param::Harmonics && (int)Ctl::Macro == (int)pd::Param::Macro, "SOUND in Param order");

  // Names: the ones patches already use for tilt and the LFO.
  check(byName("ECHO FB", kTilt) == Ctl::EchoFb && byName("TONE", kTilt) == Ctl::Tone, "tilt names");
  check(byName("PITCH", kTilt) == kNone && byName("PITCH", kLfo) == Ctl::Pitch, "pitch is the LFO's alone");
  check(byName("ATTACK", kTilt) == kNone && byName("NONE", kLfo) == kNone && byName("", kLfo) == kNone, "the rest isn't routable");
  check(!strcmp(name(kNone), "NONE"), "routed nowhere");
  for (int i = 0; i < kCount; ++i) {
    if (drivenBy((Ctl)i, kLfo)) check(info((Ctl)i).lfo != pd::LfoDest::None, "every LFO target has an instrument destination");
    for (int j = 0; j < i; ++j) check(strcmp(kInfo[i].name, kInfo[j].name) != 0, "names are unique");
  }

  // The fixed CC map: the same as before the table.
  check(ccControl(16) == Ctl::Harmonics && ccControl(19) == Ctl::Macro && ccControl(20) == Ctl::Tone &&
        ccControl(23) == Ctl::Space && ccControl(24) == Ctl::Attack && ccControl(27) == Ctl::Release &&
        ccControl(28) == Ctl::LfoRate && ccControl(31) == Ctl::LfoDelay, "16..31 in page order");
  check(ccControl(71) == Ctl::Harmonics && ccControl(74) == Ctl::Timbre && ccControl(2) == Ctl::Macro &&
        ccControl(18) == Ctl::Morph, "performance CCs");
  check(ccControl(73) == Ctl::Attack && ccControl(75) == Ctl::Decay && ccControl(72) == Ctl::Release, "GM2 envelope");
  check(ccControl(76) == Ctl::LfoRate && ccControl(77) == Ctl::LfoDepth && ccControl(1) == Ctl::LfoDepth &&
        ccControl(78) == Ctl::LfoDelay, "GM2 vibrato, the mod wheel");
  check(ccControl(7) == Ctl::Volume && ccControl(12) == Ctl::EchoFb && ccControl(13) == Ctl::EchoTime &&
        ccControl(91) == Ctl::EchoMix, "volume and echo");
  check(ccControl(64) == kNone && ccControl(0) == kNone && ccControl(102) == kNone && ccControl(106) == kNone, "unmapped");
  check(pageKnob(101) == -1 && pageKnob(102) == 0 && pageKnob(105) == 3 && pageKnob(106) == -1, "page knobs");

  // Pickup.
  Pickup p;
  check(!p.feed(0.10f, 0.50f), "far below: not yet");
  check(!p.feed(0.30f, 0.50f), "still below");
  check(p.feed(0.60f, 0.50f), "crossing picks it up");
  p.moved(0.60f);
  check(p.feed(0.62f, 0.60f), "and keeps it");
  p.moved(0.62f);
  check(!p.feed(0.64f, 0.30f), "moved by the keys: lost");
  check(p.feed(0.30f + 1.0f / 127.0f, 0.30f), "back within a step: picked up again");
  p.reset();
  check(p.feed(0.505f, 0.50f), "a fresh knob within a step takes it at once");
  p.reset();
  check(!p.feed(0.9f, 0.5f) && p.feed(0.4f, 0.5f), "from above, crossing down");
  // A stepped control rounds what the knob sets: still the knob's own move.
  p.reset();
  check(p.feed(0.26f, 0.25f), "near a step");
  p.moved(0.25f);
  check(p.feed(0.30f, 0.25f), "rounded back to the same step: still caught");
  printf("controls ok\n");
  return 0;
}
