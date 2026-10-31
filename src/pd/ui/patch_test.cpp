// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/ui/patch_test.cpp -o t && ./t
#include "patch.h"

#include <cassert>
#include <cmath>

using namespace pd::patch;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }
}  // namespace

int main() {
  Patch a;
  strcpy(a.engine, "CLOUD"); strcpy(a.surface, "49 7x7");
  a.octave = 5; a.root = 0.2449f; a.bpm = 133.0f;
  a.params[0] = 0.1f; a.params[1] = 0.2f; a.params[2] = 0.3f; a.params[3] = 0.9f;
  char text[512];
  check(toText(a, text, sizeof text) > 0, "writes");
  Patch b;
  check(fromText(text, &b), "reads back");
  check(!strcmp(b.engine, "CLOUD") && !strcmp(b.surface, "49 7x7") && b.octave == 5, "names and ints round-trip");
  check(std::fabs(b.root - a.root) < 1e-4f && b.bpm == 133.0f && std::fabs(b.params[3] - 0.9f) < 1e-4f, "floats round-trip");
  Patch c;
  check(fromText("pocketdynamo patch 1\r\nbpm=90\r\nwhatever=3\r\n", &c), "CRLF and unknown keys");
  check(c.bpm == 90.0f && !strcmp(c.engine, "ZX PULSE"), "missing keys keep defaults");
  check(!strcmp(c.echo, "OFF"), "a patch from before the echo loads with it off");
  Patch d; snprintf(d.echo, sizeof d.echo, "1/8."); d.echoFeedback = 0.6f; d.echoMix = 0.2f;
  snprintf(d.tiltSide, sizeof d.tiltSide, "ECHO MIX"); snprintf(d.tiltFront, sizeof d.tiltFront, "HARMONICS");
  char t[512]; toText(d, t, sizeof t);
  Patch e; check(fromText(t, &e) && !strcmp(e.echo, "1/8.") && std::fabs(e.echoFeedback - 0.6f) < 1e-3f &&
                 std::fabs(e.echoMix - 0.2f) < 1e-3f, "echo round-trips");
  check(!strcmp(e.tiltSide, "ECHO MIX") && !strcmp(e.tiltFront, "HARMONICS"), "tilt routing round-trips");
  check(!strcmp(c.tiltSide, "TIMBRE") && !strcmp(c.tiltFront, "MORPH"), "older patches keep the default routing");
  Patch f; snprintf(f.fxType[0], 8, "BP"); f.fxAmount[0] = 0.4f; snprintf(f.fxType[1], 8, "PWM"); f.fxAmount[1] = 0.7f;
  f.fxAmount[3] = 0.55f;
  char ft[768]; toText(f, ft, sizeof ft);
  Patch g; check(fromText(ft, &g) && !strcmp(g.fxType[0], "BP") && std::fabs(g.fxAmount[0] - 0.4f) < 1e-3f &&
                 !strcmp(g.fxType[1], "PWM") && !strcmp(g.fxType[2], "FOLD") && std::fabs(g.fxAmount[3] - 0.55f) < 1e-3f,
                 "fx round-trip");
  check(c.fxAmount[0] == 0.0f && c.fxAmount[3] == 0.0f, "older patches load with the FX off");
  check(!fromText("hello", &c), "not a patch");
  Patch junk;
  check(fromText("pocketdynamo patch 1\nbpm=nan\nroot=inf\nmacro=-nan\necho_mix=1e99\nfx_tone=LP nan\nmorph=0.25\n", &junk), "a patch with junk numbers loads");
  const Patch def;
  check(junk.bpm == def.bpm && junk.root == def.root && junk.params[3] == def.params[3] && junk.echoMix == def.echoMix &&
        junk.fxAmount[0] == def.fxAmount[0], "non-finite numbers keep their defaults");
  check(!strcmp(junk.fxType[0], "LP") && std::fabs(junk.params[2] - 0.25f) < 1e-6f, "the good values on those lines still load");

  uint8_t h[kWavHeader];
  wavHeader(h, 24000, 1000);
  uint32_t rate = 0, off = 0, n = 0;
  check(wavParse(h, sizeof h, &rate, &off, &n) && rate == 24000 && off == 44 && n == 1000, "WAV header round-trips");
  h[22] = 2;   // stereo
  check(!wavParse(h, sizeof h, &rate, &off, &n), "stereo refused");
  {
    // A chunk claiming ~4 GB (it would wrap the offset and loop forever) and
    // one running past the header: both refused, neither hangs.
    uint8_t bad[64] = {};
    memcpy(bad, "RIFF", 4); memcpy(bad + 8, "WAVE", 4);
    memcpy(bad + 12, "JUNK", 4); bad[16] = 0xF8; bad[17] = 0xFF; bad[18] = 0xFF; bad[19] = 0xFF;
    check(!wavParse(bad, sizeof bad, &rate, &off, &n), "a 4 GB chunk size is refused");
    bad[16] = 200; bad[17] = bad[18] = bad[19] = 0;
    check(!wavParse(bad, sizeof bad, &rate, &off, &n), "a chunk past the header is refused");
  }
  printf("patch ok\n");
  return 0;
}
