// Host test for midi/midi.h: g++ -std=c++17 -O2 -I src/pd src/pd/midi/midi_test.cpp
#include "midi/midi.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace pd::midi;

static int failures = 0;
static void check(bool ok, const char* what) {
  if (!ok) { ++failures; printf("FAIL %s\n", what); }
}

struct Log {
  std::vector<std::string> got;
  void noteOn(int n, int v) { got.push_back("on " + std::to_string(n) + " " + std::to_string(v)); }
  void noteOff(int n) { got.push_back("off " + std::to_string(n)); }
  void sustain(bool d) { got.push_back(d ? "sus 1" : "sus 0"); }
  void param(int p, float v) { got.push_back("p" + std::to_string(p) + " " + std::to_string((int)std::lround(v * 100))); }
  void bend(float s) { got.push_back("bend " + std::to_string((int)std::lround(s * 100))); }
  void program(int n) { got.push_back("prog " + std::to_string(n)); }
  void allOff() { got.push_back("alloff"); }
  void echoMix(float v) { got.push_back("echo " + std::to_string((int)std::lround(v * 100))); }
  void fx(int b, float v) { got.push_back("fx" + std::to_string(b) + " " + std::to_string((int)std::lround(v * 100))); }
  void tempo(float bpm) { got.push_back("tempo " + std::to_string((int)std::lround(bpm))); }
};

static std::vector<std::string> run(const std::vector<uint8_t>& bytes) {
  Parser p; In in; Log log; Msg m;
  for (uint8_t b : bytes) if (p.feed(b, &m)) in.handle(m, 0, log);
  return log.got;
}

int main() {
  // Running status, note-on velocity 0 as note-off, a realtime byte
  // landing mid-message, SysEx skipped, any channel.
  auto g = run({0x93, 60, 100, 64, 90, 0xF8, 60, 0, 0xF0, 1, 2, 3, 0xF7, 0x85, 64, 0});
  const std::vector<std::string> want = {"on 60 100", "on 64 90", "off 60", "off 64"};
  check(g == want, "notes, running status, realtime and sysex interleaved");

  g = run({0xB0, 64, 127, 0xB0, 1, 127, 74, 0, 0xB2, 64, 10, 0xB0, 123, 0, 91, 127, 20, 127, 23, 0});
  check((g == std::vector<std::string>{"sus 1", "p2 100", "p1 0", "sus 0", "alloff", "echo 100", "fx0 100", "fx3 0"}), "CCs");

  g = run({0xE0, 0, 0x40, 0xE0, 0x7F, 0x7F, 0xE0, 0, 0});
  check((g == std::vector<std::string>{"bend 0", "bend 200", "bend -200"}), "bend centre and ends");

  g = run({0xC1, 3, 4});   // program change runs on with one data byte
  check((g == std::vector<std::string>{"prog 3", "prog 4"}), "program change, running status");

  g = run({0x40, 0x90, 0xF2, 1, 2, 60, 0x90, 60, 1});   // stray data, then SPP clears running status
  check((g == std::vector<std::string>{"on 60 1"}), "stray data and system common");

  // USB packets: cable 0 note-on, a lone clock byte, a SysEx packet.
  Msg m;
  const uint8_t on[4] = {0x09, 0x90, 61, 77}, clk[4] = {0x0F, 0xF8, 0, 0}, sx[4] = {0x04, 0xF0, 1, 2};
  check(fromUsbPacket(on, &m) && m.status == 0x90 && m.d1 == 61 && m.d2 == 77, "usb note-on");
  check(fromUsbPacket(clk, &m) && m.status == 0xF8, "usb clock");
  check(!fromUsbPacket(sx, &m), "usb sysex skipped");

  // Clock: 24 ticks per beat at 125 bpm (20 ms apart) -> 125; a stop
  // restarts the average instead of reading the gap as a slow beat.
  In in; Log log;
  uint32_t t = 1000;
  for (int i = 0; i < 49; ++i, t += 20000) in.handle(Msg{0xF8, 0, 0}, t, log);
  check((log.got == std::vector<std::string>{"tempo 125", "tempo 125"}), "clock tempo");
  log.got.clear();
  t += 2000000;
  for (int i = 0; i < 25; ++i, t += 25000) in.handle(Msg{0xF8, 0, 0}, t, log);
  check((log.got == std::vector<std::string>{"tempo 100"}), "clock restarts after a stop");

  // BLE-MIDI: header, timestamped note-on, running-status note-on without a
  // timestamp, a timestamped realtime byte, then a timestamped note-off.
  {
    BleDeframer ble; Parser p; In in; Log log; Msg m;
    const uint8_t pkt[] = {0x80, 0x81, 0x90, 60, 100, 62, 90, 0x82, 0xF8, 0x83, 0x80, 60, 0};
    ble.packet(pkt, sizeof pkt, 5000, [&](uint8_t b, uint32_t at) { if (p.feed(b, &m)) in.handle(m, at, log); });
    check((log.got == std::vector<std::string>{"on 60 100", "on 62 90", "off 60"}), "ble deframe");
    const uint8_t bad[] = {0x10, 0x81, 0x90, 1, 1};   // no header bit: not BLE-MIDI
    log.got.clear();
    ble.packet(bad, sizeof bad, 6000, [&](uint8_t b, uint32_t at) { if (p.feed(b, &m)) in.handle(m, at, log); });
    check(log.got.empty(), "ble rejects a packet without a header");
  }
  // BLE clock: ticks 20 ms apart by the sender's timestamps, but delivered
  // two to a packet every 40 ms with up to 9 ms of extra latency, and across
  // the 13-bit wrap. Tempo comes out at 125 from the timestamps (arrival
  // times alone would swing it by +-9 ms a beat, about +-2 bpm).
  {
    BleDeframer ble; Parser p; In in; Log log; Msg m;
    uint32_t ts = 8000, now = 100000;
    for (int k = 0; k < 600; ++k) {   // 48 s: six 8.192 s wraps
      const uint32_t t1 = ts & 0x1FFF, t2 = (ts + 20) & 0x1FFF;
      const uint8_t pkt[] = {(uint8_t)(0x80 | (t1 >> 7)), (uint8_t)(0x80 | (t1 & 0x7F)), 0xF8,
                             (uint8_t)(0x80 | (t2 & 0x7F)), 0xF8};
      const uint32_t late = (uint32_t)((k * 7919) % 9) * 1000;
      ble.packet(pkt, sizeof pkt, now + 20000 + late, [&](uint8_t b, uint32_t at) { if (p.feed(b, &m)) in.handle(m, at, log); });
      ts += 40; now += 40000;
    }
    // The first beat may read off while the latency minimum settles.
    bool ok = log.got.size() >= 45;
    for (size_t i = 1; i < log.got.size(); ++i) ok = ok && log.got[i] == "tempo 125";
    check(ok, "ble clock tempo from timestamps, through jitter and the wrap");
    if (!ok) for (auto& g : log.got) printf("  %s\n", g.c_str());
  }

  printf(failures ? "midi_test: %d FAILED\n" : "midi_test: ok\n", failures);
  return failures != 0;
}
