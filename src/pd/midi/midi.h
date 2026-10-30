// MIDI in, shared by every transport. A transport only gets bytes or
// packets off its wire; everything after that is here, once:
//   - MidiParser: a byte stream (BLE-MIDI after its header and timestamps
//     are stripped, serial) -> messages, with running status, realtime
//     bytes passed straight through and SysEx skipped.
//   - fromUsbPacket: a USB-MIDI 4-byte event packet -> a message (the
//     packet is already framed, so no parser state is needed).
//   - BleDeframer: a BLE-MIDI packet -> bytes for a Parser, each message
//     stamped from the packet's own millisecond timestamps.
//   - MidiIn: a message -> calls on a handler (notes, sustain, the four
//     controls, bend, program, clock tempo), so the device and the host
//     tests share one mapping. Omni: every channel plays.
// Pure — host-testable (midi_test.cpp).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pd {
namespace midi {

struct Msg {
  uint8_t status = 0, d1 = 0, d2 = 0;
  uint8_t kind() const { return status >= 0xF0 ? status : (uint8_t)(status & 0xF0); }
};

class Parser {
 public:
  // Feed one byte; true when *out holds a complete message.
  bool feed(uint8_t b, Msg* out) {
    if (b >= 0xF8) { *out = Msg{b, 0, 0}; return true; }   // realtime: never interrupts a message
    if (b & 0x80) {
      n_ = 0;
      sysex_ = b == 0xF0;
      if (b >= 0xF0) {   // system common: no running status
        status_ = 0;
        common_ = b;
        need_ = b == 0xF2 ? 2 : (b == 0xF1 || b == 0xF3) ? 1 : 0;
        if (need_ == 0) { common_ = 0; if (b == 0xF6) { *out = Msg{b, 0, 0}; return true; } }
        return false;
      }
      status_ = b;
      need_ = (b & 0xE0) == 0xC0 ? 1 : 2;   // program change, channel pressure: one data byte
      return false;
    }
    if (sysex_) return false;
    const uint8_t st = status_ ? status_ : common_;
    if (!st) return false;   // stray data byte
    data_[n_++] = b;
    if (n_ < need_) return false;
    *out = Msg{st, data_[0], need_ > 1 ? data_[1] : (uint8_t)0};
    n_ = 0;
    common_ = 0;
    return true;
  }

 private:
  uint8_t status_ = 0, common_ = 0, need_ = 0, n_ = 0;
  uint8_t data_[2] = {};
  bool sysex_ = false;
};

// USB-MIDI 1.0 event packet: byte 0 is cable (high nibble) and Code Index
// Number (low), bytes 1..3 the message. SysEx packets (CIN 4..7) carry no
// message we use.
inline bool fromUsbPacket(const uint8_t p[4], Msg* out) {
  const uint8_t cin = p[0] & 0x0F;
  if (cin >= 0x8 || cin == 0x2 || cin == 0x3) { *out = Msg{p[1], p[2], p[3]}; return (p[1] & 0x80) != 0; }
  if (cin == 0xF && p[1] >= 0xF8) { *out = Msg{p[1], 0, 0}; return true; }   // a lone realtime byte
  return false;
}

// BLE-MIDI (Apple's spec, adopted by the MMA): a header byte (bit 7 set,
// low 6 bits the high bits of a 13-bit millisecond timestamp), then
// messages, each led by a timestamp byte (bit 7 set, the low 7 bits) — so
// a byte with bit 7 set right after a timestamp is a status, anywhere else
// a timestamp. Running-status data may follow without one. Packets arrive
// once per connection interval (7.5-15 ms), so arrival times would jitter
// MIDI clock: each message is placed at the sender's timestamp instead,
// mapped to local micros by the smallest offset seen (the fastest
// delivery), which tracks drift by letting the minimum age out.
class BleDeframer {
 public:
  template <typename F>   // f(byte, atUs)
  void packet(const uint8_t* p, size_t n, uint32_t nowUs, F f) {
    if (n < 2 || !(p[0] & 0x80)) return;
    uint32_t high = p[0] & 0x3F;
    uint32_t prevLow = 0;
    bool afterTs = false;
    uint32_t at = nowUs;
    for (size_t i = 1; i < n; ++i) {
      const uint8_t b = p[i];
      if ((b & 0x80) && !afterTs) {
        const uint32_t low = b & 0x7F;
        if (low < prevLow) high = (high + 1) & 0x3F;   // the low byte wrapped inside the packet
        prevLow = low;
        at = place(high << 7 | low, nowUs);
        afterTs = true;
        continue;
      }
      afterTs = false;
      f(b, at);
    }
  }

