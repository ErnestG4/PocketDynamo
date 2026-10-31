// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The one recorded sample every sampler engine reads: mono 12-bit at 24 kHz,
// two samples packed in three bytes — SP-1200 territory (26 kHz/12-bit),
// which buys 2.5 s in 90 KB and the period grit with it. (It was 3.3 s of
// int16 in 160 KB until BLE MIDI and the effects needed the room.)
// at()/put() see 16-bit values
// whose low 4 bits are zero; the take is normalized on read, so 12 bits
// hold its full 72 dB.
//
// Ownership moves with state, so no locks: Empty/Ready -> Recording (audio
// core writes, engines go silent) -> Analyzing (control core trims,
// normalizes and slices) -> Ready (engines read; nobody writes). The control
// side drives every transition; the audio side only appends while Recording.
// Pure C++ — host-testable.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../dsp/pack12.h"

namespace pd {

class SampleStore {
 public:
  static constexpr uint32_t kRate = 24000;
  static constexpr uint32_t kCapacity = 60000;   // 2.5 s
  static constexpr int kMaxSlices = 16;
  enum State : uint8_t { Empty, Recording, Analyzing, Ready };

  // --- control side ---
  void startRecording() {
    length_ = 0;
    half_ = false;
    state_.store(Recording, std::memory_order_release);
  }
  // Ends the take. Call once the audio side has seen the state change (a
  // block later); returns false if nothing usable was caught.
  void stopRecording() { state_.store(Analyzing, std::memory_order_release); }
  bool analyze() {
    if (state_.load(std::memory_order_acquire) != Analyzing) return false;
    trim();
    if (length_ < kRate / 20) { length_ = 0; state_.store(Empty, std::memory_order_release); return false; }
    normalize();
    slice();
    state_.store(Ready, std::memory_order_release);
    return true;
  }
  void clear() { length_ = 0; slices_ = 0; state_.store(Empty, std::memory_order_release); }

  // Load a take from elsewhere (SD): beginLoad() silences the readers, the
  // loader put()s up to kCapacity samples at kRate, and finishLoad(n) trims,
  // normalizes and slices it like a recording.
  void beginLoad() { state_.store(Analyzing, std::memory_order_release); }
  bool finishLoad(uint32_t n) { length_ = std::min(n, kCapacity); return analyze(); }

  // --- audio side: append one block of the 48 kHz input, decimated 2:1 ---
  void record(const float* in, size_t n) {
    if (state_.load(std::memory_order_acquire) != Recording) return;
    for (size_t i = 0; i < n && length_ < kCapacity; ++i) {
      if (!half_) { pending_ = in[i]; half_ = true; continue; }
      half_ = false;
      const float v = 0.5f * (pending_ + in[i]) * 32767.0f;   // pair average: cheap anti-alias
      put(length_++, (int)(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v)));
    }
  }

  // --- readers ---
  State state() const { return (State)state_.load(std::memory_order_acquire); }
  bool ready() const { return state() == Ready; }
  bool full() const { return length_ >= kCapacity; }
  uint32_t length() const { return length_; }
  // Sample i, as 16-bit (low 4 bits zero).
  int16_t at(uint32_t i) const { return packed_.at(i); }
  // Samples i and i + 1 (i + 1 < kCapacity), for interpolation.
  void pair(uint32_t i, int* a, int* b) const { packed_.pair(i, a, b); }
  // Store sample i from a 16-bit value, rounded to 12 bits (writer only).
  void put(uint32_t i, int v) { packed_.put(i, v); }
  float gain() const { return gain_; }   // to apply on read: normalizes the take
  int slices() const { return slices_; }
  uint32_t sliceStart(int i) const { return slice_[i]; }
  uint32_t sliceEnd(int i) const { return i + 1 < slices_ ? slice_[i + 1] : length_; }
  // Interpolated read at a fractional position, normalized, 0 outside.
  float read(uint32_t i, float frac) const {
    if (i + 1 >= length_) return 0.0f;
    const float a = (float)at(i), b = (float)at(i + 1);
    return gain_ * (1.0f / 32768.0f) * (a + frac * (b - a));
  }

