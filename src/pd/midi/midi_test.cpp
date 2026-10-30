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

  g = run({0xB0, 64, 127, 0xB0, 1, 127, 74, 0, 0xB2, 64, 10, 0xB0, 123, 0});
  check((g == std::vector<std::string>{"sus 1", "p2 100", "p1 0", "sus 0", "alloff"}), "CCs");

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

  printf(failures ? "midi_test: %d FAILED\n" : "midi_test: ok\n", failures);
  return failures != 0;
}
