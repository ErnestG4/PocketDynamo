// Host map of ZX PHASE's HARMONICS x MORPH plane: renders each point fresh
// (struck, controls held) and scores how pitched it is with YIN's cumulative
// mean normalized difference (de Cheveigne & Kawahara 2002) or by spectral
// flatness (Wiener entropy of a Welch power spectrum, 100 Hz..12 kHz): ~0 =
// pitched, ~1 = hiss. YIN can't see periods past its 85 ms lag, and a slow
// beat reads as aperiodic, so flatness is the better ear proxy. Prints one
// ASCII map per note.
//
//   g++ -std=c++17 -O2 -I src/pd host/zx_phase_map.cpp -o zx_phase_map
//   ./zx_phase_map <yin|flat> [timbre] [macro] [notes...]
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dsp/core.h"
#include "engines/zx_phase.h"

namespace {

using cpx = std::complex<double>;

void fft(std::vector<cpx>& a, bool inverse) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = 2 * M_PI / (double)len * (inverse ? 1 : -1);
    const cpx wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cpx w(1);
      for (size_t k = 0; k < len / 2; ++k) {
        const cpx u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  if (inverse) for (auto& x : a) x /= (double)n;
}

constexpr int kWindow = 8192, kMaxLag = 4096, kMinLag = 8;

// min over lags of YIN's d'(tau), plus the RMS.
float aperiodicity(const std::vector<float>& x, float* rms) {
  const int m = kWindow - kMaxLag;
  std::vector<cpx> a(16384), b(16384);
  for (int i = 0; i < m; ++i) a[i] = x[i];
  for (int i = 0; i < kWindow; ++i) b[i] = x[i];
  fft(a, false); fft(b, false);
  for (size_t i = 0; i < a.size(); ++i) a[i] = std::conj(a[i]) * b[i];
  fft(a, true);
  std::vector<double> pre(kWindow + 1, 0.0);
  for (int i = 0; i < kWindow; ++i) pre[i + 1] = pre[i] + (double)x[i] * x[i];
  const double e0 = pre[m];
  *rms = (float)std::sqrt(e0 / m);
  double sum = 0, best = 1e9;
  for (int t = 1; t <= kMaxLag; ++t) {
    const double d = e0 + (pre[t + m] - pre[t]) - 2 * a[t].real();
    sum += d;
    const double dn = sum > 0 ? d * t / sum : 1.0;
    if (t >= kMinLag && dn < best) best = dn;
  }
  return (float)best;
}

// Wiener entropy of the averaged power spectrum, 4096-point Hann frames.
float flatness(const std::vector<float>& x, float* rms) {
  constexpr int kN = 4096;
  std::vector<double> power(kN / 2, 0.0);
  double e = 0;
  for (float v : x) e += (double)v * v;
  *rms = (float)std::sqrt(e / x.size());
  for (size_t start = 0; start + kN <= x.size(); start += kN / 2) {
    std::vector<cpx> a(kN);
    for (int i = 0; i < kN; ++i)
      a[i] = x[start + i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / kN));
    fft(a, false);
    for (int i = 0; i < kN / 2; ++i) power[i] += std::norm(a[i]);
  }
  const int lo = (int)(100.0f / pd::kSampleRate * kN), hi = (int)(12000.0f / pd::kSampleRate * kN);
  double logSum = 0, sum = 0;
  for (int i = lo; i < hi; ++i) { logSum += std::log(power[i] + 1e-20); sum += power[i]; }
  const int n = hi - lo;
  return (float)(std::exp(logSum / n) / (sum / n));
}

}  // namespace

int main(int argc, char** argv) {
  const bool yin = argc > 1 && argv[1][0] == 'y';
  const float timbre = argc > 2 ? atof(argv[2]) : 0.0f;
  const float macro = argc > 3 ? atof(argv[3]) : 0.5f;
  std::vector<float> notes;
  for (int i = 4; i < argc; ++i) notes.push_back(atof(argv[i]));
  if (notes.empty()) notes = {48, 60, 72};
  constexpr int kH = 48, kM = 32, kBlock = 32;
  const int total = 4 * kWindow + (int)(0.3f * pd::kSampleRate);
  double grand = 0; int bad = 0, count = 0;
  for (float note : notes) {
    printf("note %.0f  timbre %.2f macro %.2f   (x: HARMONICS 0..1, XOR|OR|AND; y: MORPH 1 top .. 0 bottom)\n",
           note, timbre, macro);
    std::vector<std::vector<float>> grid(kM, std::vector<float>(kH));
    for (int mi = 0; mi < kM; ++mi)
      for (int hi = 0; hi < kH; ++hi) {
        pd::ZxPhaseEngine e;
        pd::EngineParams p;
        p.note = note; p.timbre = timbre; p.macro = macro;
        p.harmonics = (hi + 0.5f) / kH;
        p.morph = (mi + 0.5f) / kM;
        std::vector<float> buf(total), aux(kBlock);
        for (int i = 0; i < total; i += kBlock) {
          p.trigger = i == 0 ? pd::kTriggerRising : 0;
          e.render(p, &buf[i], aux.data(), kBlock);
        }
        std::vector<float> w(buf.end() - (yin ? kWindow : 4 * kWindow), buf.end());
        float rms;
        float ap = yin ? aperiodicity(w, &rms) : flatness(w, &rms);
        if (rms < 0.01f) ap = 2.0f;  // silence
        grid[mi][hi] = ap;
        if (ap <= 1.5f) { grand += ap; bad += ap > 0.3f; }
        ++count;
      }
    for (int mi = kM - 1; mi >= 0; --mi) {
      printf("%4.2f |", (mi + 0.5f) / kM);
      for (int hi = 0; hi < kH; ++hi) {
        const float a = grid[mi][hi];
        putchar(a > 1.5f ? '_' : a < 0.05f ? ' ' : a < 0.15f ? '.' : a < 0.3f ? ':' : a < 0.5f ? 'o' : '#');
        if (hi % 16 == 15 && hi != kH - 1) putchar('|');
      }
      printf("|\n");
    }
  }
  printf("mean score %.3f, %d/%d points above 0.3   (' '<.05 '.'<.15 ':'<.3 'o'<.5 '#' noise '_' silent)\n",
         grand / count, bad, count);
}
