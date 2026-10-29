// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/ui/tilt_test.cpp -o t && ./t
#include "tilt.h"

#include <cassert>
#include <cstdio>

using namespace pd::tilt;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
}  // namespace

int main() {
  const float s = std::sqrt(0.5f);
  check(near(timbre(0, 0, 1), 0.5f) && near(morph(0, 0, 1), 0.0f), "flat: timbre middle, morph 0");
  check(near(timbre(-1, 0, 0), 0.0f) && near(timbre(1, 0, 0), 1.0f), "edges: timbre 0 left, 1 right");
  check(near(timbre(-s, 0, s), 0.25f), "45 degrees left: timbre 0.25");
  check(near(morph(0, 1, 0), 0.8f) && near(timbre(0, 1, 0), 0.5f), "standing: morph 0.8, timbre middle");
  check(near(morph(0, 0.924f, -0.383f), 1.0f), "22.5 degrees past upright: morph 1");
  check(near(morph(0, -0.5f, 0.866f), 0.0f), "tilted back: morph 0");
  check(near(morph(0, -0.1f, -1), 1.0f), "upside down: morph stays 1");
  check(near(timbre(-s, s, 0), 0.25f), "standing and leaning left reads like flat and leaning left");
  check(near(timbre(0, 0, 0), 0.5f), "no reading holds the middle");
  printf("tilt ok\n");
  return 0;
}
