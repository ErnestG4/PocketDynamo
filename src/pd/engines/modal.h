// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Modal — a struck resonator bank: bells, mallets, kalimba, gongs, plucks.
// Ported from Kyklophoria's "Bongs" mode (core/kyk_resonate.h, Combust). Kyklophoria plays modes
// fitted to recordings; this plays parametric ratio sets only (no .kykm, no
// burst audio), so every object here is a formula or a textbook table.
//
// The bank: one two-pole recursion a mode, y = 2r cos w · y1 − r² · y2, at
// most 16 modes a voice, run mode-outer and two at a time so each mode's
// coefficients and state sit in registers for the block. A strike is a STATE,
// not an impulse: a second "strike bank" on the same poles is set to the two
// samples before n = 0 at each mode's gain and phase, rises under a 3 ms
// raised cosine (a rotating phasor, no trig per sample) and is then folded
// into the main state, which a linear bank allows exactly. With no fitted
// phases to use, the phases are a golden-ratio scramble (fundamental at sine
// phase): Kyklophoria measured 1.7x the onset energy from zero phase.
// Modes past 0.95 Nyquist are never played; modes under -90 dBFS are dropped
// (state zeroed) and cost nothing until the next strike.
//
// Controls:
//  HARMONICS  the object, morphing slot by slot (each partial gliding to its
//             neighbour's place in log frequency) through eight ratio sets:
//             string, stiff string, marimba bar, free bar (glockenspiel),
//             clamped tine (kalimba / Rhodes), membrane, church bell,
//             plate (gong). Each pure set holds over the middle 30% of its
//             step (Kyklophoria's SnapTo idea, softened).
//  TIMBRE     the mallet: felt at the centre (dark, fundamental-heavy) to
//             metal at the edge (bright). It sets the strike's mode gains — a
//             second-order mallet roll-off from 250 Hz to 32 kHz plus a
//             strike-position comb — so it shapes the NEXT strike and leaves
//             a ringing note alone, as a mallet would.
//  MORPH      decay, live: T60 of the fundamental 50 ms (pluck, damped bar)
//             to 20 s (bell), higher modes always shorter, and less so as it
//             rises (glassy at the top). Key-tracked a little. A ringing note
//             follows it with its state carried.
//  MACRO      noon is the clean bank. CW: a magnetic pickup (Rhodes physics,
//             §6.4 of the catalog) — the bank is the tine's displacement u, the
//             pole sees 1/(1 + ((u − h)/w)²), the coil the first difference of
//             that, through the coil's own resonant low-pass; turning it up
//             swings the tine harder into the field, so bark (h2 and up)
//             rises. Best on the tine set. CCW: the wash (§6.6), white noise
//             in four high octave bands under per-band envelopes struck with
//             the modes — the shimmer of a cymbal, gong or bell that the modes
//             alone miss. Both act live.
// aux is the clean bank, before pickup and wash.
//
// Note handling (§6.2/6.3): a re-strike at the same note adds to the ring —
// a hammer on a ringing bar — under a cap: if ring plus strike would pass
// kRingCap the ring is choked (through the pole radius, over the strike's
// 3 ms) and, past half, the strike shortened. A strike at another note
// leaves the old ring to a TAIL at its own frequencies under a 40 ms damper
// (Kyklophoria: carrying it onto the new note's poles was heard as a bend,
// dropping it as a pop); the tail keeps the 16 loudest modes of old tail and
// old ring together (Rings' rule for the share, an idea, not code). A pitch,
// HARMONICS or MORPH change under a ringing note rewrites every mode's state
// as the amplitude and phase it carries under the old pole and puts it back
// under the new one — the naive carry came back 28 dB loud on Kyklophoria.
// Any change of a pole's radius (choke, tail damper) rescales the second
// state sample likewise (Kyklophoria's Repole).
//
// Level: a fresh strike is scaled to kStrikeLevel on level() — the modes'
// summed amplitude, which bounds the peak, or less for a short ring of many
// modes whose scrambled phases never line up — and a mode that would decay
// before it is heard is struck up by what it loses (at most 3x). Measured: a
// fresh strike peaks 0.3–0.7 over most of the control space (low notes at
// the shortest MORPH sit lower). The ring cap bounds re-strikes, the pickup
// is soft-clipped to 0.7, the wash ducks the modes, and a knee over 0.8
// keeps everything strictly inside ±1.
//
// Cost (LX7; instruction counts from gcc 8.4 -O2 asm, cycles reasoned, not
// benched): the paired, two-sample loop is 20 instructions per 2 modes x 2
// samples, 5 a mode a sample, ≈ 6-8 cycles with FPU latency, so 16 live
// modes ≈ 100-130 cycles a sample. A strike's 3 ms and a steal's 60 ms tail
// each run a second bank; pickup ≈ 40 (its 1/(1 + z²) by Newton, since '/'
// is a __divsf3 call here), wash ≈ 50; a rebuild while a control moves is
// ≈ 75 flops a live mode, ≈ 40 a sample amortised over a 32-sample block.
#pragma once
#include "../dsp/core.h"
#include "../dsp/fastmath.h"

