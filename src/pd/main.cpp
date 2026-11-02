// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// PocketDynamo — playable: keyboard -> event queue -> instrument -> codec.
// Core 1: pd_audio render task (hw/audio_io), drains the event queue each
// block. Core 0: control task — keyboard, tilt, display, serial. The Arduino
// loop task is deleted so nothing else competes on core 1.
//
// Keys. Columns 1..12 of all four rows play notes, laid out by the current
// surface (ui/surface.h). The outer columns are controls:
//   `      dice: tap mutates the four controls, hold rolls new ones
//   TAB    next surface                    BS     strip: NOTES/CHORD/RATCHET/ARP
//   FN     function layer                  \      next engine
//   CTRL   tilt on/off                     ENTER  all notes off
//                                          SPACE  sustain pedal
// Strips: in CHORD, RATCHET or ARP the top row's 12 keys stop playing notes.
// Hold one and the notes you play take its chord, re-strike at its rate, or
// arpeggiate in its pattern (ui/arp.h). Double-tap a strip key to latch it;
// tap it again to let go.
// Tilt: while on, the device's attitude drives two controls directly
// (ui/tilt.h): side to side (0 on its left edge, 1 on its right) and front/
// back (0 flat, 0.8 standing up, 1 tipped past upright), TIMBRE and MORPH by
// default. Hold CTRL and tap a control's FN key to reroute an axis: the left
// key of each pair takes side-to-side, the right front/back (z/x HARMONICS,
// c/v TIMBRE, b/n MORPH, m/, MACRO, d/f echo feedback, s/g echo mix).
// Switching tilt off returns the controls to their set values.
// FN: z/x harmonics, c/v timbre, b/n morph, m/, macro (or, by page, the FX,
// ENV or LFO bars), [/] volume (0 dB to start, up to +12 dB),
// q/w root down/up a step, 9/0 tempo, E echo time (off/1/8/1/8./1/4), D/F echo feedback, S/G echo mix,
// P cycles the pages SOUND -> FX -> ENV -> LFO (z.., then edit TONE / GRIT /
// SHAPE / SPACE, the ADSR, or the LFO's RATE / DEPTH / SHAPE / DELAY; T/Y/U
// pick the effect in TONE / GRIT / SHAPE: dsp/fx.h), A ENV on/off (off, each
// engine keeps its own envelope), O the LFO free or synced to the tempo.
// On the LFO page CTRL + a control's key routes the LFO instead of tilt
// (t/y/u/i the FX bars, q/w pitch). ` / 1 octave, TAB previous surface,
// \ previous engine,
// BS undo the last dice, hold R to record from the mic (into SAMPLE/CLOUD's
// take, up to 2.5 s), 2..8 load a patch from SD, SHIFT+2..8 save one,
// L looper record -> play <-> overdub (synth/looper.h), K stop / clear. Holding a param, volume, root or tempo combo repeats.
// MIDI in over USB (hw/usb_midi, midi/midi.h): notes play as virtual keys,
// so strips, sustain and the looper treat them like keys; CC 64 sustain,
// CC 16-19 (and 71/74/2) the four controls, 20-23 the FX bars, 24-27 the
// ENV bars, 28-31 the LFO bars (and 1, 72-78), 7 volume, 12/13/91 echo
// feedback/time/mix, 102-105 the four bars of the page on screen (with
// pickup), 106/107 the page (ui/controls.h), pitch bend +-2, program change
// picks the engine, MIDI clock sets the tempo.
// Serial: [ / ] volume, i = print state, B = silent CPU bench, S = SD status, K<row><col
// hex><+|-> = press/release a key, M<hex>. = raw MIDI bytes (for scripted
// tests), U = reboot into the bootloader (USB-MIDI builds, to flash),
// L = the licence notice and the full licence text.
#include <Arduino.h>
#include <M5Cardputer.h>
#include <cstring>
#include <new>

#include "dsp/ars.h"
#include "dsp/rng.h"
#include "dsp/tables.h"
#include "hw/audio_io.h"
#include "hw/keys.h"
#include "hw/storage.h"
#include "hw/ble_midi.h"
#include "hw/usb_midi.h"
#include "midi/midi.h"
#include "synth/dice.h"
#include "synth/events.h"
#include "synth/instrument.h"
#include "synth/looper.h"
#include "ui/arp.h"
#include "ui/controls.h"
#include "ui/keymap.h"
#include "ui/surface.h"
#include "ui/tilt.h"

#ifndef PD_VERSION
#define PD_VERSION "dev"
#endif
#ifndef PD_GIT
#define PD_GIT "unknown"
#endif

// The AGPL's "Appropriate Legal Notices" (section 5(d)): shown on the boot
// screen and over serial, with the licence text itself in the binary.
#define PD_SOURCE_URL "github.com/ErnestG4/PocketDynamo"
extern const char kLicenseText[] asm("_binary_LICENSE_start");

// Explicit, so GCC honours render()'s PD_HOT (IRAM) section: it drops the
// attribute on an implicit instantiation of a class-template member.
template class pd::Instrument<8, pd::audio::kBlock>;

