// Control -> audio events and the lock-free single-producer/single-consumer
// ring that carries them from core 0 to core 1. The audio side drains the
// ring at the top of every block; at 32-frame blocks that is 0.67 ms of
// timing jitter, below anything a keyboard can resolve.
#pragma once
#include <atomic>
#include <cstdint>

namespace pd {

enum class EventType : uint8_t { NoteOn, NoteOff, Param, Engine, AllOff };

enum class Param : uint8_t { Harmonics, Timbre, Morph, Macro, Count };

struct Event {
  EventType type;
  uint8_t   a;      // NoteOn/Off: note. Param: Param id. Engine: engine id.
  uint8_t   b;      // NoteOn: velocity 1..127.
  float     value;  // Param: 0..1.
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
