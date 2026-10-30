// Control -> audio events and the lock-free single-producer/single-consumer
// ring that carries them from core 0 to core 1. The audio side drains the
// ring at the top of every block; at 32-frame blocks that is 0.67 ms of
// timing jitter, below anything a keyboard can resolve.
#pragma once
#include <atomic>
#include <cstdint>

namespace pd {

enum class EventType : uint8_t { NoteOn, NoteOff, Retrigger, Pitch, Param, Engine, AllOff, Tempo, Bend, Echo, Fx };

enum class Param : uint8_t { Harmonics, Timbre, Morph, Macro, Count };

// Notes are addressed by tag, not pitch: the tag names what started them (a
// key), so a chord is several NoteOns under one tag and one NoteOff ends it,
// and pitches can be anywhere between the semitones.
struct Event {
  EventType type;
  uint8_t   a;      // NoteOn/NoteOff/Retrigger: tag. Param: Param id. Engine: engine id.
  uint8_t   b;      // NoteOn: velocity 1..127.
  float     value;  // NoteOn/Pitch: pitch, MIDI note, fractional (Pitch retunes
                    // a tag's held voices legato). Retrigger: decay ms,
                    // 0 = hold as a normal note. Param: 0..1. Tempo: bpm.
                    // Bend: semitones added to every voice (MIDI pitch bend).
                    // Echo: a = 0 time (s, <= 0 off), 1 feedback, 2 mix.
                    // Fx: a = FX bar (0 TONE, 1 GRIT, 2 SHAPE, 3 SPACE),
                    // b = its effect type, value = amount.
};

template <int N>
class SpscQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");

 public:
  bool push(const Event& e) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= (uint32_t)N) return false;
    buf_[h & (N - 1)] = e;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(Event* e) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    *e = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

 private:
  Event buf_[N];
  std::atomic<uint32_t> head_{0}, tail_{0};
};

}  // namespace pd