namespace {

constexpr int kVoices = 8;
using Synth = pd::Instrument<kVoices, pd::audio::kBlock>;

Synth g_synth;
pd::TapeEcho g_echo;   // the master bus's, outside the pool so a bench pool stays small
pd::MasterFx g_fx;
pd::SpscQueue<128> g_events;
pd::Looper g_looper;

void render(const float* in, float* out, size_t n) {
  pd::sampleStore().record(in, n);   // no-op unless a take is running
  // Strike budget: at most kStrikesPerBlock note-ons start per block; the
  // rest wait in the queue (in order) for the next block, 0.67 ms later. A
  // chord's setup cost (MODAL's strike state, FM's coefficients) then never
  // lands on one block all at once. A MODAL strike builds a resonator bank
  // and counts double: four of them on top of eight ringing voices was the
  // one dropout in a 20-minute soak. A voice that builds a new engine (the
  // first notes after an engine change) counts one more: its first block
  // runs cold, and eight of them at once peaked at 110% of the block.
  constexpr int kStrikesPerBlock = 4;
  int strikes = 0;
  pd::Event e;
  while (strikes < kStrikesPerBlock && g_events.pop(&e)) {
    strikes += g_synth.handle(e);
  }
  g_synth.render(out);
}

// --- control side (core 0) ---------------------------------------------------

// Codec DAC volume register: 0xBF = 0 dB, 0.5 dB per count. Past +12 dB the
// speaker path distorts, so that is the ceiling. It starts at 0 dB: what's
// quiet on the speaker is loud in headphones.
constexpr int kDacDefault = 0xBF;        // 0 dB
constexpr int kDacMax     = 0xBF + 24;   // +12 dB

// Held FN combos auto-repeat: first repeat after kRepeatDelayMs, then every
// interval for that action.
constexpr uint32_t kRepeatDelayMs = 300;
constexpr uint32_t kDiceHoldMs = 500;     // hold the dice key this long to roll
constexpr uint32_t kDrawMs = 33;          // redraw at most ~30 Hz
constexpr uint8_t  kVelocity = 100;

enum class Strip : uint8_t { Notes, Chord, Ratchet, Arp, Count };
const char* stripName(Strip s) {
  switch (s) {
    case Strip::Chord: return "CHORD";
    case Strip::Ratchet: return "RATCHET";
    case Strip::Arp: return "ARP";
    default: return "NOTES";
  }
}

constexpr int kRows = 4, kCols = 14, kKeys = kRows * kCols;
constexpr int keyIndex(int r, int c) { return r * kCols + c; }
// MIDI notes play through virtual keys after the physical ones, so they
// take chords, ratchets, the arp, sustain and the looper exactly as keys do.
constexpr int kMidiKeys = 16, kAllKeys = kKeys + kMidiKeys;
constexpr uint8_t keyTag(int k) { return (uint8_t)(k + 1); }   // 0 = no tag
constexpr uint8_t kArpTag = kAllKeys + 4;                       // the arp's one voice
static_assert(kArpTag < pd::Looper::kLoopTagOffset, "live tags must stay below the looper's");
constexpr uint32_t kLatchTapMs = 300;                            // double-tap window

// Every control, its name, page bar, CCs and who can drive it: ui/controls.h.
namespace ctl = pd::controls;
using ctl::Ctl;
Ctl fxCtl(int bar) { return ctl::pageBar(ctl::kFxPage, bar); }
// The FX page's four bars: TONE, GRIT and SHAPE are MasterFx slots (each
// with an effect type), SPACE is the echo's diffusion.
constexpr int kFxBars = 4;
const char* const kFxBarNames[kFxBars] = {"TONE", "GRIT", "SHAPE", "SPACE"};

// Synced, RATE picks a length of one LFO cycle, slowest first.
struct LfoDiv { const char* label; float beats; };
constexpr LfoDiv kLfoDivs[] = {{"4BAR", 16.0f}, {"2BAR", 8.0f}, {"1BAR", 4.0f}, {"1/2", 2.0f}, {"1/4", 1.0f},
                               {"1/4T", 2.0f / 3.0f}, {"1/8", 0.5f}, {"1/8T", 1.0f / 3.0f}, {"1/16", 0.25f}, {"1/32", 0.125f}};
constexpr int kNumLfoDivs = sizeof kLfoDivs / sizeof kLfoDivs[0];
constexpr int kLfoShapes = (int)pd::LfoShape::Count;

struct Ui {
  int   octave = 3;          // the surface root sits at MIDI 12 * (octave + 1) + root
  float root = 0.0f;         // semitones above C, 0..12, fractional off 12-EDO
  int   dacVol = kDacDefault;
  float params[(int)pd::Param::Count] = {0.0f, 0.75f, 0.0f, 0.5f};   // as set
  Ctl   tiltDest[2] = {Ctl::Timbre, Ctl::Morph};   // what side-to-side and front/back drive
  float tiltAxis[2] = {};      // their readings, 0..1, while tilt is on
  bool  ctrlDown = false, ctrlUsed = false;   // CTRL held; a routing made while held
  float sent[(int)pd::Param::Count] = {-1.0f, -1.0f, -1.0f, -1.0f};  // last sent
  float undo[(int)pd::Param::Count] = {};
  bool  canUndo = false;
  uint32_t seed = 0;
  int   engine = 0;
  int   surface = 0;
  Strip strip = Strip::Notes;
  int   slot = -1;           // strip slot in effect (held or latched), 0..11, or -1
  int   slotKey = -1;        // the key holding it, or -1
  bool  latched = false;     // slot stays on after its key is let go
  int   lastSlot = -1;       // for double-tap latching
  uint32_t lastSlotMs = 0;
  float pitch[kAllKeys] = {};       // the note each key plays (its chord root)
  uint32_t pressSeq[kAllKeys] = {}; // press order, for the arp's ORDER pattern
  uint32_t seq = 0;
  pd::arp::Arp arp;
  pd::ArsGapGenerator arpGaps;
  uint32_t arpNext = 0;      // micros of the next arp step
  bool  arpOn = false;       // the arp voice is gated
  float bpm = 120.0f;
  bool  fn = false, sustain = false, tilt = false;
  bool  sounding[kAllKeys] = {};    // key has voices gated under its tag
  bool  sustained[kAllKeys] = {};   // ...only because the pedal is down
  bool  physical[kAllKeys] = {};    // key is physically down (and played a note)
  uint32_t ratchetNext[kAllKeys] = {};   // micros of the next strike
  int   impRoot = -1;          // IMPRESSION: the key holding the root, or -1
  float impRootPitch = 0.0f;
  int8_t midiNote[kMidiKeys] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  uint32_t midiSeen = 0;       // millis of the last MIDI message, for the status light
  int   echoDiv = -1;          // kEchoDivs index, -1 off
  float echoFb = 0.45f, echoMix = 0.35f;
  float echoSent[2] = {-1.0f, -1.0f};   // feedback, mix as last sent
  int   page = ctl::kSoundPage;   // what the four bars (and FN+z..,) show (FN+P cycles them)
  bool  envOn = false;         // the ADSR; off, each engine keeps its own envelope
  float env[4] = {0.2f, 0.55f, 0.5f, 0.65f};   // ATTACK DECAY SUSTAIN RELEASE, as bars 0..1: 10 ms, 330 ms, 50%, 700 ms
  float lfo[4] = {0.5f, 0.3f, 0.0f, 0.0f};     // RATE DEPTH SHAPE DELAY, as bars 0..1
  bool  lfoSync = false;       // RATE in tempo divisions instead of Hz
  Ctl   lfoDest = ctl::kNone;  // what the LFO drives
  int   fxType[3] = {0, 0, 0}; // TONE, GRIT, SHAPE effect, MasterFx::name()
  float fxAmount[kFxBars] = {};
  float fxSent[kFxBars] = {-1.0f, -1.0f, -1.0f, -1.0f};
  int   fxTypeSent[3] = {-1, -1, -1};
  bool  saveArm = false;       // FN+SHIFT held: digits save instead of load
  int   recKey = -1;           // key holding a recording, or -1
  uint32_t recStart = 0;
  uint32_t diceDown = 0;
  bool  diceHeld = false, diceDone = false;
  struct { char label = 0; uint8_t row, col; uint32_t next, interval; } repeat;
  const char* msg = nullptr;
  uint32_t msgUntil = 0;
  bool  dirty = true;
};
// In RTC slow memory, with the MIDI rings' slots (hw/usb_midi, hw/ble_midi):
// 8 KB that nothing else uses. It's slower than internal SRAM, so it only
// holds what the control side touches at its own pace, never the audio's
// buffers or anything atomic. The bootloader reloads it on every reset that
// isn't a deep-sleep wake, so it starts like any other global.
RTC_DATA_ATTR Ui g_ui;

const pd::surface::Surface& surf() { return pd::surface::kSurfaces[g_ui.surface]; }

void send(pd::EventType t, uint8_t a, uint8_t b = 0, float v = 0.0f) {
  const pd::Event e{t, a, b, v};
  g_events.push(e);
  g_looper.capture(micros(), e);   // records only while the looper is recording
}

// Looper replays go straight to the synth (not back into the looper).
// It waits for room rather than dropping: a stop's releases come in a burst.
void emitLooped(const pd::Event& e) { while (!g_events.push(e)) vTaskDelay(1); }

void flash(const char* m) { g_ui.msg = m; g_ui.msgUntil = millis() + 1500; g_ui.dirty = true; }

// The tilt axis driving `dest` (0 side-to-side, 1 front/back), or -1.
int tiltAxisFor(Ctl dest) {
  if (!g_ui.tilt) return -1;
  return g_ui.tiltDest[0] == dest ? 0 : g_ui.tiltDest[1] == dest ? 1 : -1;
}
bool tilted(int p) { return tiltAxisFor((Ctl)p) >= 0; }
float effective(int p) { const int a = tiltAxisFor((Ctl)p); return a >= 0 ? g_ui.tiltAxis[a] : g_ui.params[p]; }
float echoFbNow() { const int a = tiltAxisFor(Ctl::EchoFb); return a >= 0 ? 0.95f * g_ui.tiltAxis[a] : g_ui.echoFb; }
float echoMixNow() { const int a = tiltAxisFor(Ctl::EchoMix); return a >= 0 ? g_ui.tiltAxis[a] : g_ui.echoMix; }
float fxNow(int bar) { const int a = tiltAxisFor(fxCtl(bar)); return a >= 0 ? g_ui.tiltAxis[a] : g_ui.fxAmount[bar]; }

// Send the controls (set value + tilt) that have moved since last sent.
void pushParams() {
  for (int p = 0; p < (int)pd::Param::Count; ++p) {
    const float v = effective(p);
    if (std::fabs(v - g_ui.sent[p]) < 1.0f / 512.0f) continue;
    send(pd::EventType::Param, (uint8_t)p, 0, v);
    g_ui.sent[p] = v;
    g_ui.dirty = true;
  }
  const float fx[2] = {echoFbNow(), echoMixNow()};
  for (int i = 0; i < 2; ++i) {
    if (std::fabs(fx[i] - g_ui.echoSent[i]) < 1.0f / 512.0f) continue;
    send(pd::EventType::Echo, (uint8_t)(1 + i), 0, fx[i]);
    g_ui.echoSent[i] = fx[i];
    g_ui.dirty = true;
  }
  for (int b = 0; b < kFxBars; ++b) {
    const float v = fxNow(b);
    const int type = b < 3 ? g_ui.fxType[b] : 0;
    if (std::fabs(v - g_ui.fxSent[b]) < 1.0f / 512.0f && (b >= 3 || type == g_ui.fxTypeSent[b])) continue;
    send(pd::EventType::Fx, (uint8_t)b, (uint8_t)type, v);
    g_ui.fxSent[b] = v;
    if (b < 3) g_ui.fxTypeSent[b] = type;
    g_ui.dirty = true;
  }
}

// --- the ENV and LFO pages: what a bar position means ---

float envMs(int bar, float x) {   // ATTACK 2 ms..5 s; DECAY and RELEASE 5 ms..10 s
  return bar == 0 ? 2.0f * std::pow(2500.0f, x) : 5.0f * std::pow(2000.0f, x);
}
float lfoHz(float x) { return 0.05f * std::pow(400.0f, x); }   // 0.05..20 Hz, 1 Hz mid-bar
float lfoDelayMs(float x) { return 5000.0f * x * x; }           // 0..5 s
int lfoDivIndex(float x) { return pd::clamp((int)std::lround(x * (kNumLfoDivs - 1)), 0, kNumLfoDivs - 1); }
int lfoShapeIndex(float x) { return pd::clamp((int)std::lround(x * (kLfoShapes - 1)), 0, kLfoShapes - 1); }

void sendEnv(int bar) {
  send(pd::EventType::Env, (uint8_t)bar, 0, bar == 2 ? g_ui.env[2] : envMs(bar, g_ui.env[bar]));
  g_ui.dirty = true;
}
void setEnvOn(bool on) {
  g_ui.envOn = on;
  send(pd::EventType::Env, 4, 0, on ? 1.0f : 0.0f);
  g_ui.dirty = true;
}
void sendLfo(int bar) {
  const float* l = g_ui.lfo;
  switch (bar) {
    case 0:
      send(pd::EventType::Lfo, 0, 0, lfoHz(l[0]));
      send(pd::EventType::Lfo, 5, 0, g_ui.lfoSync ? kLfoDivs[lfoDivIndex(l[0])].beats : 0.0f);
      break;
    case 1: send(pd::EventType::Lfo, 1, 0, l[1]); break;
    case 2: send(pd::EventType::Lfo, 2, 0, (float)lfoShapeIndex(l[2])); break;
    case 3: send(pd::EventType::Lfo, 3, 0, lfoDelayMs(l[3])); break;
    default: break;
  }
  g_ui.dirty = true;
}
void sendLfoDest() {
  const pd::LfoDest d = g_ui.lfoDest == ctl::kNone ? pd::LfoDest::None : ctl::info(g_ui.lfoDest).lfo;
  send(pd::EventType::Lfo, 4, (uint8_t)d, 0.0f);
  g_ui.dirty = true;
}
void sendEnvLfo() {
  for (int b = 0; b < 4; ++b) { sendEnv(b); sendLfo(b); }
  setEnvOn(g_ui.envOn);
  sendLfoDest();
}

const char* envBarLabel(int bar) {
  static char label[16];
  static const char* const kNames[4] = {"ATK", "DEC", "SUS", "REL"};
  const float ms = envMs(bar, g_ui.env[bar]);
  if (bar == 2) snprintf(label, sizeof label, "SUS %.0f%%", g_ui.env[2] * 100.0f);
  else if (ms < 999.5f) snprintf(label, sizeof label, "%s %.0fms", kNames[bar], ms);
  else snprintf(label, sizeof label, "%s %.1fs", kNames[bar], ms * 1e-3f);
  return label;
}

const char* lfoBarLabel(int bar) {
  static char label[16];
  const float* l = g_ui.lfo;
  switch (bar) {
    case 0: {
      const float hz = lfoHz(l[0]);
      if (g_ui.lfoSync) snprintf(label, sizeof label, "RATE %s", kLfoDivs[lfoDivIndex(l[0])].label);
      else if (hz < 0.995f) snprintf(label, sizeof label, "RATE .%02dHz", (int)std::lround(hz * 100.0f));
      else if (hz < 9.95f) snprintf(label, sizeof label, "RATE %.1fHz", hz);
      else snprintf(label, sizeof label, "RATE %.0fHz", hz);
      break;
    }
    case 1: snprintf(label, sizeof label, "DEPTH %.0f%%", l[1] * 100.0f); break;
    case 2: snprintf(label, sizeof label, "SHAPE %s", pd::lfoShapeName((pd::LfoShape)lfoShapeIndex(l[2]))); break;
    default: snprintf(label, sizeof label, "DELAY %.1fs", lfoDelayMs(l[3]) * 1e-3f); break;
  }
  return label;
}

const char* fxBarLabel(int bar) {
  static char label[16];
  if (bar >= 3) return kFxBarNames[bar];
  snprintf(label, sizeof label, "%s %s", kFxBarNames[bar], pd::MasterFx::name(bar, g_ui.fxType[bar]));
  return label;
}

// --- echo: tempo-synced tape echo on the master bus (dsp/echo.h) ---

struct EchoDiv { const char* label; float beats; };
constexpr EchoDiv kEchoDivs[] = {{"1/8", 0.5f}, {"1/8.", 0.75f}, {"1/4", 1.0f}};
constexpr int kNumEchoDivs = sizeof kEchoDivs / sizeof kEchoDivs[0];

// The time for the division at this tempo, halved until it fits the tape.
float echoSeconds() {
  if (g_ui.echoDiv < 0) return 0.0f;
  float s = 60.0f / g_ui.bpm * kEchoDivs[g_ui.echoDiv].beats;
  while (s > pd::TapeEcho::kMaxSeconds) s *= 0.5f;
  return s;
}

void sendEcho() {
  send(pd::EventType::Echo, 0, 0, echoSeconds());
  g_ui.echoSent[0] = g_ui.echoSent[1] = -1.0f;   // and feedback and mix, as they stand
  pushParams();
  g_ui.dirty = true;
}

void setTempo(float bpm) {
  g_ui.bpm = pd::clamp(bpm, 40.0f, 300.0f);
  send(pd::EventType::Tempo, 0, 0, g_ui.bpm);
  if (g_ui.echoDiv >= 0) send(pd::EventType::Echo, 0, 0, echoSeconds());
  g_ui.dirty = true;
}

void setDac(int reg) {
  g_ui.dacVol = pd::clamp(reg, 0, kDacMax);
  pd::audio::setDacVolume((uint8_t)g_ui.dacVol);
  g_ui.dirty = true;
}

// --- every control by its id (ui/controls.h), as a 0..1 position ---

// Where a control stands, as set (before tilt and the LFO move it).
float ctlValue(Ctl c) {
  const int i = (int)c;
  switch (ctl::pageOf(c)) {
    case ctl::kSoundPage: return g_ui.params[i];
    case ctl::kFxPage: return g_ui.fxAmount[i - (int)Ctl::Tone];
    case ctl::kEnvPage: return g_ui.env[i - (int)Ctl::Attack];
    case ctl::kLfoPage: return g_ui.lfo[i - (int)Ctl::LfoRate];
    default: break;
  }
  switch (c) {
    case Ctl::EchoFb: return g_ui.echoFb / 0.95f;
    case Ctl::EchoMix: return g_ui.echoMix;
    case Ctl::EchoTime: return (float)(g_ui.echoDiv + 1) / 3.0f;
    case Ctl::Volume: return g_ui.dacVol == 0 ? 0.0f : pd::clamp((g_ui.dacVol - (0xBF - 96)) / 120.0f, 0.0f, 1.0f);
    default: return 0.0f;
  }
}

// Sets a control and sends what it changed. ENV switches on when one of its
// bars moves; LFO SHAPE and the echo time land on their steps.
void setCtl(Ctl c, float v) {
  v = pd::clamp(v, 0.0f, 1.0f);
  const int i = (int)c;
  switch (ctl::pageOf(c)) {
    case ctl::kSoundPage: g_ui.params[i] = v; pushParams(); return;
    case ctl::kFxPage: g_ui.fxAmount[i - (int)Ctl::Tone] = v; pushParams(); return;
    case ctl::kEnvPage:
      g_ui.env[i - (int)Ctl::Attack] = v;
      sendEnv(i - (int)Ctl::Attack);
      if (!g_ui.envOn) setEnvOn(true);
      return;
    case ctl::kLfoPage:
      g_ui.lfo[i - (int)Ctl::LfoRate] = c == Ctl::LfoShape ? (float)lfoShapeIndex(v) / (kLfoShapes - 1) : v;
      sendLfo(i - (int)Ctl::LfoRate);
      return;
    default: break;
  }
  switch (c) {
    case Ctl::EchoFb: g_ui.echoFb = 0.95f * v; pushParams(); break;
    case Ctl::EchoMix: g_ui.echoMix = v; pushParams(); break;
    case Ctl::EchoTime: {   // off, then 1/8, 1/8., 1/4 in four equal zones
      const int div = pd::clamp((int)(v * 4.0f), 0, 3) - 1;
      if (div != g_ui.echoDiv) { g_ui.echoDiv = div; sendEcho(); }
      break;
    }
    // -47.5 dB just above 0 up to the +12 dB ceiling, and 0 mutes.
    case Ctl::Volume: setDac(v <= 0.0f ? 0 : 0xBF - 96 + (int)std::lround(v * 120.0f)); break;
    default: break;
  }
}

// FN+z..,: the four bars of the page on screen.
void nudgeParam(pd::Param p, float d) {
  const Ctl c = ctl::pageBar(g_ui.page, (int)p);
  if (c == Ctl::LfoShape) {   // a whole shape a step
    setCtl(c, (float)pd::clamp(lfoShapeIndex(g_ui.lfo[2]) + (d > 0.0f ? 1 : -1), 0, kLfoShapes - 1) / (kLfoShapes - 1));
  } else {
    const bool wasOn = g_ui.envOn;
    setCtl(c, ctlValue(c) + d);
    if (g_ui.envOn && !wasOn) flash("ENV ON");   // moving it means wanting it
  }
  if (g_ui.page == ctl::kLfoPage && g_ui.lfoDest == ctl::kNone) flash("LFO: HOLD CTRL + A KEY TO ROUTE");
}

// --- pages, and the page knobs (CC 102-105) with pickup ---

ctl::Pickup g_pickup[ctl::kBars];

void setPage(int page) {
  g_ui.page = page % ctl::kPages;
  for (auto& k : g_pickup) k.reset();   // a knob has to find each new bar
  static char msg[16];
  snprintf(msg, sizeof msg, "%s PAGE", ctl::kPageNames[g_ui.page]);
  flash(msg);
}

void pageKnob(int bar, float v) {
  const Ctl c = ctl::pageBar(g_ui.page, bar);
  if (g_pickup[bar].feed(v, ctlValue(c))) {
    setCtl(c, v);
    g_pickup[bar].moved(ctlValue(c));
    return;
  }
  static char msg[32];
  snprintf(msg, sizeof msg, "%s %.0f%%: TURN TO IT", ctl::name(c), ctlValue(c) * 100.0f);
  flash(msg);
}

// Root moves by one step of the current tuning; whole octaves carry into
// the octave so the display stays readable.
void nudgeRoot(int dir) {
  g_ui.root += dir * 12.0f / surf().divisions;
  if (g_ui.root >= 12.0f - 1e-3f) { g_ui.root -= 12.0f; g_ui.octave = pd::clamp(g_ui.octave + 1, 0, 7); }
  if (g_ui.root < -1e-3f)         { g_ui.root += 12.0f; g_ui.octave = pd::clamp(g_ui.octave - 1, 0, 7); }
  if (std::fabs(g_ui.root) < 1e-3f) g_ui.root = 0.0f;
  g_ui.dirty = true;
}

// --- dice ---

void doDice(bool fullRoll) {
  memcpy(g_ui.undo, g_ui.params, sizeof g_ui.params);
  g_ui.canUndo = true;
  g_ui.seed = pd::rng::hash32(g_ui.seed ^ micros());
  if (fullRoll) pd::dice::roll(g_ui.params, g_ui.seed);
  else pd::dice::mutate(g_ui.params, g_ui.seed);
  pushParams();
  flash(fullRoll ? "ROLL" : "MUTATE");
}

void undoDice() {
  if (!g_ui.canUndo) { flash("NOTHING TO UNDO"); return; }
  float t[(int)pd::Param::Count];
  memcpy(t, g_ui.params, sizeof t);              // swap, so undo twice = redo
  memcpy(g_ui.params, g_ui.undo, sizeof t);
  memcpy(g_ui.undo, t, sizeof t);
  pushParams();
  flash("UNDO");
}

// --- tilt ---

void readAccel(float* a) {
  M5.Imu.update();
  M5.Imu.getAccel(&a[0], &a[1], &a[2]);
}

void toggleTilt() {
  if (g_ui.tilt) {
    g_ui.tilt = false;
    pushParams();
    flash("TILT OFF");
    return;
  }
  if (!M5.Imu.isEnabled()) { flash("NO IMU"); return; }
  float a[3];
  readAccel(a);
  g_ui.tiltAxis[0] = pd::tilt::side(a[0], a[1], a[2]);    // start where it is,
  g_ui.tiltAxis[1] = pd::tilt::front(a[0], a[1], a[2]);   // no glide in
  g_ui.tilt = true;
  pushParams();
  flash("TILT ON");
}

void serviceTilt() {
  if (!g_ui.tilt) return;
  float a[3];
  readAccel(a);
  // A bad read must not reach the smoothing: a NaN there would stay forever.
  if (!pd::isFinite(a[0]) || !pd::isFinite(a[1]) || !pd::isFinite(a[2])) return;
  float& s = g_ui.tiltAxis[0];
  float& f = g_ui.tiltAxis[1];
  s += 0.3f * (pd::tilt::side(a[0], a[1], a[2]) - s);    // ~30 ms smoothing at 100 Hz
  f += 0.3f * (pd::tilt::front(a[0], a[1], a[2]) - f);
  pushParams();
}

// Hold CTRL and tap a control's key to put it on a tilt axis: each control
// has a down/up pair, and the left key of the pair takes side-to-side, the
// right one front/back. A control already on the other axis swaps over.
bool tiltKey(char label, int* axis, Ctl* dest) {
  static constexpr char kKeys[] = "zxcvbnm,dfsg";
  static constexpr Ctl kDests[] = {Ctl::Harmonics, Ctl::Timbre, Ctl::Morph, Ctl::Macro, Ctl::EchoFb, Ctl::EchoMix};
  const char* at = label ? strchr(kKeys, label) : nullptr;
  if (!at) return false;
  const int i = (int)(at - kKeys);
  *axis = i & 1;
  *dest = kDests[i >> 1];
  if (g_ui.page == ctl::kFxPage && ctl::pageOf(*dest) == ctl::kSoundPage) *dest = fxCtl((int)*dest);   // the bars on screen
  return true;
}

void assignTilt(int axis, Ctl dest) {
  Ctl& mine = g_ui.tiltDest[axis];
  Ctl& other = g_ui.tiltDest[1 - axis];
  if (other == dest) other = mine;
  mine = dest;
  if (!g_ui.tilt) toggleTilt();
  else pushParams();
  static char msg[28];
  snprintf(msg, sizeof msg, "%s: %s", axis ? "TILT FRONT" : "TILT SIDE", ctl::name(dest));
  flash(msg);
}

// On the LFO page, hold CTRL and tap a key to route the LFO instead: either
// key of a control's pair, d/f echo feedback, s/g echo mix, t/y/u/i the FX
// bars TONE / GRIT / SHAPE / SPACE, q/w the pitch. The same key again
// unroutes it.
bool lfoKey(char label, Ctl* dest) {
  static constexpr char kKeys[] = "zxcvbnm,dfsgtyuiqw";
  static constexpr Ctl kDests[] = {Ctl::Harmonics, Ctl::Harmonics, Ctl::Timbre, Ctl::Timbre, Ctl::Morph, Ctl::Morph,
                                   Ctl::Macro, Ctl::Macro, Ctl::EchoFb, Ctl::EchoFb, Ctl::EchoMix, Ctl::EchoMix,
                                   Ctl::Tone, Ctl::Grit, Ctl::Shape, Ctl::Space, Ctl::Pitch, Ctl::Pitch};
  const char* at = label ? strchr(kKeys, label) : nullptr;
  if (!at) return false;
  *dest = kDests[at - kKeys];
  return true;
}

void assignLfo(Ctl dest) {
  g_ui.lfoDest = g_ui.lfoDest == dest ? ctl::kNone : dest;
  if (g_ui.lfoDest != ctl::kNone && g_ui.lfo[1] <= 0.0f) { g_ui.lfo[1] = 0.3f; sendLfo(1); }   // routed means audible
  sendLfoDest();
  static char msg[24];
  snprintf(msg, sizeof msg, "LFO: %s", g_ui.lfoDest != ctl::kNone ? ctl::name(g_ui.lfoDest) : "OFF");
  flash(msg);
}

// --- notes, strips, sustain ---

float ratchetPeriodUs() {
  return 60.0e6f / g_ui.bpm * pd::surface::kRatchets[g_ui.slot].beats;
}

void strike(int k, uint32_t now) {
  const float period = ratchetPeriodUs();
  send(pd::EventType::Retrigger, keyTag(k), 0, 0.8f * period * 1e-3f);
  g_ui.ratchetNext[k] = now + (uint32_t)period;
}

bool ratcheting() { return g_ui.strip == Strip::Ratchet && g_ui.slot >= 0; }

void serviceRatchet() {
  if (!ratcheting()) return;
  const uint32_t now = micros();
  for (int k = 0; k < kAllKeys; ++k)
    if (g_ui.physical[k] && g_ui.sounding[k] && (int32_t)(now - g_ui.ratchetNext[k]) >= 0) {
      // Fell more than a period behind (a redraw, a bench): restart the grid.
      const bool late = (int32_t)(now - g_ui.ratchetNext[k]) > (int32_t)ratchetPeriodUs();
      const uint32_t at = late ? now : g_ui.ratchetNext[k];
      strike(k, at);
    }
}

// Held notes go back to sustaining as normal notes.
void endRatchet() {
  if (!ratcheting()) return;
  for (int k = 0; k < kAllKeys; ++k)
    if (g_ui.physical[k] && g_ui.sounding[k]) send(pd::EventType::Retrigger, keyTag(k), 0, 0.0f);
}

// --- arp: while on, held and sustained keys are its pool, not voices ---

bool arping() { return g_ui.strip == Strip::Arp && g_ui.slot >= 0; }

void startArp() {
  for (int k = 0; k < kAllKeys; ++k)
    if (g_ui.sounding[k]) send(pd::EventType::NoteOff, keyTag(k));   // they become the pool
  g_ui.arp.restart();
  g_ui.arpGaps.Replay();
  g_ui.arpNext = micros();
  g_ui.arpOn = false;
}

void serviceArp() {
  if (!arping()) return;
  const uint32_t now = micros();
  if ((int32_t)(now - g_ui.arpNext) < 0) return;
  const auto& slot = pd::arp::kSlots[g_ui.slot];
  const float period = slot.chip ? 1e6f / pd::arp::kChipHz : 60e6f / g_ui.bpm * pd::arp::kStepBeats;
  float gap = period, motifU = 0.0f;
  if (slot.pattern == pd::arp::Pattern::Motif) {
    gap = g_ui.arpGaps.NextGap(2, 1.0f, period);   // golden-ratio three-distance rhythm
    motifU = g_ui.arpGaps.last_voice_u();
  }
  // Fell a whole step behind (a redraw, a bench): restart the grid.
  g_ui.arpNext = (int32_t)(now - g_ui.arpNext) > (int32_t)period ? now + (uint32_t)gap : g_ui.arpNext + (uint32_t)gap;

  float pitches[kAllKeys];
  int keys[kAllKeys], n = 0;
  for (int k = 0; k < kAllKeys; ++k)   // pool in press order
    if (g_ui.sounding[k]) {
      int j = n++;
      while (j > 0 && g_ui.pressSeq[keys[j - 1]] > g_ui.pressSeq[k]) { keys[j] = keys[j - 1]; --j; }
      keys[j] = k;
    }
  for (int i = 0; i < n; ++i) pitches[i] = g_ui.pitch[keys[i]];
  if (n == 0) {
    if (g_ui.arpOn) { send(pd::EventType::NoteOff, kArpTag); g_ui.arpOn = false; }
    return;
  }
  g_ui.seed = pd::rng::hash32(g_ui.seed + 1);
  const int pick = g_ui.arp.next(slot.pattern, pitches, n, g_ui.seed, motifU);
  const float pitch = pitches[pick];
  if (slot.chip && g_ui.arpOn) {
    send(pd::EventType::Pitch, kArpTag, 0, pitch);   // one voice, re-pitched: the chip arp
  } else {
    if (g_ui.arpOn) send(pd::EventType::NoteOff, kArpTag);
    send(pd::EventType::NoteOn, kArpTag, kVelocity, pitch);
    g_ui.arpOn = true;
  }
}

// The pool goes back to being notes.
void endArp() {
  if (!arping()) return;
  if (g_ui.arpOn) send(pd::EventType::NoteOff, kArpTag);
  g_ui.arpOn = false;
  for (int k = 0; k < kAllKeys; ++k)
    if (g_ui.sounding[k]) send(pd::EventType::NoteOn, keyTag(k), kVelocity, g_ui.pitch[k]);
}

void endSlot() {
  endRatchet();
  endArp();
  g_ui.slot = -1;
  g_ui.slotKey = -1;
  g_ui.latched = false;
  g_ui.dirty = true;
}

void releaseSlot() {
  g_ui.slotKey = -1;
  if (!g_ui.latched) endSlot();
}

void pressSlot(int k, int slot) {
  const uint32_t now = millis();
  if (g_ui.latched && slot == g_ui.slot) { endSlot(); return; }   // tap a latched slot: off
  const bool doubleTap = slot == g_ui.lastSlot && now - g_ui.lastSlotMs < kLatchTapMs;
  g_ui.lastSlot = slot;
  g_ui.lastSlotMs = now;
  if (slot != g_ui.slot) {
    endSlot();
    g_ui.slot = slot;
    if (ratcheting()) {
      const uint32_t t = micros();
      for (int j = 0; j < kAllKeys; ++j)
        if (g_ui.physical[j] && g_ui.sounding[j]) strike(j, t);
    }
    if (arping()) startArp();
  }
  g_ui.slotKey = k;
  g_ui.latched = doubleTap;
  g_ui.dirty = true;
}

bool chording() { return g_ui.strip == Strip::Chord && g_ui.slot >= 0; }
float surfaceBase() { return 12.0f * (g_ui.octave + 1) + g_ui.root; }

// Sound key k (physical or MIDI): pitches[0] is its note, the rest its chord.
void startKey(int k, const float* pitches, int n, uint8_t vel) {
  const uint8_t tag = keyTag(k);
  if (g_ui.sounding[k]) send(pd::EventType::NoteOff, tag);   // re-struck while sustained
  g_ui.pitch[k] = pitches[0];
  g_ui.pressSeq[k] = ++g_ui.seq;
  if (!arping())   // under the arp a key joins the pool instead of sounding
    for (int i = 0; i < n; ++i) send(pd::EventType::NoteOn, tag, vel, pitches[i]);
  g_ui.sounding[k] = true;
  g_ui.sustained[k] = false;
  g_ui.physical[k] = true;
  if (ratcheting()) strike(k, micros());
}

void playKey(int k, int degree, uint8_t vel = kVelocity) {
  int offs[pd::surface::kMaxChordNotes] = {0};
  const int n = chording() ? pd::surface::chordDegrees(surf(), g_ui.slot, offs) : 1;
  float pitches[pd::surface::kMaxChordNotes];
  for (int i = 0; i < n; ++i)
    pitches[i] = pd::clamp(surfaceBase() + pd::surface::degreeSemitones(surf(), degree + offs[i]), 0.0f, 127.0f);
  startKey(k, pitches, n, vel);
}

// --- the ratio surfaces and IMPRESSION (ui/surface.h) ---

// 5THS PYTHAG and STERN-BROCOT: each key a ratio of the root. CHORD stacks
// the plain chord shapes (in cents) on it.
void playRatioKey(int k, int r, int c) {
  const float semis = pd::surface::ratioKey(surf(), r, c);
  if (semis == pd::surface::kNoPitch) return;
  const float note = surfaceBase() + semis;
  float pitches[pd::surface::kMaxChordNotes] = {pd::clamp(note, 0.0f, 127.0f)};
  int n = 1;
  if (chording()) {
    const auto& ch = pd::surface::kCentChords[g_ui.slot];
    n = ch.n;
    for (int i = 0; i < n; ++i) pitches[i] = pd::clamp(note + ch.v[i] * 0.01f, 0.0f, 127.0f);
  }
  startKey(k, pitches, n, kVelocity);
  if (surf().kind == pd::surface::Kind::Brocot) {
    const auto q = pd::surface::brocotRatio(r, c);
    static char msg[12];
    snprintf(msg, sizeof msg, "%u/%u", q.p, q.q);
    flash(msg);
  }
}

bool noteKeysHeld(int except) {
  for (int k = 0; k < kKeys; ++k)
    if (k != except && g_ui.physical[k]) return true;
  return false;
}

// IMPRESSION: the first key (nothing else held) is the root and plays as on
// CHROMA 4THS; every other key plays what its place relative to the root
// suggests, in just intonation; letting go of every key frees the root.
void playImpressionKey(int k, int r, int c, int degree) {
  if (g_ui.impRoot < 0 || !noteKeysHeld(k)) {
    g_ui.impRoot = k;
    playKey(k, degree);
    g_ui.impRootPitch = g_ui.pitch[k];
    return;
  }
  const int rootRow = g_ui.impRoot / kCols, rootCol = g_ui.impRoot % kCols;
  float cents[2];
  const int n = pd::surface::impression(rootRow - r, c - rootCol, cents);
  float pitches[2];
  for (int i = 0; i < n; ++i) pitches[i] = pd::clamp(g_ui.impRootPitch + cents[i] * 0.01f, 0.0f, 127.0f);
  startKey(k, pitches, n, kVelocity);
}

void releaseKey(int k) {
  g_ui.physical[k] = false;
  if (!g_ui.sounding[k]) return;
  if (g_ui.sustain) { g_ui.sustained[k] = true; return; }
  send(pd::EventType::NoteOff, keyTag(k));
  g_ui.sounding[k] = false;
}

void setSustain(bool down) {
  g_ui.sustain = down;
  g_ui.dirty = true;
  if (down) return;
  for (int k = 0; k < kAllKeys; ++k)
    if (g_ui.sustained[k]) {
      send(pd::EventType::NoteOff, keyTag(k));
      g_ui.sounding[k] = g_ui.sustained[k] = false;
    }
}

void allOff() {
  send(pd::EventType::AllOff, 0);
  g_ui.arpOn = false;
  for (int k = 0; k < kAllKeys; ++k) g_ui.sounding[k] = g_ui.sustained[k] = false;
}

// --- MIDI in (midi/midi.h): notes are virtual keys ---

// A MIDI note on the surface plays as that key would, so CHORD stacks stay
// in the scale; one off it (off the scale, or off a 49-EDO grid) plays its
// own pitch with the chord's chromatic shape, in plain cents.
void midiNoteOn(int note, int vel) {
  if (g_ui.recKey >= 0) return;   // the speaker would be in the take
  int slot = -1;
  for (int i = 0; i < kMidiKeys; ++i) if (g_ui.midiNote[i] == note) slot = i;   // re-struck
  for (int i = 0; slot < 0 && i < kMidiKeys; ++i)
    if (!g_ui.sounding[kKeys + i] && !g_ui.physical[kKeys + i]) slot = i;
  if (slot < 0) {   // all busy: the oldest goes
    slot = 0;
    for (int i = 1; i < kMidiKeys; ++i)
      if (g_ui.pressSeq[kKeys + i] < g_ui.pressSeq[kKeys + slot]) slot = i;
  }
  const int k = kKeys + slot;
  g_ui.midiNote[slot] = (int8_t)note;
  const int degree = pd::surface::degreeOf(surf(), (float)note - surfaceBase());
  if (degree != pd::surface::kNoDegree) {
    playKey(k, degree, (uint8_t)vel);
  } else {
    float pitches[pd::surface::kMaxChordNotes] = {(float)note};
    int n = 1;
    if (chording()) {
      const auto& ch = pd::surface::kCentChords[g_ui.slot];
      n = ch.n;
      for (int i = 0; i < n; ++i) pitches[i] = pd::clamp(note + ch.v[i] * 0.01f, 0.0f, 127.0f);
    }
    startKey(k, pitches, n, (uint8_t)vel);
  }
}

void midiNoteOff(int note) {
  for (int i = 0; i < kMidiKeys; ++i)
    if (g_ui.midiNote[i] == note && g_ui.physical[kKeys + i]) releaseKey(kKeys + i);
}

struct MidiHandler {
  void noteOn(int n, int v) { midiNoteOn(n, v); }
  void noteOff(int n) { midiNoteOff(n); }
  void sustain(bool d) { if (d != g_ui.sustain) setSustain(d); }
  void bend(float semis) { send(pd::EventType::Bend, 0, 0, semis); }
  void program(int n) {
    g_ui.engine = n % (int)pd::EngineId::Count;
    send(pd::EventType::Engine, (uint8_t)g_ui.engine);
    g_ui.dirty = true;
  }
  void allOff() { ::allOff(); }
  // Every other CC: a page knob, the page, or the fixed map (ui/controls.h).
  void cc(int n, float v) {
    if (ctl::pageKnob((uint8_t)n) >= 0) pageKnob(ctl::pageKnob((uint8_t)n), v);
    else if (n == ctl::kPageSelectCc) { const int p = pd::clamp((int)(v * ctl::kPages), 0, ctl::kPages - 1); if (p != g_ui.page) setPage(p); }
    else if (n == ctl::kPageNextCc) { if (v >= 0.5f) setPage(g_ui.page + 1); }
    else if (ctl::ccControl((uint8_t)n) != ctl::kNone) setCtl(ctl::ccControl((uint8_t)n), v);
  }
  void tempo(float bpm) { if (std::fabs(bpm - g_ui.bpm) >= 0.5f) setTempo(std::round(bpm)); }
};

pd::midi::In g_midiIn;
pd::midi::Parser g_serialMidi;   // serial 'M' injection, for scripted tests
pd::midi::Parser g_bleMidi;
pd::midi::BleDeframer g_bleDeframer;
bool g_bleOk = false;

void onMidi(const pd::midi::Msg& m, uint32_t atUs) {
  MidiHandler h;
  g_midiIn.handle(m, atUs, h);
  if (m.status < 0xF8) {   // the clock alone doesn't light it
    if (millis() - g_ui.midiSeen > 250) g_ui.dirty = true;
    g_ui.midiSeen = millis();
  }
}

void serviceMidi() {
  pd::midi::Msg m;
  uint32_t at;
  for (int i = 0; i < 32 && pd::usbmidi::poll(&m, &at); ++i) onMidi(m, at);
  pd::blemidi::Packet pkt;
  for (int i = 0; i < 8 && pd::blemidi::poll(&pkt); ++i)
    g_bleDeframer.packet(pkt.data, pkt.len, pkt.at, [](uint8_t b, uint32_t t) {
      pd::midi::Msg msg;
      if (g_bleMidi.feed(b, &msg)) onMidi(msg, t);
    });
  static bool bleWas = false;
  if (pd::blemidi::connected() != bleWas) { bleWas = !bleWas; g_ui.dirty = true; flash(bleWas ? "BLE MIDI CONNECTED" : "BLE MIDI DISCONNECTED"); }
  static bool lit = false;
  const bool on = millis() - g_ui.midiSeen < 250;
  if (on != lit) { lit = on; g_ui.dirty = true; }
}

// --- recording: hold FN+R, the mic goes into the take ---

void startRecording(int k) {
  allOff();   // the speaker would be in the take
  pd::sampleStore().startRecording();
  g_ui.recKey = k;
  g_ui.recStart = millis();
  g_ui.dirty = true;
}

void stopRecording() {
  if (g_ui.recKey < 0) return;
  g_ui.recKey = -1;
  auto& s = pd::sampleStore();
  s.stopRecording();
  vTaskDelay(2);   // let the audio core finish the block it may be appending
  if (!s.analyze()) { flash("NOTHING HEARD"); return; }
  static char msg[32];
  snprintf(msg, sizeof msg, "TAKE %.1fs  %d SLICES", (float)s.length() / pd::SampleStore::kRate, s.slices());
  flash(msg);
  if (!pd::playsTake((pd::EngineId)g_ui.engine)) {
    g_ui.engine = (int)pd::EngineId::Sample;
    send(pd::EventType::Engine, (uint8_t)g_ui.engine);
  }
}

void serviceRecording() {
  if (g_ui.recKey < 0) return;
  if (pd::sampleStore().full()) stopRecording();
  else g_ui.dirty = true;   // the timer runs on screen
}

// --- patches: FN+2..8 loads a slot from SD, FN+SHIFT+2..8 saves it ---

pd::patch::Patch currentPatch() {
  pd::patch::Patch p;
  snprintf(p.engine, sizeof p.engine, "%s", pd::engineName((pd::EngineId)g_ui.engine));
  snprintf(p.surface, sizeof p.surface, "%s", surf().name);
  p.octave = g_ui.octave;
  p.root = g_ui.root;
  p.bpm = g_ui.bpm;
  memcpy(p.params, g_ui.params, sizeof p.params);
  snprintf(p.echo, sizeof p.echo, "%s", g_ui.echoDiv >= 0 ? kEchoDivs[g_ui.echoDiv].label : "OFF");
  p.echoFeedback = g_ui.echoFb;
  p.echoMix = g_ui.echoMix;
  snprintf(p.tiltSide, sizeof p.tiltSide, "%s", ctl::name(g_ui.tiltDest[0]));
  snprintf(p.tiltFront, sizeof p.tiltFront, "%s", ctl::name(g_ui.tiltDest[1]));
  for (int b = 0; b < 3; ++b) snprintf(p.fxType[b], sizeof p.fxType[b], "%s", pd::MasterFx::name(b, g_ui.fxType[b]));
  memcpy(p.fxAmount, g_ui.fxAmount, sizeof p.fxAmount);
  p.envOn = g_ui.envOn;
  memcpy(p.env, g_ui.env, sizeof p.env);
  snprintf(p.lfoDest, sizeof p.lfoDest, "%s", ctl::name(g_ui.lfoDest));
  memcpy(p.lfo, g_ui.lfo, sizeof p.lfo);
  p.lfoSync = g_ui.lfoSync;
  return p;
}

void applyPatch(const pd::patch::Patch& p) {
  for (int e = 0; e < (int)pd::EngineId::Count; ++e)
    if (!strcmp(p.engine, pd::engineName((pd::EngineId)e))) g_ui.engine = e;
  for (int i = 0; i < pd::surface::kNumSurfaces; ++i)
    if (!strcmp(p.surface, pd::surface::kSurfaces[i].name)) g_ui.surface = i;
  g_ui.octave = pd::clamp(p.octave, 0, 7);
  g_ui.root = pd::clamp(p.root, 0.0f, 11.999f);
  setTempo(p.bpm);
  for (int i = 0; i < 4; ++i) g_ui.params[i] = pd::clamp(p.params[i], 0.0f, 1.0f);
  g_ui.echoDiv = -1;
  for (int i = 0; i < kNumEchoDivs; ++i) if (!strcmp(p.echo, kEchoDivs[i].label)) g_ui.echoDiv = i;
  g_ui.echoFb = pd::clamp(p.echoFeedback, 0.0f, 0.95f);
  g_ui.echoMix = pd::clamp(p.echoMix, 0.0f, 1.0f);
  Ctl side = ctl::byName(p.tiltSide, ctl::kTilt), front = ctl::byName(p.tiltFront, ctl::kTilt);
  if (side == ctl::kNone) side = Ctl::Timbre;
  if (front == ctl::kNone) front = Ctl::Morph;
  if (side != front) { g_ui.tiltDest[0] = side; g_ui.tiltDest[1] = front; }
  for (int b = 0; b < 3; ++b)
    for (int t = 0; t < pd::MasterFx::kTypes; ++t)
      if (!strcmp(p.fxType[b], pd::MasterFx::name(b, t))) g_ui.fxType[b] = t;
  for (int b = 0; b < kFxBars; ++b) g_ui.fxAmount[b] = pd::clamp(p.fxAmount[b], 0.0f, 1.0f);
  g_ui.envOn = p.envOn;
  for (int i = 0; i < 4; ++i) { g_ui.env[i] = pd::clamp(p.env[i], 0.0f, 1.0f); g_ui.lfo[i] = pd::clamp(p.lfo[i], 0.0f, 1.0f); }
  g_ui.lfo[2] = (float)lfoShapeIndex(g_ui.lfo[2]) / (kLfoShapes - 1);
  g_ui.lfoSync = p.lfoSync;
  g_ui.lfoDest = ctl::byName(p.lfoDest, ctl::kLfo);
  sendEnvLfo();
  sendEcho();
  send(pd::EventType::Engine, (uint8_t)g_ui.engine);
  pushParams();
  g_ui.dirty = true;
}

void patchSlot(int slot) {
  static char msg[32];
  if (!pd::storage::mounted() && !pd::storage::begin()) { flash("NO SD CARD"); return; }
  if (g_ui.saveArm) {
    const bool ok = pd::storage::save(slot, currentPatch(), pd::sampleStore());
    snprintf(msg, sizeof msg, ok ? "SAVED %d%s" : "SAVE %d FAILED", slot, pd::sampleStore().ready() ? " +TAKE" : "");
    flash(msg);
    return;
  }
  pd::patch::Patch p = currentPatch();
  p.envOn = false;   // a patch from before ENV and the LFO plays without them
  snprintf(p.lfoDest, sizeof p.lfoDest, "NONE");
  p.lfoSync = false;
  bool tookTake = false;
  allOff();
  if (!pd::storage::load(slot, &p, pd::sampleStore(), &tookTake)) {
    snprintf(msg, sizeof msg, "SLOT %d EMPTY", slot);
    flash(msg);
    return;
  }
  applyPatch(p);
  snprintf(msg, sizeof msg, "LOADED %d%s", slot, tookTake ? " +TAKE" : "");
  flash(msg);
}

// --- looper: FN+L record -> play <-> overdub, FN+K stop, then clear ---

void looperPress() {
  uint8_t held[kAllKeys + 1];
  int n = 0;
  for (int k = 0; k < kAllKeys; ++k) if (g_ui.sounding[k]) held[n++] = keyTag(k);
  if (g_ui.arpOn) held[n++] = kArpTag;
  const uint32_t beatUs = (uint32_t)(60e6f / g_ui.bpm);
  g_looper.press(micros(), beatUs, held, n, emitLooped);
  g_ui.dirty = true;
}

void looperStop() {
  g_looper.stop(emitLooped);
  flash(g_looper.state() == pd::Looper::Empty ? "LOOP CLEARED" : "LOOP STOPPED");
}

// --- key dispatch ---

// One step of an FN combo. Returns the auto-repeat interval in ms, or 0 for
// actions that fire once per press.
uint32_t fnAction(char label) {
  constexpr float kStep = 1.0f / 32.0f;
  switch (label) {
    case 'z': nudgeParam(pd::Param::Harmonics, -kStep); return 30;
    case 'x': nudgeParam(pd::Param::Harmonics, +kStep); return 30;
    case 'c': nudgeParam(pd::Param::Timbre, -kStep); return 30;
    case 'v': nudgeParam(pd::Param::Timbre, +kStep); return 30;
    case 'b': nudgeParam(pd::Param::Morph, -kStep); return g_ui.page == ctl::kLfoPage ? 250 : 30;   // LFO SHAPE: a shape a step
    case 'n': nudgeParam(pd::Param::Morph, +kStep); return g_ui.page == ctl::kLfoPage ? 250 : 30;
    case 'm': nudgeParam(pd::Param::Macro, -kStep); return 30;
    case ',': nudgeParam(pd::Param::Macro, +kStep); return 30;
    case '[': setDac(g_ui.dacVol - 6); return 150;
    case ']': setDac(g_ui.dacVol + 6); return 150;
    case 'q': nudgeRoot(-1); return 150;
    case 'w': nudgeRoot(+1); return 150;
    case 'e': {   // echo time: off -> 1/8 -> 1/8. -> 1/4 -> off
      g_ui.echoDiv = g_ui.echoDiv + 1 >= kNumEchoDivs ? -1 : g_ui.echoDiv + 1;
      sendEcho();
      static char msg[24];
      if (g_ui.echoDiv < 0) snprintf(msg, sizeof msg, "ECHO OFF");
      else snprintf(msg, sizeof msg, "ECHO %s  %.0f ms", kEchoDivs[g_ui.echoDiv].label, echoSeconds() * 1000.0f);
      flash(msg);
      return 0;
    }
    case 'd': case 'f': {   // echo feedback
      g_ui.echoFb = pd::clamp(g_ui.echoFb + (label == 'f' ? 0.05f : -0.05f), 0.0f, 0.95f);
      sendEcho();
      static char msg[24];
      snprintf(msg, sizeof msg, "ECHO FEEDBACK %.0f%%", g_ui.echoFb * 100.0f);
      flash(msg);
      return 80;
    }
    case 'p': setPage(g_ui.page + 1); return 0;   // the pages: SOUND -> FX -> ENV -> LFO
    case 'a':   // the ADSR on/off
      setEnvOn(!g_ui.envOn);
      flash(g_ui.envOn ? "ENV ON" : "ENV OFF: EACH ENGINE'S OWN");
      return 0;
    case 'o':   // the LFO's rate: free in Hz, or synced to the tempo
      g_ui.lfoSync = !g_ui.lfoSync;
      sendLfo(0);
      flash(g_ui.lfoSync ? "LFO SYNCED TO THE TEMPO" : "LFO FREE-RUNNING");
      return 0;
    case 't': case 'y': case 'u': {   // the effect in TONE, GRIT, SHAPE
      const int bar = label == 't' ? 0 : label == 'y' ? 1 : 2;
      g_ui.fxType[bar] = (g_ui.fxType[bar] + 1) % pd::MasterFx::kTypes;
      pushParams();
      static char msg[20];   // flash keeps the pointer: its own copy, not the label buffer
      snprintf(msg, sizeof msg, "%s", fxBarLabel(bar));
      flash(msg);
      return 0;
    }
    case 's': case 'g': {   // echo mix
      g_ui.echoMix = pd::clamp(g_ui.echoMix + (label == 'g' ? 0.05f : -0.05f), 0.0f, 1.0f);
      pushParams();
      static char msg[24];
      snprintf(msg, sizeof msg, "ECHO MIX %.0f%%", g_ui.echoMix * 100.0f);
      flash(msg);
      return 80;
    }
    case '9': setTempo(g_ui.bpm - 2.0f); return 60;
    case '0': setTempo(g_ui.bpm + 2.0f); return 60;
    case '`': g_ui.octave = pd::clamp(g_ui.octave - 1, 0, 7); g_ui.dirty = true; return 0;
    case '1': g_ui.octave = pd::clamp(g_ui.octave + 1, 0, 7); g_ui.dirty = true; return 0;
    case '>':   // TAB
      g_ui.surface = (g_ui.surface + pd::surface::kNumSurfaces - 1) % pd::surface::kNumSurfaces;
      g_ui.dirty = true;
      return 0;
    case '<': undoDice(); return 0;   // BS
    case '\\':                        // previous engine
      g_ui.engine = (g_ui.engine + (int)pd::EngineId::Count - 1) % (int)pd::EngineId::Count;
      send(pd::EventType::Engine, (uint8_t)g_ui.engine);
      g_ui.dirty = true;
      return 0;
    default: return 0;
  }
}

void serviceRepeat(uint32_t now) {
  auto& rp = g_ui.repeat;
  if (!rp.label || (int32_t)(now - rp.next) < 0) return;
  fnAction(rp.label);
  rp.next = now + rp.interval;
}

void serviceDice(uint32_t now) {
  if (g_ui.diceHeld && !g_ui.diceDone && now - g_ui.diceDown >= kDiceHoldMs) {
    doDice(true);
    g_ui.diceDone = true;
  }
}

void onKey(int r, int c, bool pressed) {
  if (r >= kRows || c >= kCols) return;
  const int k = keyIndex(r, c);

  // Modifiers and releases first: they must work whatever else is held.
  if (pd::keymap::isFn(r, c)) {
    g_ui.fn = pressed;
    if (!pressed) g_ui.repeat.label = 0;
    return;
  }
  if (r == 3 && c == 13) { setSustain(pressed); return; }   // SPACE
  if (r == 3 && c == 0) {   // CTRL: a tap toggles tilt; held, it routes tilt (tiltKey)
    if (pressed) { g_ui.ctrlDown = true; g_ui.ctrlUsed = false; }
    else { g_ui.ctrlDown = false; if (!g_ui.ctrlUsed) toggleTilt(); }
    return;
  }
  if (!pressed) {
    if (r == 2 && c == 1) g_ui.saveArm = false;   // SHIFT
    if (g_ui.repeat.label && g_ui.repeat.row == r && g_ui.repeat.col == c) g_ui.repeat.label = 0;
    if (k == g_ui.slotKey) releaseSlot();
    if (k == g_ui.recKey) stopRecording();
    if (r == 0 && c == 0 && g_ui.diceHeld) {
      if (!g_ui.diceDone) doDice(false);
      g_ui.diceHeld = false;
    }
    releaseKey(k);
    if (g_ui.impRoot >= 0 && !noteKeysHeld(-1)) g_ui.impRoot = -1;   // IMPRESSION: all keys up
    return;
  }

  if (g_ui.ctrlDown) {
    const char label = pd::keymap::kLabel[r][c];
    int axis;
    Ctl dest;
    if (g_ui.page == ctl::kLfoPage) {
      if (lfoKey(label, &dest)) { g_ui.ctrlUsed = true; assignLfo(dest); return; }
    } else if (tiltKey(label, &axis, &dest)) { g_ui.ctrlUsed = true; assignTilt(axis, dest); return; }
  }

  if (g_ui.fn) {
    const char label = pd::keymap::kLabel[r][c];
    if (label == 'r') { startRecording(k); return; }
    if (label == 'l') { looperPress(); return; }
    if (label == 'k') { looperStop(); return; }
    if (label == '^') { g_ui.saveArm = true; flash("SAVE: PICK 2..8"); return; }
    if (label >= '2' && label <= '8') { patchSlot(label - '0'); return; }
    const uint32_t interval = fnAction(label);
    auto& rp = g_ui.repeat;
    rp.label = interval ? label : 0;
    rp.row = (uint8_t)r; rp.col = (uint8_t)c;
    rp.next = millis() + kRepeatDelayMs;
    rp.interval = interval;
    return;
  }

  // Control columns.
  if (c == 0) {
    switch (r) {
      case 0: g_ui.diceHeld = true; g_ui.diceDone = false; g_ui.diceDown = millis(); return;
      case 1: g_ui.surface = (g_ui.surface + 1) % pd::surface::kNumSurfaces; g_ui.dirty = true; return;
      default: return;
    }
  }
  if (c == 13) {
    switch (r) {
      case 0:   // BS: next strip
        endSlot();
        g_ui.strip = (Strip)(((int)g_ui.strip + 1) % (int)Strip::Count);
        g_ui.dirty = true;
        return;
      case 1:
        g_ui.engine = (g_ui.engine + 1) % (int)pd::EngineId::Count;
        send(pd::EventType::Engine, (uint8_t)g_ui.engine);
        g_ui.dirty = true;
        return;
      case 2: allOff(); return;   // ENTER
      default: return;
    }
  }

  if (r == 0 && g_ui.strip != Strip::Notes) { pressSlot(k, c - 1); return; }

  const auto kind = surf().kind;
  if (kind == pd::surface::Kind::Fifths || kind == pd::surface::Kind::Brocot) { playRatioKey(k, r, c); return; }
  const int degree = pd::surface::keyDegree(surf(), r, c);
  if (degree == pd::surface::kNoDegree) return;
  if (kind == pd::surface::Kind::Impression) { playImpressionKey(k, r, c, degree); return; }
  playKey(k, degree);
}

// --- display ---

// Drawn off-screen and pushed whole: no flicker from the full redraw. The
// canvas is 4-bit with a palette (16 KB, half of 8-bit RGB332): the screen
// only uses these colours, and the drawing calls take their indices.
M5Canvas g_canvas(&M5Cardputer.Display);
enum : uint8_t { kPalBlack, kPalOrange, kPalWhite, kPalLightGrey, kPalDarkGrey, kPalCyan, kPalGreen, kPalRed, kPalYellow, kPalMagenta };
void makePalette() {
  static const uint8_t kRgb[][3] = {   // the TFT_ colours, from their RGB565 values
      {0, 0, 0}, {255, 182, 0}, {255, 255, 255}, {214, 210, 214}, {123, 125, 123},
      {0, 255, 255}, {0, 255, 0}, {255, 0, 0}, {255, 255, 0}, {255, 64, 255}};
  g_canvas.createPalette();
  for (size_t i = 0; i < sizeof kRgb / sizeof kRgb[0]; ++i) g_canvas.setPaletteColor(i, kRgb[i][0], kRgb[i][1], kRgb[i][2]);
}

// The LFO's colour marks what it drives: magenta labels, and magenta bars on
// its own page. ENV's bars are yellow, grey while it's off.
void drawPageBar(int y, const char* label, float v, uint8_t labelColour, uint8_t barColour) {
  auto& d = g_canvas;
  d.setTextColor(labelColour, kPalBlack);
  d.setCursor(4, y + 2);
  d.print(label);
  d.drawRect(64, y, 172, 12, kPalDarkGrey);
  d.fillRect(65, y + 1, 170, 10, kPalBlack);
  d.fillRect(65, y + 1, (int)(170 * v), 10, barColour);
}

// FX bars: cyan (the FX page's colour) or green while tilted.
void drawFxBar(int y, int bar) {
  auto& d = g_canvas;
  d.setTextColor(g_ui.lfoDest == fxCtl(bar) ? kPalMagenta : kPalCyan, kPalBlack);
  d.setCursor(4, y + 2);
  d.print(fxBarLabel(bar));
  d.setTextColor(kPalLightGrey, kPalBlack);
  d.drawRect(64, y, 172, 12, kPalDarkGrey);
  d.fillRect(65, y + 1, 170, 10, kPalBlack);
  d.fillRect(65, y + 1, (int)(170 * fxNow(bar)), 10, tiltAxisFor(fxCtl(bar)) >= 0 ? kPalGreen : kPalCyan);
}

void drawBar(int y, const char* name, int p) {
  auto& d = g_canvas;
  d.setTextColor(g_ui.lfoDest == (Ctl)p ? kPalMagenta : kPalLightGrey, kPalBlack);
  d.setCursor(4, y + 2);
  d.print(name);
  d.drawRect(64, y, 172, 12, kPalDarkGrey);
  d.fillRect(65, y + 1, 170, 10, kPalBlack);
  d.fillRect(65, y + 1, (int)(170 * effective(p)), 10, tilted(p) ? kPalGreen : kPalOrange);
}

void rootLabel(char* buf, size_t n) {
  static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int semi = (int)std::lround(g_ui.root);
  const int cents = (int)std::lround((g_ui.root - semi) * 100.0f);
  const char* name = kNames[((semi % 12) + 12) % 12];
  if (cents) snprintf(buf, n, "%s%d%+dc", name, g_ui.octave, cents);
  else snprintf(buf, n, "%s%d", name, g_ui.octave);
}

// The boot screen: version, commit and the licence notice, held for
// kNoticeMs (the instrument already plays underneath).
constexpr uint32_t kNoticeMs = 2500;
void drawNotice() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextSize(2);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  d.setCursor(4, 6);
  d.print("PocketDynamo");
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(4, 30);
  d.print("v" PD_VERSION "  " PD_GIT);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  static const char* const kLines[] = {
      "Copyright 2026 Combust",
      "Free software: GNU AGPL v3 or later.",
      "There is NO WARRANTY.",
      "Source code and licence:",
      PD_SOURCE_URL,
  };
  for (int i = 0; i < 5; ++i) { d.setCursor(4, 50 + 14 * i); d.print(kLines[i]); }
}

