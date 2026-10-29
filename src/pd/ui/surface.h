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

struct Surface {
  const char* name;
  uint8_t divisions;   // equal steps per octave
  uint8_t len;         // notes per octave in the scale; == divisions: every step
  uint8_t steps[12];   // the scale, in steps, when len < divisions
  uint8_t colStep;     // degrees per column right
  uint8_t rowStep;     // degrees per row up
  bool    piano;       // tracker piano table (keymap.h) instead of a grid
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
};
constexpr int kNumSurfaces = sizeof(kSurfaces) / sizeof(kSurfaces[0]);

inline bool chromatic(const Surface& s) { return s.len == s.divisions; }

// Scale degree of key (r, c) above the surface root, or kNoDegree.
inline int keyDegree(const Surface& s, int r, int c) {
  if (s.piano) { const int n = keymap::kNote[r][c]; return n < 0 ? kNoDegree : n; }
  if (!isNoteColumn(c)) return kNoDegree;
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