namespace pd {

namespace modal {

constexpr int kModes = 16;
constexpr int kSets = 8;

// A parametric object: mode-frequency ratios to the played note, the strike's
// amplitude law (index^-alpha: a strike is near an impulse and excites modes
// near equally; Kyklophoria's quarter-power law, steeper for strings), how
// steeply decay falls with frequency, and a T60 scale.
struct Body {
  float ratio[kModes];
  float alpha, decay_tilt, t60_scale;
};

// Free-free bar: ((β_n L)/4.730)², β_n L = 4.730, 7.853, 10.996, (2n+1)π/2.
// Clamped-free cantilever: ((β_n L)/1.875)², β_n L = 1.875, 4.694, 7.855, ...
// Membrane: ordered Bessel zeros over j01 (Kyklophoria's DrumRatios). Bell:
// a minor-third church bell by partial name (hum, prime, tierce, quint,
// nominal, ...; Rossing-style values, approximate). Plate: simply supported,
// sides 1 : 1.38, (m² + (n/1.38)²) sorted. Stiff string: k·√(1 + Bk²), B 0.003.
// Marimba: the first three tuned 1 : 4 : 10 by the undercut, the rest the free
// bar's spacing scaled on from there.
constexpr Body kBodies[kSets] = {
  {{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, 1.0f, 0.8f, 1.0f},          // string
  {{1.0f, 2.009f, 3.036f, 4.089f, 5.176f, 6.306f, 7.486f, 8.721f, 10.019f, 11.385f,
    12.823f, 14.338f, 15.935f, 17.616f, 19.384f, 21.243f}, 0.9f, 1.0f, 1.0f},           // stiff string
  {{1.0f, 3.98f, 9.92f, 16.4f, 24.6f, 34.3f, 45.7f, 58.6f, 73.2f, 89.6f, 107.6f,
    127.4f, 148.9f, 172.1f, 197.0f, 223.6f}, 0.7f, 1.4f, 0.6f},                         // marimba
  {{1.0f, 2.757f, 5.404f, 8.933f, 13.344f, 18.638f, 24.814f, 31.872f, 39.812f,
    48.635f, 58.34f, 68.927f, 80.397f, 92.748f, 105.982f, 120.099f}, 0.5f, 0.6f, 1.3f}, // free bar
  {{1.0f, 6.27f, 17.55f, 34.39f, 56.84f, 84.91f, 118.6f, 157.9f, 202.8f, 253.3f,
    309.5f, 371.2f, 438.6f, 511.6f, 590.2f, 674.4f}, 1.0f, 1.0f, 1.5f},                 // tine
  {{1.0f, 1.593f, 2.135f, 2.295f, 2.653f, 2.917f, 3.155f, 3.5f, 3.598f, 3.647f,
    4.06f, 4.154f, 4.23f, 4.601f, 4.832f, 4.903f}, 0.5f, 0.9f, 0.35f},                  // membrane
  {{0.5f, 1.0f, 1.183f, 1.506f, 2.0f, 2.514f, 2.662f, 3.011f, 3.567f, 4.166f,
    4.585f, 5.433f, 6.06f, 6.796f, 7.53f, 8.215f}, 0.35f, 0.45f, 1.6f},                 // bell
  {{1.0f, 2.033f, 2.967f, 3.754f, 4.0f, 5.722f, 6.165f, 6.246f, 7.278f, 8.132f,
    9.0f, 9.263f, 10.835f, 11.23f, 11.41f, 11.868f}, 0.3f, 0.35f, 1.4f},                // plate
};

constexpr int   kWashBands = 4;
constexpr float kWashHz[kWashBands] = {1200.0f, 2600.0f, 5400.0f, 11000.0f};

// Everything worked out once with libm, never on the audio path: log ratios,
// the phase scramble, filter coefficients, the ramp's rotation.
struct Tables {
  float lratio[kSets][kModes];     // log2 of each ratio
  float lt60[kSets];               // log2 of each T60 scale
  float lindex[kModes];            // log2(k + 1)
  float sph[kModes], cph[kModes];  // the strike phase, sin and cos
  float rot_c, rot_s;              // the 3 ms raised cosine's step, as a rotation
  float wb0[kWashBands], wa1[kWashBands], wa2[kWashBands], iwrms[kWashBands];   // iwrms: 1 / noise gain
  float pb0, pb1, pa1, pa2;        // the coil: RBJ low-pass (b2 = b0)
  float lw_hz;                     // log2(2π / sr): w of 1 Hz
  int   ramp_len;

  Tables() {
    lw_hz = std::log2(6.2831853f / kSampleRate);
    for (int s = 0; s < kSets; ++s) {
      for (int k = 0; k < kModes; ++k) lratio[s][k] = std::log2(kBodies[s].ratio[k]);
      lt60[s] = std::log2(kBodies[s].t60_scale);
    }
    for (int k = 0; k < kModes; ++k) {
      lindex[k] = std::log2((float)(k + 1));
      float ph = 6.2831853f * (float)k * 0.6180340f;
      ph -= 6.2831853f * (float)(int)(ph / 6.2831853f);
      sph[k] = std::sin(ph);
      cph[k] = std::cos(ph);
    }
    ramp_len = (int)(0.003f * kSampleRate + 0.5f);
    rot_c = std::cos(3.14159265f / (float)ramp_len);
    rot_s = std::sin(3.14159265f / (float)ramp_len);
    // Wash band-passes, RBJ constant 0 dB peak, Q 1.4 (about an octave). The
    // noise gain is exact: for 1/(1 + a1 z^-1 + a2 z^-2) the autocorrelation
    // r0, r1, r2 is closed form, and the (1 − z^-2) numerator makes it
    // b0² · 2(r0 − r2).
    for (int b = 0; b < kWashBands; ++b) {
      const float w0 = 6.2831853f * std::min(kWashHz[b], 0.45f * kSampleRate) / kSampleRate;
      const float alpha = std::sin(w0) / (2.0f * 1.4f), a0 = 1.0f + alpha;
      wb0[b] = alpha / a0;
      wa1[b] = -2.0f * std::cos(w0) / a0;
      wa2[b] = (1.0f - alpha) / a0;
      const float a1 = wa1[b], a2 = wa2[b];
      const float r0 = (1.0f + a2) / ((1.0f - a2) * ((1.0f + a2) * (1.0f + a2) - a1 * a1));
      const float r1 = -a1 * r0 / (1.0f + a2);
      const float r2 = -a1 * r1 - a2 * r0;
      iwrms[b] = 1.0f / (wb0[b] * std::sqrt(2.0f * (r0 - r2)));
    }
    // The coil's own resonance: 3.5 kHz (ModalEngine::kCoilW), Q 1.2.
    const float w0 = 6.2831853f * 3500.0f / kSampleRate;
    const float alpha = std::sin(w0) / (2.0f * 1.2f), cw = std::cos(w0), a0 = 1.0f + alpha;
    pb0 = (1.0f - cw) * 0.5f / a0;
    pb1 = (1.0f - cw) / a0;
    pa1 = -2.0f * cw / a0;
    pa2 = (1.0f - alpha) / a0;
  }
};

inline const Tables& tables() {
  static const Tables t;
  return t;
}

// Adds modes idx[0..count) into out over m samples: y = a·y1 + b·y2 with
// a = c1·dc, b = c2·dc² (dc < 1 is a damper pulling every pole in). Two modes
// a pass, Kyklophoria's pairing: two independent chains fill the FPU's
// latency, and the sums are the same either way. kWeighted multiplies each
// sample by w[k] on its way into out (the strike's ramp).
template <bool kWeighted>
inline void runModes(const uint8_t* idx, int count, const float* c1, const float* c2,
                     float* y1, float* y2, float dc, const float* w, float* out, int m) {
  const float dc2 = dc * dc;
  int i = 0;
  for (; i + 1 < count; i += 2) {
    const int j0 = idx[i], j1 = idx[i + 1];
    const float a0 = c1[j0] * dc, b0 = c2[j0] * dc2, a1 = c1[j1] * dc, b1 = c2[j1] * dc2;
    float p1 = y1[j0], p2 = y2[j0], q1 = y1[j1], q2 = y2[j1];
    // Two samples a pass, the state registers trading places instead of
    // being copied: four FP moves a sample fewer (LX7 asm, gcc 8.4).
    int k = 0;
    for (; k + 1 < m; k += 2) {
      p2 = a0 * p1 + b0 * p2;            // y[n]
      q2 = a1 * q1 + b1 * q2;
      p1 = a0 * p2 + b0 * p1;            // y[n + 1]
      q1 = a1 * q2 + b1 * q1;
      out[k] += kWeighted ? w[k] * (p2 + q2) : p2 + q2;
      out[k + 1] += kWeighted ? w[k + 1] * (p1 + q1) : p1 + q1;
    }
    if (k < m) {
      const float yp = a0 * p1 + b0 * p2;
      const float yq = a1 * q1 + b1 * q2;
      p2 = p1; p1 = yp;
      q2 = q1; q1 = yq;
      out[k] += kWeighted ? w[k] * (yp + yq) : yp + yq;
    }
    y1[j0] = p1; y2[j0] = p2; y1[j1] = q1; y2[j1] = q2;
  }
  if (i < count) {
    const int j = idx[i];
    const float a = c1[j] * dc, b = c2[j] * dc2;
    float u1 = y1[j], u2 = y2[j];
    for (int k = 0; k < m; ++k) {
      const float y = a * u1 + b * u2;
      u2 = u1; u1 = y;
      out[k] += kWeighted ? w[k] * y : y;
    }
    y1[j] = u1; y2[j] = u2;
  }
}

}  // namespace modal

class ModalEngine : public Engine {
 public:
  // After note-off the instrument's release; the bank's own decay (MORPH)
  // does the shaping while the key is held, so this is long enough for a
  // bell to ring out and short enough to free the voice.
  static constexpr float kReleaseMs = 3000.0f;