void printNotice(bool full) {
  Serial.printf("PocketDynamo " PD_VERSION " (" PD_GIT ")\n"
                "Copyright 2026 Combust. Free software under the GNU Affero General Public\n"
                "License v3.0 or later, with NO WARRANTY. Source: https://" PD_SOURCE_URL "\n");
  if (full) Serial.print(kLicenseText);
}

void draw(const pd::audio::Stats& s) {
  auto& d = g_canvas;
  d.fillScreen(kPalBlack);
  d.setTextSize(2);
  d.setTextColor(kPalOrange, kPalBlack);
  d.setCursor(4, 2);
  d.print(pd::engineName((pd::EngineId)g_ui.engine));
  d.setTextSize(1);
  d.setTextColor(kPalWhite, kPalBlack);
  d.setCursor(124, 2);
  d.print(surf().name);
  char root[16];
  rootLabel(root, sizeof root);
  d.setTextColor(kPalLightGrey, kPalBlack);
  d.setCursor(124, 12);
  d.printf("%s %3.0fbpm %+ddB", root, g_ui.bpm, (g_ui.dacVol - 0xBF) / 2);
  d.setCursor(4, 24);
  d.setTextColor(g_ui.strip == Strip::Notes ? kPalDarkGrey : kPalCyan, kPalBlack);
  d.print(stripName(g_ui.strip));
  d.setTextColor(g_ui.sustain ? kPalGreen : kPalDarkGrey, kPalBlack);
  d.setCursor(64, 24);
  d.print("SUS");
  d.setTextColor(g_ui.tilt ? kPalGreen : kPalDarkGrey, kPalBlack);
  d.setCursor(88, 24);
  d.print("TILT");
  if (pd::usbmidi::available() || pd::blemidi::connected()) {
    d.setTextColor(millis() - g_ui.midiSeen < 250 ? kPalGreen : kPalDarkGrey, kPalBlack);
    d.setCursor(170, 2);
    d.print("MIDI");
  }
  if (g_bleOk) {   // BT: dim while advertising, lit while a host is connected
    d.setTextColor(pd::blemidi::connected() ? kPalCyan : kPalDarkGrey, kPalBlack);
    d.setCursor(200, 2);
    d.print("BT");
  }
  d.setCursor(122, 24);
  const auto& take = pd::sampleStore();
  if (g_ui.recKey >= 0) {
    d.setTextColor(kPalRed, kPalBlack);
    d.printf("REC %.1fs", (millis() - g_ui.recStart) * 1e-3f);
  } else if (take.ready()) {
    d.setTextColor(kPalDarkGrey, kPalBlack);
    d.printf("%.1fs/%d", (float)take.length() / pd::SampleStore::kRate, take.slices());
  }
  d.setCursor(170, 24);
  // Cyan while tilt drives the echo, green while it's on, grey off.
  const bool echoTilted = tiltAxisFor(Ctl::EchoFb) >= 0 || tiltAxisFor(Ctl::EchoMix) >= 0;
  d.setTextColor(echoTilted ? kPalCyan : g_ui.echoDiv >= 0 ? kPalGreen : kPalDarkGrey, kPalBlack);
  d.printf("E%s", g_ui.echoDiv >= 0 ? kEchoDivs[g_ui.echoDiv].label : "--");
  d.setTextColor(kPalDarkGrey, kPalBlack);
  d.setCursor(206, 24);
  d.printf("%lu.%lu%%", (unsigned long)(s.loadAvgPermil / 10), (unsigned long)(s.loadAvgPermil % 10));
  d.setTextColor(kPalLightGrey, kPalBlack);
  switch (g_ui.page) {
    case ctl::kFxPage:
      for (int b = 0; b < kFxBars; ++b) drawFxBar(36 + 16 * b, b);
      break;
    case ctl::kEnvPage:
      for (int b = 0; b < 4; ++b)
        drawPageBar(36 + 16 * b, envBarLabel(b), g_ui.env[b], g_ui.envOn ? kPalYellow : kPalDarkGrey,
                    g_ui.envOn ? kPalYellow : kPalDarkGrey);
      break;
    case ctl::kLfoPage:
      for (int b = 0; b < 4; ++b)
        drawPageBar(36 + 16 * b, lfoBarLabel(b), g_ui.lfo[b], g_ui.lfoDest != ctl::kNone ? kPalMagenta : kPalDarkGrey, kPalMagenta);
      break;
    default:
      drawBar(36, "HARMONICS", 0);
      drawBar(52, "TIMBRE", 1);
      drawBar(68, "MORPH", 2);
      drawBar(84, "MACRO", 3);
      break;
  }

  if (g_ui.strip != Strip::Notes) {
    for (int i = 0; i < 12; ++i) {
      const char* label = g_ui.strip == Strip::Chord ? pd::surface::chord(surf(), i).label
                        : g_ui.strip == Strip::Ratchet ? pd::surface::kRatchets[i].label
                                                       : pd::arp::kSlots[i].label;
      const int x = 6 + 19 * i;
      const bool held = i == g_ui.slot;
      d.fillRect(x, 102, 18, 14, held ? (g_ui.latched ? kPalGreen : kPalCyan) : kPalBlack);
      d.drawRect(x, 102, 18, 14, kPalDarkGrey);
      d.setTextColor(held ? kPalBlack : kPalCyan, held ? (g_ui.latched ? kPalGreen : kPalCyan) : kPalBlack);
      d.setCursor(x + 9 - 3 * (int)strlen(label), 106);
      d.print(label);
    }
  }
  d.setCursor(4, 124);
  if (g_ui.msg && (int32_t)(millis() - g_ui.msgUntil) < 0) {
    d.setTextColor(kPalYellow, kPalBlack);
    d.print(g_ui.msg);
  } else if (g_looper.state() != pd::Looper::Empty) {
    const uint32_t now = micros();
    const auto st = g_looper.state();
    d.setTextColor(st == pd::Looper::Recording || st == pd::Looper::Overdub ? kPalRed : kPalGreen, kPalBlack);
    if (st == pd::Looper::Recording) d.printf("LOOP REC %.1fs", g_looper.elapsed(now) * 1e-6f);
    else d.printf("LOOP %s %.1fs  %d ev", st == pd::Looper::Overdub ? "OVERDUB" : st == pd::Looper::Stopped ? "STOPPED" : "PLAY",
                  g_looper.lapUs() * 1e-6f, g_looper.events());
    if (st == pd::Looper::Playing || st == pd::Looper::Overdub)
      d.fillRect(4, 133, (int)(232 * g_looper.position(now)), 2, kPalGreen);
  } else if (g_ui.page == ctl::kEnvPage) {
    d.setTextColor(kPalDarkGrey, kPalBlack);
    d.print(g_ui.envOn ? "ENV ON   FN+a: each engine's own" : "ENV OFF (each engine's own)  FN+a: on");
  } else if (g_ui.page == ctl::kLfoPage) {
    d.setTextColor(kPalDarkGrey, kPalBlack);
    d.printf("LFO>%s CTRL+key:route FN+o:%s", ctl::name(g_ui.lfoDest), g_ui.lfoSync ? "free" : "sync");
  } else {
    d.setTextColor(kPalDarkGrey, kPalBlack);
    d.print("`dice TAB surf BS strip \\eng CTRL tilt SPC sus");
  }
  d.pushSprite(0, 0);
}

