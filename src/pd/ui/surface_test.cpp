// Host test: g++ -std=c++17 -O2 -I src/pd src/pd/ui/surface_test.cpp -o t && ./t
#include "surface.h"
#include "../synth/dice.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace pd;
using namespace pd::surface;

namespace {
void check(bool c, const char* m) { if (!c) { fprintf(stderr, "FAIL: %s\n", m); assert(false); } }
bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
const Surface& byName(const char* n) {
  for (const auto& s : kSurfaces) if (!strcmp(s.name, n)) return s;
  assert(false); return kSurfaces[0];
}
}  // namespace

int main() {
  for (const auto& s : kSurfaces) {
    // Every note column of every row plays, except the piano's gaps; control
    // columns never do. Pitch rises left to right on every grid row.
    for (int r = 0; r < 4; ++r) {
      check(keyDegree(s, r, 0) == kNoDegree && keyDegree(s, r, 13) == kNoDegree, "outer columns are controls");
      if (s.piano) continue;
      for (int c = 2; c <= 12; ++c)
        check(degreeSemitones(s, keyDegree(s, r, c)) > degreeSemitones(s, keyDegree(s, r, c - 1)), "rises along a row");
    }
    check(keyDegree(s, 3, 1) == (s.piano ? kNoDegree : 0), "bottom-left is the root");
    for (int i = 0; i < kNumChords; ++i) {
      int d[kMaxChordNotes];
      const int n = chordDegrees(s, i, d);
      check(n >= 2 && d[0] == 0, "chords start on the played key");
      for (int k = 1; k < n; ++k) check(d[k] > d[k - 1], "chord tones ascend");
    }
    printf("%-13s ok\n", s.name);
  }

  // Scale surfaces stay in the scale: a fourth up per row, an octave = 7 degrees.
  const Surface& maj = byName("MAJOR 4THS");
  check(near(degreeSemitones(maj, keyDegree(maj, 2, 1)), 5.0f), "major: row up = a fourth");
  check(near(degreeSemitones(maj, 7), 12.0f) && near(degreeSemitones(maj, -1), -1.0f), "major: octave and below root");
  const Surface& bass = byName("CHROMA 4THS");
  check(near(degreeSemitones(bass, keyDegree(bass, 0, 1)), 15.0f), "chromatic: three rows = three fourths");

  // 49-EDO: superpyth fifth is 29 steps; 7x7 rows are one step apart.
  const Surface& sp = byName("49 SUPERPYTH");
  check(near(degreeSemitones(sp, 4), 29 * 12.0f / 49), "superpyth: degree 4 is the 29-step fifth");
  const Surface& x7 = byName("49 7x7");
  check(near(degreeSemitones(x7, keyDegree(x7, 2, 1)) * 100.0f, 1200.0f / 49), "7x7: a row up is one 49-EDO step");
  check(near(degreeSemitones(x7, keyDegree(x7, 3, 2)) * 100.0f, 7 * 1200.0f / 49), "7x7: a column is a 7-EDO step");
  int d[kMaxChordNotes];
  chordDegrees(x7, 1, d);   // M: 400 c, 700 c snap to 16 and 29 steps
  check(d[1] == 16 && d[2] == 29, "49-EDO major triad snaps to 16 and 29 steps");
  const Surface& pen = byName("PENTA 4THS");
  chordDegrees(pen, 10, d);   // OCT on a 5-note scale is 5 degrees
  check(d[1] == 5, "stack chords rescale to the scale length");

  // Dice: reproducible from the seed, in range, and seeds differ.
  float a[4] = {0.5f, 0.5f, 0.5f, 0.5f}, b[4] = {0.5f, 0.5f, 0.5f, 0.5f}, c[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  dice::roll(a, 1234); dice::roll(b, 1234); dice::roll(c, 1235);
  bool differ = false;
  for (int p = 0; p < 4; ++p) {
    check(a[p] == b[p] && a[p] >= 0.0f && a[p] <= 1.0f, "roll is seeded and bounded");
    differ |= a[p] != c[p];
  }
  check(differ, "different seeds roll differently");
  float hsum = 0.0f, tmin = 1.0f;
  for (uint32_t seed = 0; seed < 2000; ++seed) {
    float p[4] = {}; dice::roll(p, seed);
    hsum += p[0]; tmin = std::min(tmin, p[1]);
  }
  check(hsum / 2000 < 0.35f && tmin >= 0.25f, "harmonics lean low, timbre stays off the bottom");
  for (uint32_t seed = 0; seed < 200; ++seed) {
    float m[4] = {0.5f, 0.5f, 0.5f, 0.5f}; dice::mutate(m, seed);
    for (float v : m) check(std::fabs(v - 0.5f) <= 0.2f + 1e-6f, "mutate stays a neighbour");
    float z[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    dice::mutate(z, seed);
    check(z[0] != 0.0f || z[1] != 1.0f || z[2] != 0.0f || z[3] != 1.0f, "mutate always moves something, even at the ends");
    for (float v : z) check(v >= 0.0f && v <= 1.0f, "mutate stays in range");
  }
  printf("dice ok\n");

  // degreeOf inverts degreeSemitones on every surface, and finds nothing
  // between the keys.
  for (const auto& s : kSurfaces)
    for (int d = -20; d <= 40; ++d) check(degreeOf(s, degreeSemitones(s, d)) == d, "degreeOf round-trips");
  check(degreeOf(byName("MAJOR 4THS"), 1.0f) == kNoDegree, "C# is off C major");
  check(degreeOf(byName("MAJOR 4THS"), -1.0f) == -1, "B below the root is degree -1");
  check(degreeOf(byName("PENTA 4THS"), 15.0f) == 6, "Eb an octave up in minor pentatonic");
  check(degreeOf(byName("49 SUPERPYTH"), 7.0f) == kNoDegree, "a 12-EDO fifth is off the 49 grid");
  check(degreeOf(byName("CHROMA 4THS"), 13.0f) == 13, "chromatic: every semitone");
  printf("degreeOf ok\n");
  return 0;
}