  ModalEngine() { reset(); }

  void reset() override {
    for (int k = 0; k < modal::kModes; ++k) {
      w_[k] = cw_[k] = sw_[k] = isw_[k] = r_[k] = c1_[k] = c2_[k] = 0.0f;
      y1_[k] = y2_[k] = s1_[k] = s2_[k] = 0.0f;
      tc1_[k] = tc2_[k] = ty1_[k] = ty2_[k] = 0.0f;
      live_[k] = false;
    }
    tail_n_ = tail_left_ = 0;
    choke_left_ = 0; choke_dc_ = 1.0f;
    ramping_ = false; ramp_n_ = 0; ramp_c_ = 1.0f; ramp_s_ = 0.0f;
    note_ = 1e9f; harm_ = morph_ = -1.0f;
    alpha_ = 1.0f;
    spread_ = 1.6f;
    lw_ = -4.0f;
    for (int b = 0; b < modal::kWashBands; ++b) env_[b] = wx1_[b] = wx2_[b] = wy1_[b] = wy2_[b] = 0.0f;
    noise_ = 0x9E3779B9u;
    pick_on_ = false; pick_prev_ = pz1_ = pz2_ = 0.0f;
    pick_d_ = 6.2831853f * 262.0f / kSampleRate;
  }

  void render(const EngineParams& p, float* out, float* aux, size_t n) override {
    const float note = clamp(p.note, -128.0f, 127.0f);
    const float harm = clamp(p.harmonics, 0.0f, 1.0f), timbre = clamp(p.timbre, 0.0f, 1.0f);
    const float morph = clamp(p.morph, 0.0f, 1.0f), macro = clamp(p.macro, 0.0f, 1.0f);

    if (p.trigger & kTriggerRising) {
      const bool same = std::fabs(note - note_) < 0.5f;
      if (!same) toTail();
      build(note, harm, morph, true, same);
      strike(timbre);
      for (int b = 0; b < modal::kWashBands; ++b) env_[b] = 1.0f;
    } else if (std::fabs(note - note_) > 1e-3f || std::fabs(harm - harm_) > 1.0f / 2048.0f ||
               std::fabs(morph - morph_) > 1.0f / 2048.0f) {
      build(note, harm, morph, false, true);
    }

    // The live list, once a block: a fold only ever moves state between banks
    // of modes that are already on it.
    uint8_t idx[modal::kModes];
    int count = 0;
    for (int k = 0; k < modal::kModes; ++k) if (live_[k]) idx[count++] = (uint8_t)k;

    for (size_t done = 0; done < n;) {
      int m = (int)std::min<size_t>(n - done, kRun);
      if (choke_left_ > 0) m = std::min(m, choke_left_);
      if (ramping_) m = std::min(m, modal::tables().ramp_len - ramp_n_);
      runBank(idx, count, out + done, m);
      done += (size_t)m;
    }

    float level = 0.0f;
    for (size_t i = 0; i < n; ++i) { aux[i] = out[i]; level += std::fabs(out[i]); }
    // Kyklophoria's guard: a voice gone non-finite, or far past its own
    // units, is reset rather than heard.
    if (!(level < 4.0f * (float)n)) {
      reset();
      for (size_t i = 0; i < n; ++i) out[i] = aux[i] = 0.0f;
      return;
    }

    cull();
    pickup(macro, out, n);
    wash(macro, timbre, morph, out, n);

    // A knee over 0.8: transparent below, never reaching 1.
    for (size_t i = 0; i < n; ++i) {
      const float a = std::fabs(out[i]);
      if (a > kKnee) {
        const float t = (a - kKnee) * (1.0f / (1.0f - kKnee));
        const float y = kKnee + (1.0f - kKnee) * t * fastmath::recip(1.0f + t);
        out[i] = out[i] < 0.0f ? -y : y;
      }
    }
  }

