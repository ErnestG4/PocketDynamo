// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host renderer for PocketDynamo engines: plays a plaits-lab style scenario
// (controls ramped linearly start->end, optional periodic trigger) and writes
// a stereo 16-bit WAV, main on the left, aux on the right.
//
//   g++ -std=c++17 -O2 -I src/pd host/pd_render.cpp -o pd_render
//   ./pd_render <engine> <out.wav> <seconds> <note> <trigger_hz> <block>
//               <h0> <h1> <t0> <t1> <m0> <m1> <x0> <x1> [gain]
//
// Build with -DPD_SAMPLE_RATE=47872.34f to match Plaits-lab renders.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "dsp/core.h"
#include "engines/zx_phase.h"
#include "engines/zx_pulse.h"

namespace {

std::unique_ptr<pd::Engine> makeEngine(const char* id) {
  if (!strcmp(id, "zxpulse")) return std::make_unique<pd::ZxPulseEngine>();
  if (!strcmp(id, "zxphase")) return std::make_unique<pd::ZxPhaseEngine>();
  return nullptr;
}

void put16(FILE* f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
void put32(FILE* f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

bool writeWav(const char* path, const std::vector<int16_t>& lr, uint32_t rate) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const uint32_t bytes = (uint32_t)lr.size() * 2;
  fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16); put16(f, 1); put16(f, 2); put32(f, rate); put32(f, rate * 4);
  put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
  fwrite(lr.data(), 2, lr.size(), f);
  fclose(f);
  return true;
}

int16_t toPcm(float x) {
  x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
  return (int16_t)(x * 32767.0f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 15) {
    fprintf(stderr, "usage: %s engine out.wav seconds note trigger_hz block "
                    "h0 h1 t0 t1 m0 m1 x0 x1 [gain]\n", argv[0]);
    return 2;
  }
  auto engine = makeEngine(argv[1]);
  if (!engine) { fprintf(stderr, "unknown engine %s\n", argv[1]); return 2; }
  const float seconds = atof(argv[3]), note = atof(argv[4]), trigHz = atof(argv[5]);
  const size_t block = (size_t)atoi(argv[6]);
  float c[8];
  for (int i = 0; i < 8; ++i) c[i] = atof(argv[7 + i]);
  const float gain = argc > 15 ? (float)atof(argv[15]) : 0.8f;

  // Output at a nominal 48 kHz, as plaits-lab labels its previews.
  const uint32_t rate = 48000;
  const size_t total = (size_t)(seconds * rate);
  std::vector<int16_t> lr;
  lr.reserve(total * 2);
  std::vector<float> out(block), aux(block);
  double nextTrig = trigHz > 0.0f ? 0.0 : 1e30;
  float peak = 0.0f; double sumSq = 0.0; bool finite = true;
  for (size_t pos = 0; pos < total; pos += block) {
    const size_t n = std::min(block, total - pos);
    const float a = (float)pos / (float)total;
    pd::EngineParams p;
    p.note = note;
    p.harmonics = c[0] + (c[1] - c[0]) * a;
    p.timbre    = c[2] + (c[3] - c[2]) * a;
    p.morph     = c[4] + (c[5] - c[4]) * a;
    p.macro     = c[6] + (c[7] - c[6]) * a;
    const double t = (double)pos / rate;
    if (t >= nextTrig) { p.trigger = pd::kTriggerRising; nextTrig += 1.0 / trigHz; }
    engine->render(p, out.data(), aux.data(), n);
    for (size_t i = 0; i < n; ++i) {
      if (!std::isfinite(out[i]) || !std::isfinite(aux[i])) finite = false;
      const float m = out[i] * gain;
      peak = std::max(peak, std::fabs(m));
      sumSq += (double)m * m;
      lr.push_back(toPcm(m));
      lr.push_back(toPcm(aux[i] * gain));
    }
  }
  if (!writeWav(argv[2], lr, rate)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
  printf("%s peak=%.4f rms=%.5f finite=%d\n", argv[2], peak,
         std::sqrt(sumSq / (double)total), (int)finite);
  return finite ? 0 : 1;
}