 private:
  // 13-bit ms timestamp -> local micros.
  uint32_t place(uint32_t ts, uint32_t nowUs) {
    if (!started_) { lastTs_ = ts; started_ = true; }
    ms_ += (ts - lastTs_) & 0x1FFF;   // unwrap: timestamps only move forward
    lastTs_ = ts;
    const int32_t offset = (int32_t)(nowUs - ms_ * 1000u);   // latency + clock offset
    if (!haveOffset_ || offset - offset_ < 0) {   // the fastest delivery yet
      offset_ = offset;
      offsetAt_ = nowUs;
      haveOffset_ = true;
    } else if (nowUs - offsetAt_ > 1000000u) {    // age it 1 ms a second, so drift can't strand it
      offset_ += std::min<int32_t>(1000, offset - offset_);
      offsetAt_ = nowUs;
    }
    const uint32_t at = ms_ * 1000u + (uint32_t)offset_;
    return (int32_t)(at - nowUs) > 0 ? nowUs : at;   // never in the future
  }

  bool started_ = false, haveOffset_ = false;
  uint32_t lastTs_ = 0, ms_ = 0, offsetAt_ = 0;
  int32_t offset_ = 0;
};

// MIDI clock (24 per quarter) -> bpm, averaged over a beat's ticks. A gap
// of over half a second (the clock stopped) starts the average again.
class ClockTempo {
 public:
  // true when *bpm holds a new estimate (once per beat).
  bool tick(uint32_t nowUs, float* bpm) {
    if (count_ > 0 && nowUs - last_ > 500000u) count_ = 0;
    if (count_ == 0) { first_ = nowUs; }
    last_ = nowUs;
    if (++count_ <= kTicksPerBeat) return false;
    *bpm = 60.0e6f / (float)(nowUs - first_);
    first_ = nowUs;
    count_ = 1;
    return true;
  }

 private:
  static constexpr int kTicksPerBeat = 24;
  uint32_t first_ = 0, last_ = 0;
  int count_ = 0;
};

// Control changes -> the four controls (0 HARMONICS, 1 TIMBRE, 2 MORPH,
// 3 MACRO), or -1. General-purpose knobs 16..19 map in order; the common
// performance CCs land where they'd be expected.
inline int ccParam(uint8_t cc) {
  switch (cc) {
    case 16: case 71: return 0;   // GP 1, resonance -> HARMONICS
    case 17: case 74: return 1;   // GP 2, brightness -> TIMBRE
    case 18: case 1:  return 2;   // GP 3, mod wheel -> MORPH
    case 19: case 2:  return 3;   // GP 4, breath -> MACRO
    default: return -1;
  }
}

constexpr float kBendRange = 2.0f;   // semitones each way

// Handler: noteOn(note, vel), noteOff(note), sustain(bool), param(p, 0..1),
// bend(semitones), program(n), allOff(), tempo(bpm).
class In {
 public:
  template <typename H>
  void handle(const Msg& m, uint32_t nowUs, H& h) {
    switch (m.kind()) {
      case 0x90: if (m.d2) { h.noteOn(m.d1, m.d2); break; } [[fallthrough]];
      case 0x80: h.noteOff(m.d1); break;
      case 0xB0:
        if (m.d1 == 64) h.sustain(m.d2 >= 64);
        else if (m.d1 == 120 || m.d1 == 123) h.allOff();
        else if (ccParam(m.d1) >= 0) h.param(ccParam(m.d1), (float)m.d2 * (1.0f / 127.0f));
        break;
      case 0xE0: {
        const int v = ((int)m.d2 << 7 | m.d1) - 8192;
        h.bend((float)v * (kBendRange / 8192.0f));
        break;
      }
      case 0xC0: h.program(m.d1); break;
      case 0xF8: { float bpm; if (clock_.tick(nowUs, &bpm)) h.tempo(bpm); break; }
      default: break;
    }
  }

 private:
  ClockTempo clock_;
};

}  // namespace midi
}  // namespace pd