  // For the host test: the largest pole radius anywhere in the voice, from
  // the coefficients as they run (a choke's or a damper's included).
  float maxPoleRadius() const {
    float worst = 0.0f;
    const float dc = choke_dc_;
    for (int k = 0; k < modal::kModes; ++k) {
      worst = std::max(worst, poleRadius(c1_[k] * dc, c2_[k] * dc * dc));
      if (k < tail_n_) worst = std::max(worst, poleRadius(tc1_[k], tc2_[k]));
    }
    return worst;
  }
  int liveModes() const {
    int c = 0;
    for (int k = 0; k < modal::kModes; ++k) c += live_[k];
    return c;
  }
  int tailModes() const { return tail_n_; }

 private:
  static constexpr int   kRun = 64;            // the longest run with fixed state
  static constexpr float kStrikeLevel = 0.6f;  // a unit strike's level(), the gains' scale
  static constexpr float kRingCap = 0.78f;     // ring plus strike, level()
  static constexpr float kCull2 = 9e-10f;      // (3e-5)²: under -90 dBFS a mode stops
  static constexpr float kKnee = 0.8f;
  static constexpr float kTailMs = 40.0f;      // the damper on a stolen note: -60 dB over this
  static constexpr float kTailRunMs = 60.0f;   // ... and -90 dB by here, when it stops

  // The level of a set of modes from the sum and the sum of squares of their
  // amplitudes. The sum bounds the peak, and a long ring of inharmonic modes
  // drifts through every alignment and gets there; a short one is gone
  // before its scrambled phases line up and peaks nearer 1.6 × rms. So the
  // smaller of the two, the rms side's spread growing with the ring
  // (spread_: 1.6 for a fundamental ringing under 15 cycles, 2.4 from 22).
  float level(float sum, float sum2) const { return std::min(sum, spread_ * std::sqrt(sum2)); }

  static float poleRadius(float c1, float c2) {
    const float disc = c1 * c1 + 4.0f * c2;
    if (disc < 0.0f) return std::sqrt(-c2);
    const float s = std::sqrt(disc);
    return 0.5f * std::max(std::fabs(c1 + s), std::fabs(c1 - s));
  }