// --- serial ---

// Serial 'B': each engine plays 8 held notes on the live instrument, on the
// audio core, muted and with the effects off, and reports the render task's
// own load and peak over a second (the peak includes the 8 strikes). This is
// what playing costs, cache contention and all, and needs no second voice
// pool. The volume, echo and effects are restored after; the bench's events
// go straight to the synth, so a running looper doesn't record them.
void cpuBench() {
  auto push = [](pd::EventType t, uint8_t a, uint8_t b, float v) {
    while (!g_events.push(pd::Event{t, a, b, v})) vTaskDelay(1);
  };
  const int dac = g_ui.dacVol;
  setDac(0);
  allOff();   // through the UI, so its idea of what's sounding (the arp's too) stays true
  push(pd::EventType::Echo, 0, 0, 0.0f);
  for (int b = 0; b < kFxBars; ++b) push(pd::EventType::Fx, (uint8_t)b, (uint8_t)(b < 3 ? g_ui.fxType[b] : 0), 0.0f);
  push(pd::EventType::Lfo, 4, (uint8_t)pd::LfoDest::None, 0.0f);   // nothing moving under the bench
  push(pd::EventType::AllOff, 0, 0, 0.0f);
  constexpr uint8_t kTag = 100;   // clear of the keys', MIDI's and the arp's tags
  for (int eng = 0; eng < (int)pd::EngineId::Count; ++eng) {
    push(pd::EventType::Engine, (uint8_t)eng, 0, 0.0f);
    vTaskDelay(pdMS_TO_TICKS(50));
    const uint32_t xrun0 = pd::audio::takeStats().underruns;   // and start a fresh window
    for (int n = 0; n < kVoices; ++n) push(pd::EventType::NoteOn, (uint8_t)(kTag + n), 100, 48.0f + 4 * n);
    vTaskDelay(pdMS_TO_TICKS(1000));
    const auto st = pd::audio::takeStats();
    Serial.printf("bench engine=%s voices=%d avg=%lu.%lu%% peak=%lu.%lu%% xrun=+%lu (live, 1 s, peak incl. the strikes)\n",
                  pd::engineName((pd::EngineId)eng), g_synth.activeVoices(),
                  (unsigned long)(st.loadAvgPermil / 10), (unsigned long)(st.loadAvgPermil % 10),
                  (unsigned long)(st.loadPeakPermil / 10), (unsigned long)(st.loadPeakPermil % 10),
                  (unsigned long)(st.underruns - xrun0));
    for (int n = 0; n < kVoices; ++n) push(pd::EventType::NoteOff, (uint8_t)(kTag + n), 0, 0.0f);
  }
  push(pd::EventType::AllOff, 0, 0, 0.0f);
  push(pd::EventType::Engine, (uint8_t)g_ui.engine, 0, 0.0f);
  for (int b = 0; b < kFxBars; ++b) { g_ui.fxSent[b] = -1.0f; if (b < 3) g_ui.fxTypeSent[b] = -1; }
  sendEcho();   // and pushParams(): the effects as they were
  sendLfoDest();
  setDac(dac);
}

