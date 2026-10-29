// The ARP strip: which note of the held pool plays next. Slots 1..6 step on
// the tempo grid (16ths), retriggering each note; slots 7..12 are the same
// patterns as chip arps at a ZX frame rate (50 Hz, as Follin's drivers
// stepped once per interrupt), one voice re-pitched legato.
// MOTIF takes its timing and its note from Lenticular's three-distance gaps
// (dsp/ars.h): each of the <= 3 gap lengths owns a note, so the rhythm is
// the melody. Pure — host-testable.
#pragma once
#include <cstdint>

namespace pd {
namespace arp {

enum class Pattern : uint8_t { Up, Down, UpDown, Order, Random, Motif };

struct Slot { const char* label; Pattern pattern; bool chip; };
constexpr int kNumSlots = 12;
constexpr Slot kSlots[kNumSlots] = {
  {"UP", Pattern::Up, false},     {"DN", Pattern::Down, false},   {"UD", Pattern::UpDown, false},
  {"ORD", Pattern::Order, false}, {"RND", Pattern::Random, false}, {"MTF", Pattern::Motif, false},
  {"cUP", Pattern::Up, true},     {"cDN", Pattern::Down, true},   {"cUD", Pattern::UpDown, true},
  {"cOR", Pattern::Order, true},  {"cRN", Pattern::Random, true}, {"cMO", Pattern::Motif, true},
};
constexpr float kChipHz = 50.0f;
constexpr float kStepBeats = 0.25f;   // tempo slots step in 16ths

constexpr int kMaxPool = 56;

class Arp {
 public:
  void restart() { step_ = -1; last_ = -1; }

  // pitches[0..n) in press order. rnd: a fresh random word; motifU: the
  // ARS voice quantile of this step's gap. Returns the index to play.
  int next(Pattern p, const float* pitches, int n, uint32_t rnd, float motifU) {
    if (n <= 0) return -1;
    int sorted[kMaxPool];
    for (int i = 0; i < n; ++i) {   // insertion sort by pitch, stable on ties
      int j = i;
      while (j > 0 && pitches[sorted[j - 1]] > pitches[i]) { sorted[j] = sorted[j - 1]; --j; }
      sorted[j] = i;
    }
    int pick;
    switch (p) {
      case Pattern::Up:    step_ = (step_ + 1) % n; pick = sorted[step_]; break;
      case Pattern::Down:  step_ = (step_ + 1) % n; pick = sorted[n - 1 - step_]; break;
      case Pattern::UpDown: {
        const int period = n > 1 ? 2 * n - 2 : 1;
        step_ = (step_ + 1) % period;
        pick = sorted[step_ < n ? step_ : period - step_];
        break;
      }
      case Pattern::Order: step_ = (step_ + 1) % n; pick = step_; break;
      case Pattern::Random:
        pick = (int)(rnd % (uint32_t)n);
        if (n > 1 && pick == last_) pick = (pick + 1 + (int)((rnd >> 16) % (uint32_t)(n - 1))) % n;
        break;
      default: {   // Motif
        int i = (int)(motifU * (float)n);
        pick = sorted[i < 0 ? 0 : (i >= n ? n - 1 : i)];
        break;
      }
    }
    last_ = pick;
    return pick;
  }

 private:
  int step_ = -1;
  int last_ = -1;
};

}  // namespace arp
}  // namespace pd