  // The modes' coefficients for a note and the HARMONICS/MORPH position.
  // all: every mode (a strike), else only those still ringing. carry: the
  // ringing state rewritten under the new poles as its amplitude and phase.
  void build(float note, float harm, float morph, bool all, bool carry) {
    const modal::Tables& t = modal::tables();
    note_ = note; harm_ = harm; morph_ = morph;

    // HARMONICS: eight sets, seven steps; each set holds over the middle
    // 30% of the travel around it and the steps between are smoothstepped.
    const float pos = harm * (float)(modal::kSets - 1);
    const int a = std::min((int)pos, modal::kSets - 2), b = a + 1;
    float u = clamp((pos - (float)a - 0.15f) * (1.0f / 0.7f), 0.0f, 1.0f);
    u = u * u * (3.0f - 2.0f * u);
    const modal::Body& A = modal::kBodies[a];
    const modal::Body& B = modal::kBodies[b];
    alpha_ = A.alpha + u * (B.alpha - A.alpha);
    const float tilt = (A.decay_tilt + u * (B.decay_tilt - A.decay_tilt)) * (1.25f - 0.75f * morph);

    // T60 of the fundamental, 50 ms .. 20 s, a little shorter up the keys.
    const float lt60 = -4.321928f + 8.643856f * morph + t.lt60[a] + u * (t.lt60[b] - t.lt60[a]) +
                       clamp((60.0f - note) * (1.0f / 36.0f), -1.5f, 1.5f);
    // log2 of the fundamental's w = 2π f / sr.
    const float lw = t.lw_hz + 8.7813597f + (note - 69.0f) * (1.0f / 12.0f);   // + log2(440)
    lw_ = lw;
    // level()'s spread from the fundamental's ring in cycles, T60 · f0.
    spread_ = 1.6f * fastmath::exp2(clamp(lt60 + lw - t.lw_hz - 3.9068906f, 0.0f, 0.5849625f));   // − log2(15)

    const float dc = choke_dc_;
    for (int k = 0; k < modal::kModes; ++k) {
      if (!all && !live_[k]) continue;
      const float lr = t.lratio[a][k] + u * (t.lratio[b][k] - t.lratio[a][k]);
      const float w = fastmath::exp2(lw + lr);
      if (!(w < kWMax)) {   // past 0.95 Nyquist: silent, state and strike cleared
        w_[k] = cw_[k] = sw_[k] = isw_[k] = r_[k] = c1_[k] = c2_[k] = 0.0f;
        y1_[k] = y2_[k] = s1_[k] = s2_[k] = 0.0f;
        live_[k] = false;
        continue;
      }
      // 10 ms .. 30 s, and never under three of the mode's own cycles: a
      // 30 Hz membrane at 35 ms was a thud that never reached its peak.
      const float cycles = 1.5849625f + t.lw_hz - (lw + lr);   // log2(3 / f)
      const float l60 = clamp(std::max(lt60 - tilt * lr, cycles), -6.643856f, 4.906891f);
      // Past 0.8 Nyquist a mode is damped harder the nearer it is to the
      // edge (to -60 dB in 14 ms at it), so one carried up and out by a
      // glide or a HARMONICS sweep has gone quiet before it is cut.
      const float edge = std::max(0.0f, w - kWEdge) * (0.01f / (kWMax - kWEdge));
      const float r = fastmath::expNegSmall((6.9077553f / kSampleRate) * fastmath::exp2(-l60) + edge);
      float sw, cw;
      fastmath::sinCos(w, &sw, &cw);
      if (carry && r_[k] > 0.0f) {
        // A sin φ and A cos φ under the old pole, written back under the new:
        // y1 = A sin φ, r·y2 = A sin(φ − w).
        const float inv = fastmath::recip(r);
        float S = y1_[k], C = (S * cw_[k] - r_[k] * dc * y2_[k]) * isw_[k];
        y2_[k] = (S * cw - C * sw) * inv * fastmath::recip(dc);
        S = s1_[k]; C = (S * cw_[k] - r_[k] * s2_[k]) * isw_[k];
        s2_[k] = (S * cw - C * sw) * inv;
      } else {
        y1_[k] = y2_[k] = s1_[k] = s2_[k] = 0.0f;
      }
      w_[k] = w; cw_[k] = cw; sw_[k] = sw; isw_[k] = fastmath::recip(std::max(sw, 1e-6f)); r_[k] = r;
      c1_[k] = 2.0f * r * cw;
      c2_[k] = -r * r;
    }
  }