void printState(const pd::audio::Stats& s) {
  Serial.printf("pd engine=%s surface=%s strip=%s slot=%d sus=%d tilt=%d(%s=%.2f,%s=%.2f) voices=%d "
                "load=%lu.%lu%% peak=%lu.%lu%% late=%lu xrun=%lu dac_db=%+.1f oct=%d root=%.2f bpm=%.0f "
                "h=%.2f t=%.2f m=%.2f x=%.2f take=%.2fs/%d echo=%s/%.2f fx=%s:%.2f,%s:%.2f,%s:%.2f,SPACE:%.2f page=%s env=%s lfo=%s/%.2f heap=%lu/%lu "
                "stack=%lu/%lu faults=%lu up=%lu\n",
                pd::engineName((pd::EngineId)g_ui.engine), surf().name, stripName(g_ui.strip), g_ui.slot,
                g_ui.sustain, g_ui.tilt, ctl::name(g_ui.tiltDest[0]), g_ui.tiltAxis[0],
                ctl::name(g_ui.tiltDest[1]), g_ui.tiltAxis[1], g_synth.activeVoices(),
                (unsigned long)(s.loadAvgPermil / 10), (unsigned long)(s.loadAvgPermil % 10),
                (unsigned long)(s.loadPeakPermil / 10), (unsigned long)(s.loadPeakPermil % 10),
                (unsigned long)s.lateWrites, (unsigned long)s.underruns, (g_ui.dacVol - 0xBF) * 0.5f, g_ui.octave, g_ui.root, g_ui.bpm,
                effective(0), effective(1), effective(2), effective(3),
                pd::sampleStore().ready() ? (float)pd::sampleStore().length() / pd::SampleStore::kRate : 0.0f,
                pd::sampleStore().slices(), g_ui.echoDiv >= 0 ? kEchoDivs[g_ui.echoDiv].label : "off", g_ui.echoFb,
                pd::MasterFx::name(0, g_ui.fxType[0]), fxNow(0), pd::MasterFx::name(1, g_ui.fxType[1]), fxNow(1),
                pd::MasterFx::name(2, g_ui.fxType[2]), fxNow(2), fxNow(3), ctl::kPageNames[g_ui.page],
                g_ui.envOn ? "on" : "off", ctl::name(g_ui.lfoDest), g_ui.lfo[1],
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
                (unsigned long)pd::audio::stackFreeMin(), (unsigned long)uxTaskGetStackHighWaterMark(nullptr),
                (unsigned long)g_synth.faults(), (unsigned long)(millis() / 1000));
}

