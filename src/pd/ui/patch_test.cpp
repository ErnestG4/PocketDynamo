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
  check(!fromText("hello", &c), "not a patch");

  uint8_t h[kWavHeader];
  wavHeader(h, 24000, 1000);
  uint32_t rate = 0, off = 0, n = 0;
  check(wavParse(h, sizeof h, &rate, &off, &n) && rate == 24000 && off == 44 && n == 1000, "WAV header round-trips");
  h[22] = 2;   // stereo
  check(!wavParse(h, sizeof h, &rate, &off, &n), "stereo refused");
  printf("patch ok\n");
  return 0;
}