  // The hammer: the strike bank set to every mode's gain and phase, ramped in
  // over 3 ms. What rings already stays and is added to, under the cap.
  void strike(float timbre) {
    const modal::Tables& t = modal::tables();
    if (ramping_) fold(0.5f - 0.5f * ramp_c_);   // a strike still coming in stays at the level heard
    endChoke();

    // The mallet: a second-order roll-off at 250 Hz .. 32 kHz, and the strike
    // position from 0.3 of the length (centre-ish) to 0.06 (near the end).
    const float wc = 6.2831853f * 250.0f * fastmath::exp2(7.0f * timbre) / kSampleRate;
    const float iwc2 = 1.0f / (wc * wc);
    const float x = 0.3f - 0.24f * timbre;
    float g[modal::kModes], sum = 0.0f, sum2 = 0.0f;
    for (int k = 0; k < modal::kModes; ++k) {
      g[k] = 0.0f;
      if (!(r_[k] > 0.0f)) continue;
      float pos = x * (float)(k + 1);
      pos -= (float)(int)pos;
      float sn, cs;
      fastmath::sinCos(3.14159265f * pos, &sn, &cs);
      const float comb = 0.4f + 0.6f * std::min(1.0f, 2.0f * sn);
      g[k] = fastmath::exp2(-alpha_ * t.lindex[k]) * comb * fastmath::recip(1.0f + w_[k] * w_[k] * iwc2);
      sum += g[k];
      sum2 += g[k] * g[k];
    }
    if (!(sum > 0.0f)) return;

    // The cap, on the level measure: if the ring and the strike would pass
    // it, choke the ring (to no less than half) over the ramp and shorten
    // the strike by what is still over.
    float ring = 0.0f, ring2 = 0.0f;
    for (int k = 0; k < modal::kModes; ++k)
      if (live_[k]) { const float a2 = amp2(k, 1.0f); ring += std::sqrt(a2); ring2 += a2; }
    ring = level(ring, ring2);
    float swing = kStrikeLevel;
    if (ring + swing > kRingCap) {
      const float keep = std::min(ring, std::max(kRingCap - swing, 0.5f * ring));
      swing = std::max(0.0f, std::min(swing, kRingCap - keep));
      if (keep < ring) {
        choke_left_ = t.ramp_len;
        choke_dc_ = fastmath::exp2(fastmath::log2(keep / ring) / (float)t.ramp_len);
        for (int k = 0; k < modal::kModes; ++k) y2_[k] /= choke_dc_;   // Repole: r → r·dc
      }
    }
    const float scale = swing / level(sum, sum2);
    const float half = -0.5f * (float)t.ramp_len;
    float gs = 0.0f, gd = 0.0f;
    for (int k = 0; k < modal::kModes; ++k) {
      if (!(g[k] > 0.0f)) continue;
      // A mode that decays before it is heard is struck up by what it loses
      // there: half the ramp plus a quarter of its own cycle, where a sine
      // from rest first peaks; at most 3x (a 30 Hz membrane at the shortest
      // MORPH was a thud at a tenth of the level otherwise).
      const float lag = half - 1.5707963f * fastmath::recip(w_[k]);
      const float comp = std::min(3.0f, fastmath::exp2(lag * fastmath::log2(r_[k])));
      // g r^n sin(n w + φ) from n = 0: the two samples before it.
      const float gk = g[k] * scale * comp, cw = cw_[k], sw = sw_[k], ir = fastmath::recip(r_[k]);
      const float s1w = t.sph[k] * cw - t.cph[k] * sw;   // sin(φ − w)
      const float c1w = t.cph[k] * cw + t.sph[k] * sw;   // cos(φ − w)
      s1_[k] = gk * s1w * ir;
      s2_[k] = gk * (s1w * cw - c1w * sw) * ir * ir;      // sin(φ − 2w) / r²
      live_[k] = true;
      // What the pickup's difference makes of this mode, 2 sin(w/2), under
      // the coil's corner: the strike's weighted mean is the pickup's gain.
      float sh, ch;
      fastmath::sinCos(0.5f * w_[k], &sh, &ch);
      const float q = w_[k] * (1.0f / kCoilW);
      gs += gk;
      gd += gk * 2.0f * sh / (1.0f + q * q);
    }
    // The pickup's gain: between the strike's weighted mean (the onset's
    // peak) and the note's own fundamental (the sustain's level), the
    // geometric mean of the two; floored at 60 Hz.
    if (gs > 0.0f) {
      float sh, ch;
      fastmath::sinCos(std::min(0.5f * fastmath::exp2(lw_), 1.5f), &sh, &ch);
      pick_d_ = std::max(std::sqrt(gd / gs * 2.0f * sh), 6.2831853f * 60.0f / kSampleRate);
    }
    ramping_ = true;
    ramp_n_ = 0;
    ramp_c_ = 1.0f; ramp_s_ = 0.0f;
  }

  // A mode's squared amplitude in the main bank, read under radius r·dc.
  float amp2(int k, float dc) const {
    const float a = y1_[k], b = r_[k] * dc * y2_[k];
    return (a * a + b * b - 2.0f * a * b * cw_[k]) * isw_[k] * isw_[k];
  }

  // The strike bank added into the main state at weight wt (the ramp's, or 1
  // at its end); a choke under way runs the main state at r·dc, so the second
  // sample is moved to that radius.
  void fold(float wt) {
    const float id = fastmath::recip(choke_dc_);
    for (int k = 0; k < modal::kModes; ++k) {
      y1_[k] += wt * s1_[k];
      y2_[k] += wt * s2_[k] * id;
      s1_[k] = s2_[k] = 0.0f;
    }
    ramping_ = false;
  }

  void endChoke() {
    choke_left_ = 0;
    if (choke_dc_ == 1.0f) return;
    for (int k = 0; k < modal::kModes; ++k) y2_[k] *= choke_dc_;   // Repole: r·dc → r
    choke_dc_ = 1.0f;
  }

