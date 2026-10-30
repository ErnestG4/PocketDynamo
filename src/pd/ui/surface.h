// The playing surface: how the key grid becomes pitches. Columns 1..12 of all
// four rows play; columns 0 and 13 are controls. Most surfaces are
// isomorphic: one column right is colStep scale degrees, one row up is
// rowStep, so a shape played anywhere is the same chord or interval. Scale
// surfaces only contain scale notes — every key is a right note.
//
// The 49-EDO surfaces (24.5 cent steps) are the weird ones:
//   49 SUPERPYTH  a major scale built from 49-EDO's sharp fifth (29 steps,
//                 710 c): wide 220 c whole tones, tiny 49 c semitones, and a
//                 441 c major third near the septimal 9/7.
//   49 7x7        49 = 7 x 7. Across a row, 7-step jumps: the equal 7-note
//                 octave (171 c steps), gamelan/Thai flavoured. Each row up is
//                 one 49-EDO step, so stacking rows shades every note sharp.
//   49 CHROMA     every step in order: the 48 keys span one octave less a step.
// And three that leave equal steps behind (Kind other than Edo):
//   5THS PYTHAG   a chain of pure 3:2 fifths: across a row each key is a
//                 fifth on (folded into the row's octave), and the chain runs
//                 on up the rows, an octave each; so a row's 12 notes are the
//                 Pythagorean 12, each row a comma (23.5 c) sharper.
//   STERN-BROCOT  pure ratios root x p/q from the Stern-Brocot tree between
//                 1/1 and 2/1, breadth first: the bottom row the simplest
//                 twelve (1/1 to 7/4), each row up the next twelve, one level
//                 deeper, all in the same octave and ascending.
//   IMPRESSION    the first key held is a root; every other key plays what
//                 its direction and distance from it suggest (impression()).
// Pure table and arithmetic — host-testable.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "keymap.h"