int hexDigit(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

void controlTask(void*) {
  uint32_t lastStats = millis(), lastDraw = 0, lastTilt = 0;
  pd::audio::Stats stats = {};
  pd::keys::KeyEvent ev[10];
  Serial.setTimeout(50);
  for (;;) {
    const int n = pd::keys::poll(ev, 10);
    for (int i = 0; i < n; ++i) onKey(ev[i].row, ev[i].col, ev[i].pressed);
    const uint32_t now = millis();
    serviceRepeat(now);
    serviceDice(now);
    serviceRatchet();
    serviceMidi();
    serviceArp();
    g_looper.service(micros(), emitLooped);
    if (g_looper.state() != pd::Looper::Empty && g_looper.state() != pd::Looper::Stopped) g_ui.dirty = true;
    serviceRecording();
    if (now - lastTilt >= 10) { lastTilt = now; serviceTilt(); }
    while (Serial.available()) {
      const int c = Serial.read();
      if (c == '[') setDac(g_ui.dacVol - 6);
      else if (c == ']') setDac(g_ui.dacVol + 6);
      else if (c == 'i') printState(stats);
      else if (c == 'B') cpuBench();
      else if (c == 'S') { if (!pd::storage::mounted()) pd::storage::begin(); pd::storage::report(); }
      else if (c == 'U') pd::usbmidi::rebootToBootloader();
      else if (c == 'L') printNotice(true);
      else if (c == 'M') {   // M<hex bytes>. : raw MIDI, as if it came in over USB
        int hi = -1;
        for (uint32_t t0 = millis(); millis() - t0 < 200;) {
          const int ch = Serial.read();
          if (ch < 0) continue;
          if (ch == '.') break;
          const int d = hexDigit(ch);
          if (d < 0) continue;
          if (hi < 0) { hi = d; continue; }
          pd::midi::Msg m;
          if (g_serialMidi.feed((uint8_t)(hi << 4 | d), &m)) onMidi(m, micros());
          hi = -1;
        }
      }
      else if (c == 'K') {
        char b[3];
        if (Serial.readBytes(b, 3) == 3 && b[0] >= '0' && b[0] <= '3' && hexDigit(b[1]) >= 0)
          onKey(b[0] - '0', hexDigit(b[1]), b[2] == '+');
      }
    }
    if (now - lastStats >= 1000) {
      lastStats = now;
      stats = pd::audio::takeStats();
      printState(stats);
      g_ui.dirty = true;
    }
    if (g_ui.msg && (int32_t)(now - g_ui.msgUntil) >= 0) { g_ui.msg = nullptr; g_ui.dirty = true; }
    if (g_ui.dirty && now - lastDraw >= kDrawMs && now >= kNoticeMs) { lastDraw = now; draw(stats); g_ui.dirty = false; }
    vTaskDelay(1);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  cfg.internal_spk = false;   // we own the codec and I2S
  cfg.internal_mic = false;
  M5Cardputer.begin(cfg, false);   // our own TCA8418 driver, not M5's keyboard
  drawNotice();
  g_canvas.setColorDepth(4);
  const bool canvasOk = g_canvas.createSprite(240, 135) != nullptr;
  if (canvasOk) makePalette();
  const bool keysOk = pd::keys::begin();
  pd::sineTable();   // build the shared table now, not inside the first FM note's block
  g_synth.setEcho(&g_echo);   // before audio starts: the render task reads them
  g_synth.setFx(&g_fx);
  const bool audioOk = pd::audio::begin(render);
  const bool sdOk = pd::storage::begin();
  pd::usbmidi::begin();
  g_bleOk = pd::blemidi::begin();
  if (audioOk) setDac(kDacDefault);
  printNotice(false);
  Serial.printf("pd " PD_VERSION " begin: audio=%s keys=%s imu=%s sd=%s ble=%s canvas=%s free_heap=%lu\n", audioOk ? "ok" : "FAIL",
                keysOk ? "ok" : "FAIL", M5.Imu.isEnabled() ? "ok" : "none", sdOk ? "ok" : "none",
                g_bleOk ? "ok" : "FAIL", canvasOk ? "ok" : "FAIL", (unsigned long)ESP.getFreeHeap());
  g_ui.seed = pd::rng::hash32(micros());
  g_ui.arpGaps.Init(g_ui.seed | 1u);
  pushParams();
  setTempo(g_ui.bpm);
  sendEnvLfo();
  xTaskCreatePinnedToCore(controlTask, "pd_ctrl", 8192, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelete(nullptr); }