  // A strike at another note: the ring goes to the tail at its own poles under
  // a damper, merged with any tail still sounding; the 16 loudest stay.
  void toTail() {
    if (ramping_) fold(0.5f - 0.5f * ramp_c_);
    endChoke();
    const float td = fastmath::exp2(-9.9657843f / (kTailMs * 0.001f * kSampleRate));   // 1e-3 over kTailMs
    float cc1[2 * modal::kModes], cc2[2 * modal::kModes], cy1[2 * modal::kModes], cy2[2 * modal::kModes];
    float e[2 * modal::kModes];
    int m = 0;
    for (int k = 0; k < modal::kModes; ++k) {
      if (live_[k]) {
        const float a2 = amp2(k, 1.0f);
        if (a2 > kCull2) {
          cc1[m] = c1_[k] * td; cc2[m] = c2_[k] * td * td;
          cy1[m] = y1_[k]; cy2[m] = y2_[k] / td;   // Repole: r → r·td
          e[m++] = a2;
        }
      }
      y1_[k] = y2_[k] = 0.0f;
      live_[k] = false;
    }
    if (tail_left_ > 0) {
      for (int k = 0; k < tail_n_; ++k) {
        const float r = std::sqrt(std::max(-tc2_[k], 0.0f));
        if (!(r > 0.0f)) continue;
        const float cw = tc1_[k] / (2.0f * r), s2 = std::max(1.0f - cw * cw, 1e-12f);
        const float a = ty1_[k], b = r * ty2_[k];
        const float a2 = (a * a + b * b - 2.0f * a * b * cw) / s2;
        if (a2 > kCull2) { cc1[m] = tc1_[k]; cc2[m] = tc2_[k]; cy1[m] = ty1_[k]; cy2[m] = ty2_[k]; e[m++] = a2; }
      }
    }
    // The loudest 16, by insertion into a descending order.
    int ord[2 * modal::kModes];
    for (int i = 0; i < m; ++i) {
      int j = i;
      while (j > 0 && e[ord[j - 1]] < e[i]) { ord[j] = ord[j - 1]; --j; }
      ord[j] = i;
    }
    tail_n_ = std::min(m, modal::kModes);
    for (int i = 0; i < tail_n_; ++i) {
      const int s = ord[i];
      tc1_[i] = cc1[s]; tc2_[i] = cc2[s]; ty1_[i] = cy1[s]; ty2_[i] = cy2[s];
    }
    tail_left_ = tail_n_ > 0 ? (int)(kTailRunMs * 0.001f * kSampleRate) : 0;
  }

  // One run of m <= kRun samples with the choke and the ramp fixed through it.
  void runBank(const uint8_t* idx, int count, float* out, int m) {
    for (int k = 0; k < m; ++k) out[k] = 0.0f;
    const float dc = choke_dc_;
    modal::runModes<false>(idx, count, c1_, c2_, y1_, y2_, dc, nullptr, out, m);
    if (ramping_) {
      // The raised cosine as a rotating phasor, renormalised once a run.
      const modal::Tables& t = modal::tables();
      float w[kRun];
      float c = ramp_c_, s = ramp_s_;
      const float g = 1.5f - 0.5f * (c * c + s * s);
      c *= g; s *= g;
      for (int k = 0; k < m; ++k) {
        const float c2 = c * t.rot_c - s * t.rot_s;
        s = s * t.rot_c + c * t.rot_s;
        c = c2;
        w[k] = 0.5f - 0.5f * c;
      }
      ramp_c_ = c; ramp_s_ = s;
      modal::runModes<true>(idx, count, c1_, c2_, s1_, s2_, 1.0f, w, out, m);
      ramp_n_ += m;
    }
    if (tail_n_ > 0) {
      static const uint8_t kAll[modal::kModes] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
      modal::runModes<false>(kAll, tail_n_, tc1_, tc2_, ty1_, ty2_, 1.0f, nullptr, out, m);
      tail_left_ -= m;
      if (tail_left_ <= 0) { tail_n_ = 0; tail_left_ = 0; }
    }
    // The choke ends before the fold, so the fold lands at radius r.
    if (choke_left_ > 0 && (choke_left_ -= m) <= 0) endChoke();
    if (ramping_ && ramp_n_ >= modal::tables().ramp_len) fold(1.0f);
  }

  // Modes under -90 dBFS stop (their state zeroed) until the next strike.
  void cull() {
    if (ramping_) return;
    for (int k = 0; k < modal::kModes; ++k)
      if (live_[k] && amp2(k, choke_dc_) < kCull2) { y1_[k] = y2_[k] = 0.0f; live_[k] = false; }
  }

  // MACRO clockwise: the bank read through a magnetic pickup. Flux
  // φ = 1/(1 + (x − h)²) with x the tine's swing into the field, the coil the
  // first difference of φ normalised back to the fundamental's level, then the
  // coil's resonant low-pass and a cubic soft clip. Crossfaded in over the
  // first quarter of the travel, the swing rising with the rest of it.
  void pickup(float macro, float* io, size_t n) {
    const float amt = macro > 0.51f ? (macro - 0.51f) * (1.0f / 0.49f) : 0.0f;
    if (amt <= 0.0f) { pick_on_ = false; return; }
    const modal::Tables& t = modal::tables();
    const float mix = std::min(1.0f, 4.0f * amt);
    const float kx = (0.7f + 2.8f * amt) * (1.0f / kStrikeLevel);
    // Back to the bank's level: the small-signal slope of φ at rest,
    // 2h/(1 + h²)² at h = 0.6, times what the coil's difference does to the
    // last strike (pick_d_, see strike()).
    const float norm = fastmath::recip(0.6488f * kx * pick_d_);
    constexpr float h = 0.6f;
    if (!pick_on_) {   // from rest at the tine's actual place: no step
      const float z = io[0] * kx - h;
      pick_prev_ = 1.0f / (1.0f + z * z);
      pz1_ = pz2_ = 0.0f;
      pick_on_ = true;
    }
    float prev = pick_prev_, z1 = pz1_, z2 = pz2_;
    for (size_t i = 0; i < n; ++i) {
      const float u = io[i];
      const float z = u * kx - h;
      const float phi = fastmath::recip(1.0f + z * z);   // no divide per sample
      const float d = (phi - prev) * norm;
      prev = phi;
      float y = t.pb0 * d + z1;                        // transposed direct form II
      z1 = t.pb1 * d - t.pa1 * y + z2;
      z2 = t.pb0 * d - t.pa2 * y;
      // Cubic soft clip to ±0.7: unity slope at 0, flat at ±1.05. The
      // difference makes an onset spike the sustain never has; this is where
      // a hard strike's bark compresses.
      y = clamp(y, -1.05f, 1.05f);
      y -= (4.0f / 27.0f / 0.49f) * y * y * y;
      io[i] = u + mix * (y - u);
    }
    pick_prev_ = prev;
    pz1_ = std::fabs(z1) < 1e-20f ? 0.0f : z1;
    pz2_ = std::fabs(z2) < 1e-20f ? 0.0f : z2;
  }