  // Host tests fill the store directly.
  void loadForTest(const float* x, uint32_t n) {
    state_.store(Recording);
    length_ = 0; half_ = false;
    for (uint32_t i = 0; i < n && length_ < kCapacity; ++i)
      put(length_++, (int)std::lround(std::fmax(-1.0f, std::fmin(1.0f, x[i])) * 32767.0f));
    stopRecording();
  }

 private:
  // Cut leading/trailing silence: start 5 ms before the first sample within
  // 40 dB of the peak, end 20 ms after the last within 50 dB.
  void trim() {
    int peak = 1;
    for (uint32_t i = 0; i < length_; ++i) peak = std::max(peak, std::abs((int)at(i)));
    if (peak < 33) { length_ = 0; return; }   // nothing above -60 dBFS: no take
    const int on = peak / 100, off = peak / 316;
    uint32_t a = 0, b = length_;
    while (a < length_ && std::abs((int)at(a)) < on) ++a;
    while (b > a && std::abs((int)at(b - 1)) < off) --b;
    a = a > kRate / 200 ? a - kRate / 200 : 0;
    b = std::min(length_, b + kRate / 50);
    if (b <= a) { length_ = 0; return; }
    for (uint32_t j = 0; j < b - a; ++j) put(j, at(j + a));   // forward: reads stay ahead of writes
    length_ = b - a;
  }

  void normalize() {
    int peak = 1;
    for (uint32_t i = 0; i < length_; ++i) peak = std::max(peak, std::abs((int)at(i)));
    gain_ = std::fmin(0.9f * 32768.0f / (float)peak, 64.0f);   // cap: don't blow up hiss
  }

  // Onset slices from 5 ms frame energies: a slice starts where log energy
  // jumps by > 6 dB over the recent floor and sits within 30 dB of the
  // loudest frame, at least 80 ms after the previous start.
  void slice() {
    constexpr uint32_t kFrame = kRate / 200;            // 5 ms
    constexpr uint32_t kMinGap = kRate * 8 / 100;       // 80 ms
    slices_ = 0;
    slice_[slices_++] = 0;
    float loudest = 1e-9f;
    for (uint32_t f = 0; f + kFrame <= length_; f += kFrame) loudest = std::fmax(loudest, frameEnergy(f, kFrame));
    const float floorDb = 10.0f * std::log10(loudest) - 30.0f;
    float prevDb = -200.0f, recent = -200.0f;
    for (uint32_t f = kFrame; f + kFrame <= length_ && slices_ < kMaxSlices; f += kFrame) {
      const float db = 10.0f * std::log10(frameEnergy(f, kFrame) + 1e-12f);
      recent = std::fmax(prevDb, recent - 1.5f);   // decaying memory of the last frames
      if (db > floorDb && db - recent > 6.0f && f - slice_[slices_ - 1] >= kMinGap) {
        // Back up to the zero crossing just before the frame.
        uint32_t s = f;
        while (s > slice_[slices_ - 1] + 1 && (at(s) >= 0) == (at(s - 1) >= 0) && f - s < kFrame) --s;
        slice_[slices_++] = s;
      }
      prevDb = db;
    }
  }

  float frameEnergy(uint32_t from, uint32_t n) const {
    float e = 0.0f;
    for (uint32_t i = from; i < from + n; ++i) { const float v = (float)at(i); e += v * v; }
    return e / (float)n;
  }

  Pack12<kCapacity> packed_;
  uint32_t length_ = 0;
  float gain_ = 1.0f;
  float pending_ = 0.0f;
  bool half_ = false;
  int slices_ = 0;
  uint32_t slice_[kMaxSlices] = {};
  std::atomic<uint8_t> state_{Empty};
};

// The device has one store; engines and the control side share it.
inline SampleStore& sampleStore() {
  static SampleStore s;
  return s;
}

}  // namespace pd