namespace pd {
namespace surface {

constexpr int kNoDegree = -10000;

constexpr bool isNoteColumn(int c) { return c >= 1 && c <= 12; }

enum class Kind : uint8_t { Edo, Fifths, Brocot, Impression };

struct Surface {
  const char* name;
  uint8_t divisions;   // equal steps per octave
  uint8_t len;         // notes per octave in the scale; == divisions: every step
  uint8_t steps[12];   // the scale, in steps, when len < divisions
  uint8_t colStep;     // degrees per column right
  uint8_t rowStep;     // degrees per row up
  bool    piano;       // tracker piano table (keymap.h) instead of a grid
  Kind    kind = Kind::Edo;   // Edo: degrees of `divisions` equal steps; else ratioKey()
};

constexpr Surface kSurfaces[] = {
  {"MAJOR 4THS",   12,  7, {0, 2, 4, 5, 7, 9, 11},        1,  3, false},
  {"MINOR 4THS",   12,  7, {0, 2, 3, 5, 7, 8, 10},        1,  3, false},
  {"DORIAN 4THS",  12,  7, {0, 2, 3, 5, 7, 9, 10},        1,  3, false},
  {"PENTA 4THS",   12,  5, {0, 3, 5, 7, 10},              1,  2, false},
  {"CHROMA 4THS",  12, 12, {},                            1,  5, false},
  {"PIANO",        12, 12, {},                            0,  0, true},
  {"49 SUPERPYTH", 49,  7, {0, 9, 18, 20, 29, 38, 47},    1,  3, false},
  {"49 7x7",       49, 49, {},                            7,  1, false},
  {"49 CHROMA",    49, 49, {},                            1, 12, false},
  {"5THS PYTHAG",  12, 12, {},                            0,  0, false, Kind::Fifths},
  {"STERN-BROCOT", 12, 12, {},                            0,  0, false, Kind::Brocot},
  {"IMPRESSION",   12, 12, {},                            1,  5, false, Kind::Impression},
};
constexpr int kNumSurfaces = sizeof(kSurfaces) / sizeof(kSurfaces[0]);

inline bool chromatic(const Surface& s) { return s.len == s.divisions; }

// Scale degree of key (r, c) above the surface root, or kNoDegree. (The
// ratio surfaces have no degrees: ratioKey(). IMPRESSION's root key plays
// its chromatic-fourths degree.)
inline int keyDegree(const Surface& s, int r, int c) {
  if (s.piano) { const int n = keymap::kNote[r][c]; return n < 0 ? kNoDegree : n; }
  if (!isNoteColumn(c) || s.kind == Kind::Fifths || s.kind == Kind::Brocot) return kNoDegree;
  return (c - 1) * s.colStep + (3 - r) * s.rowStep;   // bottom-left is the root
}

// Degree -> semitones above the root (fractional off 12-EDO).
inline float degreeSemitones(const Surface& s, int d) {
  const int L = s.len;
  const int oct = d >= 0 ? d / L : -((-d + L - 1) / L);
  const int idx = d - oct * L;
  const int step = (chromatic(s) ? idx : s.steps[idx]) + oct * s.divisions;
  return (float)step * (12.0f / (float)s.divisions);
}

// The degree that plays `semitones` above the root, or kNoDegree when no
// key of the surface lands on it (a MIDI note off the scale, or off a
// 49-EDO grid).
inline int degreeOf(const Surface& s, float semitones) {
  const float steps = semitones * (float)s.divisions / 12.0f;
  const int step = (int)std::lround(steps);
  if (std::fabs(steps - (float)step) > 0.01f) return kNoDegree;
  const int oct = step >= 0 ? step / s.divisions : -((-step + s.divisions - 1) / s.divisions);
  const int inOct = step - oct * s.divisions;
  if (chromatic(s)) return step;
  for (int i = 0; i < s.len; ++i)
    if (s.steps[i] == inOct) return oct * s.len + i;
  return kNoDegree;
}

// --- the ratio surfaces ------------------------------------------------------

constexpr float kNoPitch = -1.0e9f;

// The Stern-Brocot tree between 1/1 and 2/1, breadth first: 1/1, then
// each level's mediants (3/2; 4/3 5/3; 5/4 7/5 8/5 7/4; ...), and within a
// level the simplest first (smallest p*q), so every row spans the octave.
// Rows of twelve from the bottom, each sorted to ascend.
struct Ratio { uint16_t p, q; };
struct BrocotTable {
  static constexpr int kKeys = 48, kMaxNodes = 64;   // depths 0..6
  Ratio key[kKeys];
  constexpr BrocotTable() : key() {
    Ratio all[kMaxNodes + 2] = {};   // the tree so far, in order, between 1/1 and 2/1
    int n = 2;
    all[0] = {1, 1}; all[1] = {2, 1};
    Ratio order[kMaxNodes] = {};
    int depth[kMaxNodes] = {};
    int got = 0;
    order[got] = {1, 1}; depth[got++] = 0;
    for (int level = 1; level <= 6; ++level) {
      Ratio next[kMaxNodes + 2] = {};
      int m = 0;
      for (int i = 0; i + 1 < n; ++i) {
        next[m++] = all[i];
        const Ratio med = {(uint16_t)(all[i].p + all[i + 1].p), (uint16_t)(all[i].q + all[i + 1].q)};
        next[m++] = med;
        if (got < kMaxNodes) { order[got] = med; depth[got++] = level; }
      }
      next[m++] = all[n - 1];
      for (int i = 0; i < m; ++i) all[i] = next[i];
      n = m;
    }
    // Breadth first, simplest first within a level.
    for (int i = 1; i < got; ++i)
      for (int j = i; j > 0; --j) {
        const bool before = depth[j] < depth[j - 1] ||
            (depth[j] == depth[j - 1] && order[j].p * order[j].q < order[j - 1].p * order[j - 1].q);
        if (!before) break;
        const Ratio t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        const int d = depth[j]; depth[j] = depth[j - 1]; depth[j - 1] = d;
      }
    for (int row = 0; row < 4; ++row) {
      Ratio* r = order + row * 12;
      for (int i = 1; i < 12; ++i)
        for (int j = i; j > 0 && r[j].p * r[j - 1].q < r[j - 1].p * r[j].q; --j) {
          const Ratio t = r[j]; r[j] = r[j - 1]; r[j - 1] = t;
        }
      for (int i = 0; i < 12; ++i) key[row * 12 + i] = r[i];
    }
  }
};
constexpr BrocotTable kBrocot{};

// The Stern-Brocot ratio of key (r, c), or {0, 0} off the note columns.
inline Ratio brocotRatio(int r, int c) {
  if (!isNoteColumn(c)) return {0, 0};
  return kBrocot.key[(3 - r) * 12 + (c - 1)];
}

// Semitones above the root for the ratio surfaces, or kNoPitch.
inline float ratioKey(const Surface& s, int r, int c) {
  if (!isNoteColumn(c)) return kNoPitch;
  const int row = 3 - r;   // from the bottom
  if (s.kind == Kind::Fifths) {
    const int k = row * 12 + (c - 1);                      // fifths along the chain
    const float cents = std::fmod((float)k * 701.955f, 1200.0f) + 1200.0f * (float)row;
    return cents * 0.01f;
  }
  if (s.kind == Kind::Brocot) {
    const Ratio q = brocotRatio(r, c);
    return 12.0f * std::log2((float)q.p / (float)q.q);
  }
  return kNoPitch;
}

// IMPRESSION: what a key plays, given its place relative to the root key
// (rows up, columns right), in just-intonation cents above the root. The
// four directions are lines: right brightens (5/4, 15/8, 9/4), left darkens
// (6/5, 7/4, 8/3), up opens (3/2, 2, 3), down goes below the root (3/4, 1/2,
// 2/3). The diagonals are two-note shapes: up-right major, up-left minor,
// down-right suspended, down-left septimal. Three rings, Chebyshev
// distance; anything farther aliases back onto them an octave up, so every
// key says something about the root. Returns the note count (1 or 2).
inline int impression(int up, int right, float* cents) {
  if (up == 0 && right == 0) { cents[0] = 0.0f; return 1; }
  const int d = std::max(std::abs(up), std::abs(right));
  const int ring = (d - 1) % 3, lift = (d - 1) / 3;
  // [direction][ring][note]; 0 in the second note = a single note
  static constexpr float kPalette[8][3][2] = {
    {{386.3f, 0}, {1088.3f, 0}, {1403.9f, 0}},           // right: 5/4, 15/8, 9/4
    {{315.6f, 0}, {968.8f, 0}, {1698.0f, 0}},            // left: 6/5, 7/4, 8/3
    {{702.0f, 0}, {1200.0f, 0}, {1902.0f, 0}},           // up: 3/2, 2, 3
    {{-498.0f, 0}, {-1200.0f, 0}, {-702.0f, 0}},         // down: 3/4, 1/2, 2/3
    {{386.3f, 702.0f}, {1088.3f, 1586.3f}, {884.4f, 1403.9f}},   // up-right: major colours
    {{315.6f, 702.0f}, {1017.6f, 1515.6f}, {813.7f, 1403.9f}},   // up-left: minor colours
    {{203.9f, 702.0f}, {498.0f, 884.4f}, {203.9f, 1088.3f}},     // down-right: sus colours
    {{266.9f, 968.8f}, {582.5f, 968.8f}, {231.2f, 1166.0f}},     // down-left: septimal (7/6, 7/5, 8/7)
  };
  int dir;
  if (up == 0) dir = right > 0 ? 0 : 1;
  else if (right == 0) dir = up > 0 ? 2 : 3;
  else if (up > 0) dir = right > 0 ? 4 : 5;
  else dir = right > 0 ? 6 : 7;
  const float* note = kPalette[dir][ring];
  cents[0] = note[0] + 1200.0f * (float)lift;
  if (note[1] == 0.0f) return 1;
  cents[1] = note[1] + 1200.0f * (float)lift;
  return 2;
}

// --- chords: hold a strip key, play a key, get the chord on that key ---------

constexpr int kNumChords = 12;
constexpr int kMaxChordNotes = 4;

struct Chord { const char* label; uint8_t n; int16_t v[kMaxChordNotes]; };

// Scale surfaces: stacks of scale degrees (written for 7-note scales, rescaled
// to the scale's length), so chords stay in the scale wherever you play them.
constexpr Chord kStackChords[kNumChords] = {
  {"5",   2, {0, 4}},       {"TRI", 3, {0, 2, 4}},    {"S2",  3, {0, 1, 4}},
  {"S4",  3, {0, 3, 4}},    {"6",   4, {0, 2, 4, 5}}, {"7",   4, {0, 2, 4, 6}},
  {"9",   4, {0, 2, 6, 8}}, {"AD9", 4, {0, 2, 4, 8}}, {"4TH", 4, {0, 3, 6, 9}},
  {"OPN", 3, {0, 4, 9}},    {"OCT", 2, {0, 7}},       {"CLS", 3, {0, 1, 2}},
};

// Chromatic surfaces: intervals in cents, snapped to the nearest step of the
// tuning, so in 49-EDO every chord comes out bent its own way.
constexpr Chord kCentChords[kNumChords] = {
  {"5",   2, {0, 700}},                {"M",   3, {0, 400, 700}},
  {"m",   3, {0, 300, 700}},           {"S2",  3, {0, 200, 700}},
  {"S4",  3, {0, 500, 700}},           {"M7",  4, {0, 400, 700, 1100}},
  {"m7",  4, {0, 300, 700, 1000}},     {"7",   4, {0, 400, 700, 1000}},
  {"m9",  4, {0, 300, 1000, 1400}},    {"M9",  4, {0, 400, 1100, 1400}},
  {"69",  4, {0, 200, 900, 1600}},     {"7:4", 4, {0, 386, 702, 969}},
};

inline const Chord& chord(const Surface& s, int i) {
  return chromatic(s) ? kCentChords[i] : kStackChords[i];
}

// Degree offsets of chord i on surface s; returns the note count.
inline int chordDegrees(const Surface& s, int i, int* out) {
  const Chord& ch = chord(s, i);
  for (int k = 0; k < ch.n; ++k)
    out[k] = chromatic(s)
        ? (int)std::lround(ch.v[k] * (float)s.divisions / 1200.0f)
        : (int)std::lround(ch.v[k] * (float)s.len / 7.0f);
  // Rescaling can merge neighbours on short scales (a 5-note cluster): keep
  // every tone distinct by pushing it up a degree.
  for (int k = 1; k < ch.n; ++k) out[k] = std::max(out[k], out[k - 1] + 1);
  return ch.n;
}

// --- ratchet: hold a strip key and held notes re-strike at its rate ----------

constexpr int kNumRatchets = 12;
struct Ratchet { const char* label; float beats; };   // period in quarter notes
constexpr Ratchet kRatchets[kNumRatchets] = {
  {"4", 1.0f},      {"8.", 0.75f},   {"4t", 2.0f / 3},  {"8", 0.5f},
  {"16.", 0.375f},  {"8t", 1.0f / 3}, {"16", 0.25f},    {"32.", 0.1875f},
  {"16t", 1.0f / 6}, {"32", 0.125f},  {"32t", 1.0f / 12}, {"64", 0.0625f},
};

}  // namespace surface
}  // namespace pd
