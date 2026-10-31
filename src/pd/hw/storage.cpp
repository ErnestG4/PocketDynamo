// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include "storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

namespace pd {
namespace storage {
namespace {

// Cardputer ADV microSD. The display owns SPI3, so the card takes SPI2.
constexpr int kSck = 40, kMiso = 39, kMosi = 14, kCs = 12;
constexpr uint32_t kHz = 25000000;
constexpr const char* kDir = "/pocketdynamo";

bool g_mounted = false;

void path(char* buf, size_t n, int slot, const char* ext) { snprintf(buf, n, "%s/p%d.%s", kDir, slot, ext); }

}  // namespace

bool begin() {
  SPI.begin(kSck, kMiso, kMosi, kCs);
  // Some cards won't start at 25 MHz on these traces; fall back to 4 MHz.
  for (uint32_t hz : {kHz, 4000000u}) {
    g_mounted = SD.begin(kCs, SPI, hz);
    if (g_mounted) break;
    SD.end();
  }
  if (g_mounted && !SD.exists(kDir)) SD.mkdir(kDir);
  return g_mounted;
}

void report() {
  static const char* kTypes[] = {"none", "MMC", "SD", "SDHC", "unknown"};
  const uint8_t t = g_mounted ? SD.cardType() : 0;
  // (No used-space figure: FAT computes it by scanning the whole table,
  // which blocks the control task for seconds on a big card.)
  Serial.printf("sd mounted=%d type=%s size=%lluMB\n", g_mounted, kTypes[t < 5 ? t : 4],
                g_mounted ? SD.cardSize() >> 20 : 0ull);
}

bool mounted() { return g_mounted; }

bool save(int slot, const patch::Patch& p, const SampleStore& take) {
  if (!g_mounted) return false;
  char name[40], text[512];
  path(name, sizeof name, slot, "txt");
  File f = SD.open(name, FILE_WRITE);
  if (!f) return false;
  const int n = patch::toText(p, text, sizeof text);
  const bool ok = f.write((const uint8_t*)text, n) == (size_t)n;
  f.close();
  if (!ok) return false;

  path(name, sizeof name, slot, "wav");
  if (!take.ready()) { if (SD.exists(name)) SD.remove(name); return true; }
  File w = SD.open(name, FILE_WRITE);
  if (!w) return false;
  uint8_t h[patch::kWavHeader];
  patch::wavHeader(h, SampleStore::kRate, take.length());
  bool wok = w.write(h, sizeof h) == sizeof h;
  // The take is stored as recorded (12-bit, written as plain 16-bit PCM);
  // its normalization is redone on load.
  int16_t chunk[256];
  for (uint32_t i = 0; wok && i < take.length(); i += 256) {
    const uint32_t n = std::min<uint32_t>(256, take.length() - i);
    for (uint32_t j = 0; j < n; ++j) chunk[j] = take.at(i + j);
    wok = w.write((const uint8_t*)chunk, n * 2) == n * 2;
  }
  w.close();
  return wok;
}

bool load(int slot, patch::Patch* p, SampleStore& take, bool* tookTake) {
  *tookTake = false;
  if (!g_mounted) return false;
  char name[40], text[512];
  path(name, sizeof name, slot, "txt");
  File f = SD.open(name, FILE_READ);
  if (!f) return false;
  const size_t n = f.read((uint8_t*)text, sizeof text - 1);
  f.close();
  text[n] = 0;
  if (!patch::fromText(text, p)) return false;

  path(name, sizeof name, slot, "wav");
  File w = SD.open(name, FILE_READ);
  if (!w) return true;   // a patch with no take keeps the current one
  uint8_t h[256];
  const size_t hn = w.read(h, sizeof h);
  uint32_t rate = 0, offset = 0, samples = 0;
  if (!patch::wavParse(h, hn, &rate, &offset, &samples) || rate < 4000) { w.close(); return true; }
  w.seek(offset);
  take.beginLoad();
  delay(2);   // let the audio core finish a block that may still be reading
  // Resample to the store's rate on the fly (linear), in 512-sample chunks.
  const float step = (float)rate / (float)SampleStore::kRate;
  int16_t chunk[512];
  uint32_t chunkStart = 0, chunkLen = 0, out = 0;
  float pos = 0.0f;
  while (out < SampleStore::kCapacity) {
    const uint32_t i = (uint32_t)pos;
    if (i + 1 >= samples) break;
    while (i + 1 >= chunkStart + chunkLen) {   // slide the window forward
      if (chunkLen >= 2) { chunk[0] = chunk[chunkLen - 2]; chunk[1] = chunk[chunkLen - 1]; chunkStart += chunkLen - 2; chunkLen = 2; }
      else { chunkStart = 0; chunkLen = 0; }
      const size_t got = w.read((uint8_t*)(chunk + chunkLen), (sizeof chunk / 2 - chunkLen) * 2) / 2;
      if (!got) break;
      chunkLen += got;
    }
    if (i + 1 >= chunkStart + chunkLen) break;
    const float frac = pos - (float)i;
    const float a = chunk[i - chunkStart], b = chunk[i + 1 - chunkStart];
    take.put(out++, (int)(a + frac * (b - a)));
    pos += step;
  }
  w.close();
  *tookTake = take.finishLoad(out);
  return true;
}

}  // namespace storage
}  // namespace pd