  // MACRO counter-clockwise: the wash. White noise in four high octave bands,
  // each under an envelope struck to 1 with the modes and falling at its own
  // T60 (shorter up the bands, following MORPH); TIMBRE tilts the bands.
  // The envelopes fall whether or not the wash is heard.
  void wash(float macro, float timbre, float morph, float* io, size_t n) {
    const modal::Tables& t = modal::tables();
    const float amt = macro < 0.49f ? (0.49f - macro) * (1.0f / 0.49f) : 0.0f;
    const float l60 = clamp(-5.0f + 7.5f * morph, -5.0f, 3.0f);   // 30 ms .. 8 s at the lowest band
    const float inv_n = fastmath::recip((float)n);
    float g0[modal::kWashBands], dg[modal::kWashBands];
    bool any = false;
    for (int b = 0; b < modal::kWashBands; ++b) {
      const float e0 = env_[b];
      if (e0 <= 0.0f) { g0[b] = dg[b] = 0.0f; continue; }
      float e1 = e0 * fastmath::exp2(-9.9657843f * (float)n * (1.0f / kSampleRate) * fastmath::exp2(-(l60 - 0.5f * (float)b)));
      if (e1 < 1e-4f) e1 = 0.0f;
      env_[b] = e1;
      // rms 0.03 a band at full (uniform noise is rms 0.577), tilted ±6 dB.
      const float lvl = amt * (0.03f / 0.57735f) * t.iwrms[b] *
                        fastmath::exp2((timbre - 0.5f) * ((float)b - 1.5f) * 1.33f);
      g0[b] = lvl * e0;
      dg[b] = lvl * (e1 - e0) * inv_n;
      if (amt > 0.0f) any = true;
    }
    if (amt <= 0.0f) return;
    // The modes make room for the wash, down to 0.8 at full; with the knob
    // and not the envelopes, so a wash dying out is not a step in the ring.
    const float duck = 1.0f - 0.2f * amt;
    if (!any) {
      for (size_t i = 0; i < n; ++i) io[i] *= duck;
      return;
    }
    for (size_t i = 0; i < n; ++i) {
      noise_ ^= noise_ << 13; noise_ ^= noise_ >> 17; noise_ ^= noise_ << 5;
      const float x = (float)(int32_t)noise_ * (1.0f / 2147483648.0f);
      float acc = 0.0f;
      for (int b = 0; b < modal::kWashBands; ++b) {
        const float y = t.wb0[b] * (x - wx2_[b]) - t.wa1[b] * wy1_[b] - t.wa2[b] * wy2_[b];
        wx2_[b] = wx1_[b]; wx1_[b] = x; wy2_[b] = wy1_[b]; wy1_[b] = y;
        acc += g0[b] * y;
        g0[b] += dg[b];
      }
      io[i] = duck * io[i] + acc;
    }
  }

  static constexpr float kWMax = 0.95f * 3.14159265f;
  static constexpr float kWEdge = 0.8f * 3.14159265f;
  static constexpr float kCoilW = 6.2831853f * 3500.0f / kSampleRate;   // the coil's corner, as w

  // Per mode: the pole (w, its sin and cos, 1/sin, radius, coefficients),
  // the main state, the strike bank's state, and the tail's own bank.
  float w_[modal::kModes], cw_[modal::kModes], sw_[modal::kModes], isw_[modal::kModes];
  float r_[modal::kModes], c1_[modal::kModes], c2_[modal::kModes];
  float y1_[modal::kModes], y2_[modal::kModes], s1_[modal::kModes], s2_[modal::kModes];
  float tc1_[modal::kModes], tc2_[modal::kModes], ty1_[modal::kModes], ty2_[modal::kModes];
  bool  live_[modal::kModes];
  int   tail_n_, tail_left_;
  int   choke_left_;             // samples of the re-strike choke still to run
  float choke_dc_;               // its per-sample pull on the main bank's poles, 1 when none
  bool  ramping_;
  int   ramp_n_;
  float ramp_c_, ramp_s_;        // the raised cosine's phasor
  float note_, harm_, morph_;    // what the coefficients were built for
  float alpha_;                  // the strike's amplitude law at this HARMONICS
  float spread_;                 // level()'s allowance for scrambled phases
  float lw_;                     // log2 of the note's w
  float env_[modal::kWashBands], wx1_[modal::kWashBands], wx2_[modal::kWashBands];
  float wy1_[modal::kWashBands], wy2_[modal::kWashBands];
  uint32_t noise_;
  bool  pick_on_;
  float pick_prev_, pz1_, pz2_;
  float pick_d_;                 // the coil's difference on the last strike, see pickup()
};

}  // namespace pd
