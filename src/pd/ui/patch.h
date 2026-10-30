// A patch: the sound and playing setup, as a small text file a person can
// read and edit on the SD card. Engines and surfaces are stored by name, so
// reordering the tables never breaks a saved patch; unknown or missing keys
// keep their defaults. The take goes alongside as a plain WAV (wav*()).
// Pure — host-testable.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pd {
namespace patch {

struct Patch {
  char  engine[16] = "ZX PULSE";
  char  surface[16] = "MAJOR 4THS";
  int   octave = 3;
  float root = 0.0f;
  float bpm = 120.0f;
  float params[4] = {0.0f, 0.75f, 0.0f, 0.5f};   // harmonics timbre morph macro
  char  echo[8] = "OFF";                         // echo time: OFF, 1/8, 1/8., 1/4
  float echoFeedback = 0.45f;
  float echoMix = 0.35f;
  char  tiltSide[12] = "TIMBRE";                 // what each tilt axis drives
  char  tiltFront[12] = "MORPH";
  char  fxType[3][8] = {"LP", "CRUSH", "FOLD"};  // TONE, GRIT, SHAPE
  float fxAmount[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // TONE, GRIT, SHAPE, SPACE
};

constexpr const char* kFxKeys[4] = {"fx_tone", "fx_grit", "fx_shape", "fx_space"};

constexpr const char* kParamKeys[4] = {"harmonics", "timbre", "morph", "macro"};

// Returns the text length written (always NUL-terminated, truncated to n).
inline int toText(const Patch& p, char* buf, size_t n) {
  return snprintf(buf, n,
                  "pocketdynamo patch 1\n"
                  "engine=%s\nsurface=%s\noctave=%d\nroot=%.4f\nbpm=%.1f\n"
                  "%s=%.4f\n%s=%.4f\n%s=%.4f\n%s=%.4f\n"
                  "echo=%s\necho_fb=%.3f\necho_mix=%.3f\ntilt_side=%s\ntilt_front=%s\n"
                  "fx_tone=%s %.3f\nfx_grit=%s %.3f\nfx_shape=%s %.3f\nfx_space=%.3f\n",
                  p.engine, p.surface, p.octave, p.root, p.bpm,
                  kParamKeys[0], p.params[0], kParamKeys[1], p.params[1],
                  kParamKeys[2], p.params[2], kParamKeys[3], p.params[3],
                  p.echo, p.echoFeedback, p.echoMix, p.tiltSide, p.tiltFront,
                  p.fxType[0], p.fxAmount[0], p.fxType[1], p.fxAmount[1], p.fxType[2], p.fxAmount[2], p.fxAmount[3]);
}

// Parses what toText wrote (or a hand edit of it). False if it isn't a patch.
inline bool fromText(const char* text, Patch* out) {
  if (strncmp(text, "pocketdynamo patch", 18) != 0) return false;
  Patch p = *out;
  const char* line = text;
  while (*line) {
    const char* end = strchr(line, '\n');
    const size_t len = end ? (size_t)(end - line) : strlen(line);
    char l[64];
    const size_t m = len < sizeof l - 1 ? len : sizeof l - 1;
    memcpy(l, line, m);
    l[m] = 0;
    if (m && l[m - 1] == '\r') l[m - 1] = 0;
    char* eq = strchr(l, '=');
    if (eq) {
      *eq = 0;
      const char* v = eq + 1;
      auto copy = [v](char* dst, size_t cap) { strncpy(dst, v, cap - 1); dst[cap - 1] = 0; };
      if (!strcmp(l, "engine")) copy(p.engine, sizeof p.engine);
      else if (!strcmp(l, "surface")) copy(p.surface, sizeof p.surface);
      else if (!strcmp(l, "octave")) p.octave = atoi(v);
      else if (!strcmp(l, "root")) p.root = (float)atof(v);
      else if (!strcmp(l, "bpm")) p.bpm = (float)atof(v);
      else if (!strcmp(l, "echo")) copy(p.echo, sizeof p.echo);
      else if (!strcmp(l, "echo_fb")) p.echoFeedback = (float)atof(v);
      else if (!strcmp(l, "echo_mix")) p.echoMix = (float)atof(v);
      else if (!strcmp(l, "tilt_side")) copy(p.tiltSide, sizeof p.tiltSide);
      else if (!strcmp(l, "tilt_front")) copy(p.tiltFront, sizeof p.tiltFront);
      else if (!strcmp(l, "fx_space")) p.fxAmount[3] = (float)atof(v);
      else for (int i = 0; i < 3; ++i)
        if (!strcmp(l, kFxKeys[i])) {   // "<TYPE> <amount>"
          char t[8] = {};
          float a = 0.0f;
          if (sscanf(v, "%7s %f", t, &a) >= 1) { memcpy(p.fxType[i], t, sizeof t); p.fxAmount[i] = a; }
        }
      else for (int i = 0; i < 4; ++i) if (!strcmp(l, kParamKeys[i])) p.params[i] = (float)atof(v);
    }
    line += len + (end ? 1 : 0);
  }
  *out = p;
  return true;
}

// --- WAV: mono 16-bit PCM ---

constexpr size_t kWavHeader = 44;

inline void put32(uint8_t* b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
inline void put16(uint8_t* b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
inline uint32_t get32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
inline uint16_t get16(const uint8_t* b) { return (uint16_t)(b[0] | b[1] << 8); }

inline void wavHeader(uint8_t* h, uint32_t rate, uint32_t samples) {
  memcpy(h, "RIFF", 4); put32(h + 4, 36 + samples * 2);
  memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
  put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, rate); put32(h + 28, rate * 2);
  put16(h + 32, 2); put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, samples * 2);
}

// Finds the PCM data in a WAV header block (first `n` bytes of the file).
// Accepts mono 16-bit PCM only; returns false otherwise.
inline bool wavParse(const uint8_t* h, size_t n, uint32_t* rate, uint32_t* dataOffset, uint32_t* samples) {
  if (n < 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
  bool fmtOk = false;
  for (size_t at = 12; at + 8 <= n;) {
    const uint32_t size = get32(h + at + 4);
    if (!memcmp(h + at, "fmt ", 4) && at + 24 <= n) {
      fmtOk = get16(h + at + 8) == 1 && get16(h + at + 10) == 1 && get16(h + at + 22) == 16;
      *rate = get32(h + at + 12);
    } else if (!memcmp(h + at, "data", 4)) {
      if (!fmtOk) return false;
      *dataOffset = (uint32_t)at + 8;
      *samples = size / 2;
      return true;
    }
    at += 8 + size + (size & 1);
  }
  return false;
}

}  // namespace patch
}  // namespace pd
