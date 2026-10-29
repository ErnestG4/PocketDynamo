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
// Tilt: while on, the device's attitude sets TIMBRE (side to side, 0 on its
// left edge, 1 on its right) and MORPH (0 flat, 0.8 standing up, 1 tipped
// past upright) directly (ui/tilt.h); switching off returns their set values.
// FN: z/x harmonics, c/v timbre, b/n morph, m/, macro, [/] volume 0..+12 dB,
// q/w root down/up a step, 9/0 tempo, ` / 1 octave, TAB previous surface,
// BS undo the last dice, hold R to record from the mic (into SAMPLE/CLOUD's
// take, up to 3.3 s), 2..8 load a patch from SD, SHIFT+2..8 save one,
// L looper record -> play <-> overdub (synth/looper.h), K stop / clear. Holding a param, volume, root or tempo combo repeats.
// Serial: [ / ] volume, i = print state, B = silent CPU bench, S = SD status, K<row><col
// hex><+|-> = press/release a key (for scripted tests).
#include <Arduino.h>
#include <M5Cardputer.h>
#include <cstring>

#include "dsp/ars.h"
#include "dsp/rng.h"
#include "dsp/tables.h"
#include "hw/audio_io.h"
#include "hw/keys.h"
#include "hw/storage.h"
#include "synth/dice.h"
#include "synth/events.h"
#include "synth/instrument.h"
#include "synth/looper.h"
#include "ui/arp.h"
#include "ui/keymap.h"
#include "ui/surface.h"
#include "ui/tilt.h"

namespace {

constexpr int kVoices = 8;
using Synth = pd::Instrument<kVoices, pd::audio::kBlock>;

Synth g_synth;
pd::SpscQueue<128> g_events;
pd::Looper g_looper;

void render(const float* in, float* out, size_t n) {
  pd::sampleStore().record(in, n);   // no-op unless a take is running
  pd::Event e;
  while (g_events.pop(&e)) g_synth.handle(e);
  g_synth.render(out);
}

// --- control side (core 0) ---------------------------------------------------

// Codec DAC volume register: 0xBF = 0 dB, 0.5 dB per count. Past +12 dB the
// speaker path distorts, so that is the ceiling.
constexpr int kDacDefault = 0xBF + 12;   // +6 dB
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
constexpr uint8_t keyTag(int k) { return (uint8_t)(k + 1); }   // 0 = no tag
constexpr uint8_t kArpTag = kKeys + 4;                           // the arp's one voice
constexpr uint32_t kLatchTapMs = 300;                            // double-tap window

struct Ui {
  int   octave = 3;          // the surface root sits at MIDI 12 * (octave + 1) + root
  float root = 0.0f;         // semitones above C, 0..12, fractional off 12-EDO
  int   dacVol = kDacDefault;
  float params[(int)pd::Param::Count] = {0.0f, 0.75f, 0.0f, 0.5f};   // as set
  float tiltVal[(int)pd::Param::Count] = {};   // tilt's TIMBRE and MORPH, while on
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
  float pitch[kKeys] = {};       // the note each key plays (its chord root)
  uint32_t pressSeq[kKeys] = {}; // press order, for the arp's ORDER pattern
  uint32_t seq = 0;
  pd::arp::Arp arp;
  pd::ArsGapGenerator arpGaps;
  uint32_t arpNext = 0;      // micros of the next arp step
  bool  arpOn = false;       // the arp voice is gated
  float bpm = 120.0f;
  bool  fn = false, sustain = false, tilt = false;
  bool  sounding[kKeys] = {};    // key has voices gated under its tag
  bool  sustained[kKeys] = {};   // ...only because the pedal is down
  bool  physical[kKeys] = {};    // key is physically down (and played a note)
  uint32_t ratchetNext[kKeys] = {};   // micros of the next strike
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
Ui g_ui;

const pd::surface::Surface& surf() { return pd::surface::kSurfaces[g_ui.surface]; }

void send(pd::EventType t, uint8_t a, uint8_t b = 0, float v = 0.0f) {
  const pd::Event e{t, a, b, v};
  g_events.push(e);
  g_looper.capture(micros(), e);   // records only while the looper is recording
}

// Looper replays go straight to the synth (not back into the looper).
void emitLooped(const pd::Event& e) { g_events.push(e); }

void flash(const char* m) { g_ui.msg = m; g_ui.msgUntil = millis() + 1500; g_ui.dirty = true; }

bool tilted(int p) { return g_ui.tilt && (p == (int)pd::Param::Timbre || p == (int)pd::Param::Morph); }
float effective(int p) { return tilted(p) ? g_ui.tiltVal[p] : g_ui.params[p]; }

// Send the controls (set value + tilt) that have moved since last sent.
void pushParams() {
  for (int p = 0; p < (int)pd::Param::Count; ++p) {
    const float v = effective(p);
    if (std::fabs(v - g_ui.sent[p]) < 1.0f / 512.0f) continue;
    send(pd::EventType::Param, (uint8_t)p, 0, v);
    g_ui.sent[p] = v;
    g_ui.dirty = true;
  }
}

void nudgeParam(pd::Param p, float d) {
  float& v = g_ui.params[(int)p];
  v = pd::clamp(v + d, 0.0f, 1.0f);
  pushParams();
  g_ui.dirty = true;
}

void setDac(int reg) {
  g_ui.dacVol = pd::clamp(reg, 0, kDacMax);
  pd::audio::setDacVolume((uint8_t)g_ui.dacVol);
  g_ui.dirty = true;
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
  g_ui.tiltVal[(int)pd::Param::Timbre] = pd::tilt::timbre(a[0], a[1], a[2]);   // start where it is,
  g_ui.tiltVal[(int)pd::Param::Morph] = pd::tilt::morph(a[0], a[1], a[2]);     // no glide in
  g_ui.tilt = true;
  pushParams();
  flash("TILT ON");
}

void serviceTilt() {
  if (!g_ui.tilt) return;
  float a[3];
  readAccel(a);
  float& t = g_ui.tiltVal[(int)pd::Param::Timbre];
  float& m = g_ui.tiltVal[(int)pd::Param::Morph];
  t += 0.3f * (pd::tilt::timbre(a[0], a[1], a[2]) - t);   // ~30 ms smoothing at 100 Hz
  m += 0.3f * (pd::tilt::morph(a[0], a[1], a[2]) - m);
  pushParams();
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
  for (int k = 0; k < kKeys; ++k)
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
  for (int k = 0; k < kKeys; ++k)
    if (g_ui.physical[k] && g_ui.sounding[k]) send(pd::EventType::Retrigger, keyTag(k), 0, 0.0f);
}

// --- arp: while on, held and sustained keys are its pool, not voices ---

bool arping() { return g_ui.strip == Strip::Arp && g_ui.slot >= 0; }

void startArp() {
  for (int k = 0; k < kKeys; ++k)
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

  float pitches[kKeys];
  int keys[kKeys], n = 0;
  for (int k = 0; k < kKeys; ++k)   // pool in press order
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
  for (int k = 0; k < kKeys; ++k)
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
      for (int j = 0; j < kKeys; ++j)
        if (g_ui.physical[j] && g_ui.sounding[j]) strike(j, t);
    }
    if (arping()) startArp();
  }
  g_ui.slotKey = k;
  g_ui.latched = doubleTap;
  g_ui.dirty = true;
}

void playKey(int k, int degree) {
  const uint8_t tag = keyTag(k);
  if (g_ui.sounding[k]) send(pd::EventType::NoteOff, tag);   // re-struck while sustained
  int offs[pd::surface::kMaxChordNotes] = {0};
  int n = 1;
  if (g_ui.strip == Strip::Chord && g_ui.slot >= 0) n = pd::surface::chordDegrees(surf(), g_ui.slot, offs);
  const float base = 12.0f * (g_ui.octave + 1) + g_ui.root;
  g_ui.pitch[k] = pd::clamp(base + pd::surface::degreeSemitones(surf(), degree), 0.0f, 127.0f);
  g_ui.pressSeq[k] = ++g_ui.seq;
  if (!arping())   // under the arp a key joins the pool instead of sounding
    for (int i = 0; i < n; ++i) {
      const float pitch = base + pd::surface::degreeSemitones(surf(), degree + offs[i]);
      send(pd::EventType::NoteOn, tag, kVelocity, pd::clamp(pitch, 0.0f, 127.0f));
    }
  g_ui.sounding[k] = true;
  g_ui.sustained[k] = false;
  g_ui.physical[k] = true;
  if (ratcheting()) strike(k, micros());
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
  for (int k = 0; k < kKeys; ++k)
    if (g_ui.sustained[k]) {
      send(pd::EventType::NoteOff, keyTag(k));
      g_ui.sounding[k] = g_ui.sustained[k] = false;
    }
}

void allOff() {
  send(pd::EventType::AllOff, 0);
  g_ui.arpOn = false;
  for (int k = 0; k < kKeys; ++k) g_ui.sounding[k] = g_ui.sustained[k] = false;
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
  return p;
}

void applyPatch(const pd::patch::Patch& p) {
  for (int e = 0; e < (int)pd::EngineId::Count; ++e)
    if (!strcmp(p.engine, pd::engineName((pd::EngineId)e))) g_ui.engine = e;
  for (int i = 0; i < pd::surface::kNumSurfaces; ++i)
    if (!strcmp(p.surface, pd::surface::kSurfaces[i].name)) g_ui.surface = i;
  g_ui.octave = pd::clamp(p.octave, 0, 7);
  g_ui.root = pd::clamp(p.root, 0.0f, 11.999f);
  g_ui.bpm = pd::clamp(p.bpm, 40.0f, 300.0f);
  for (int i = 0; i < 4; ++i) g_ui.params[i] = pd::clamp(p.params[i], 0.0f, 1.0f);
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
  uint8_t held[kKeys + 1];
  int n = 0;
  for (int k = 0; k < kKeys; ++k) if (g_ui.sounding[k]) held[n++] = keyTag(k);
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
    case 'b': nudgeParam(pd::Param::Morph, -kStep); return 30;
    case 'n': nudgeParam(pd::Param::Morph, +kStep); return 30;
    case 'm': nudgeParam(pd::Param::Macro, -kStep); return 30;
    case ',': nudgeParam(pd::Param::Macro, +kStep); return 30;
    case '[': setDac(g_ui.dacVol - 6); return 150;
    case ']': setDac(g_ui.dacVol + 6); return 150;
    case 'q': nudgeRoot(-1); return 150;
    case 'w': nudgeRoot(+1); return 150;
    case '9': g_ui.bpm = pd::clamp(g_ui.bpm - 2.0f, 40.0f, 300.0f); g_ui.dirty = true; return 60;
    case '0': g_ui.bpm = pd::clamp(g_ui.bpm + 2.0f, 40.0f, 300.0f); g_ui.dirty = true; return 60;
    case '`': g_ui.octave = pd::clamp(g_ui.octave - 1, 0, 7); g_ui.dirty = true; return 0;
    case '1': g_ui.octave = pd::clamp(g_ui.octave + 1, 0, 7); g_ui.dirty = true; return 0;
    case '>':   // TAB
      g_ui.surface = (g_ui.surface + pd::surface::kNumSurfaces - 1) % pd::surface::kNumSurfaces;
      g_ui.dirty = true;
      return 0;
    case '<': undoDice(); return 0;   // BS
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
    return;
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
      case 3: toggleTilt(); return;
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

  const int degree = pd::surface::keyDegree(surf(), r, c);
  if (degree == pd::surface::kNoDegree) return;
  playKey(k, degree);
}

// --- display ---

// Drawn off-screen and pushed whole: no flicker from the full redraw.
M5Canvas g_canvas(&M5Cardputer.Display);

void drawBar(int y, const char* name, int p) {
  auto& d = g_canvas;
  d.setCursor(4, y + 2);
  d.print(name);
  d.drawRect(64, y, 172, 12, TFT_DARKGREY);
  d.fillRect(65, y + 1, 170, 10, TFT_BLACK);
  d.fillRect(65, y + 1, (int)(170 * effective(p)), 10, tilted(p) ? TFT_GREEN : TFT_ORANGE);
}

void rootLabel(char* buf, size_t n) {
  static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int semi = (int)std::lround(g_ui.root);
  const int cents = (int)std::lround((g_ui.root - semi) * 100.0f);
  const char* name = kNames[((semi % 12) + 12) % 12];
  if (cents) snprintf(buf, n, "%s%d%+dc", name, g_ui.octave, cents);
  else snprintf(buf, n, "%s%d", name, g_ui.octave);
}

void draw(const pd::audio::Stats& s) {
  auto& d = g_canvas;
  d.fillScreen(TFT_BLACK);
  d.setTextSize(2);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  d.setCursor(4, 2);
  d.print(pd::engineName((pd::EngineId)g_ui.engine));
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(124, 2);
  d.print(surf().name);
  char root[16];
  rootLabel(root, sizeof root);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.setCursor(124, 12);
  d.printf("%s %3.0fbpm %+ddB", root, g_ui.bpm, (g_ui.dacVol - 0xBF) / 2);
  d.setCursor(4, 24);
  d.setTextColor(g_ui.strip == Strip::Notes ? TFT_DARKGREY : TFT_CYAN, TFT_BLACK);
  d.print(stripName(g_ui.strip));
  d.setTextColor(g_ui.sustain ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  d.setCursor(64, 24);
  d.print("SUS");
  d.setTextColor(g_ui.tilt ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  d.setCursor(88, 24);
  d.print("TILT");
  d.setCursor(122, 24);
  const auto& take = pd::sampleStore();
  if (g_ui.recKey >= 0) {
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.printf("REC %.1fs", (millis() - g_ui.recStart) * 1e-3f);
  } else if (take.ready()) {
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.printf("%.1fs/%d", (float)take.length() / pd::SampleStore::kRate, take.slices());
  }
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(170, 24);
  d.printf("cpu %lu.%lu%%", (unsigned long)(s.loadAvgPermil / 10), (unsigned long)(s.loadAvgPermil % 10));
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  drawBar(36, "HARMONICS", 0);
  drawBar(52, "TIMBRE", 1);
  drawBar(68, "MORPH", 2);
  drawBar(84, "MACRO", 3);

  if (g_ui.strip != Strip::Notes) {
    for (int i = 0; i < 12; ++i) {
      const char* label = g_ui.strip == Strip::Chord ? pd::surface::chord(surf(), i).label
                        : g_ui.strip == Strip::Ratchet ? pd::surface::kRatchets[i].label
                                                       : pd::arp::kSlots[i].label;
      const int x = 6 + 19 * i;
      const bool held = i == g_ui.slot;
      d.fillRect(x, 102, 18, 14, held ? (g_ui.latched ? TFT_GREEN : TFT_CYAN) : TFT_BLACK);
      d.drawRect(x, 102, 18, 14, TFT_DARKGREY);
      d.setTextColor(held ? TFT_BLACK : TFT_CYAN, held ? (g_ui.latched ? TFT_GREEN : TFT_CYAN) : TFT_BLACK);
      d.setCursor(x + 9 - 3 * (int)strlen(label), 106);
      d.print(label);
    }
  }
  d.setCursor(4, 124);
  if (g_ui.msg && (int32_t)(millis() - g_ui.msgUntil) < 0) {
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.print(g_ui.msg);
  } else if (g_looper.state() != pd::Looper::Empty) {
    const uint32_t now = micros();
    const auto st = g_looper.state();
    d.setTextColor(st == pd::Looper::Recording || st == pd::Looper::Overdub ? TFT_RED : TFT_GREEN, TFT_BLACK);
    if (st == pd::Looper::Recording) d.printf("LOOP REC %.1fs", g_looper.elapsed(now) * 1e-6f);
    else d.printf("LOOP %s %.1fs  %d ev", st == pd::Looper::Overdub ? "OVERDUB" : st == pd::Looper::Stopped ? "STOPPED" : "PLAY",
                  g_looper.lapUs() * 1e-6f, g_looper.events());
    if (st == pd::Looper::Playing || st == pd::Looper::Overdub)
      d.fillRect(4, 133, (int)(232 * g_looper.position(now)), 2, TFT_GREEN);
  } else {
    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.print("`dice TAB surf BS strip \\eng CTRL tilt SPC sus");
  }
  d.pushSprite(0, 0);
}

// --- serial ---

void cpuBench() {
  static Synth bench;   // static: a voice pool is too big for the task stack
  constexpr int kBlocks = 1000;
  constexpr uint32_t kCyclesPerBlock = 240000000ULL * pd::audio::kBlock / pd::audio::kSampleRate;
  for (int eng = 0; eng < (int)pd::EngineId::Count; ++eng) {
    bench.handle({pd::EventType::AllOff, 0, 0, 0.0f});
    for (int i = 0; i < 2000; ++i) { float o[pd::audio::kBlock]; bench.render(o); }   // drain tails
    bench.handle({pd::EventType::Engine, (uint8_t)eng, 0, 0.0f});
    for (int p = 0; p < (int)pd::Param::Count; ++p)
      bench.handle({pd::EventType::Param, (uint8_t)p, 0, effective(p)});
    for (int n = 0; n < kVoices; ++n)
      bench.handle({pd::EventType::NoteOn, (uint8_t)(n + 1), 100, 48.0f + 4 * n});
    float o[pd::audio::kBlock];
    uint32_t sum = 0, peak = 0, over = 0, first = 0;
    int peakAt = -1;
    for (int b = 0; b < kBlocks; ++b) {
      const uint32_t c0 = ESP.getCycleCount();
      bench.render(o);
      const uint32_t dc = ESP.getCycleCount() - c0;
      sum += dc;
      if (dc > peak) { peak = dc; peakAt = b; }
      if (dc > kCyclesPerBlock) ++over;
      if (b == 0) first = dc;
    }
    Serial.printf("bench engine=%s voices=%d avg=%.1f%% peak=%.1f%% (block %d) first=%.1f%% over100=%lu "
                  "of one core per block (%lu cycles avg)\n",
                  pd::engineName((pd::EngineId)eng), bench.activeVoices(),
                  100.0f * sum / kBlocks / kCyclesPerBlock, 100.0f * peak / kCyclesPerBlock, peakAt,
                  100.0f * first / kCyclesPerBlock, (unsigned long)over, (unsigned long)(sum / kBlocks));
  }
  bench.handle({pd::EventType::AllOff, 0, 0, 0.0f});
}

void printState(const pd::audio::Stats& s) {
  Serial.printf("pd engine=%s surface=%s strip=%s slot=%d sus=%d tilt=%d(t%.2f,m%.2f) voices=%d "
                "load=%lu.%lu%% peak=%lu.%lu%% late=%lu dac_db=%+.1f oct=%d root=%.2f bpm=%.0f "
                "h=%.2f t=%.2f m=%.2f x=%.2f\n",
                pd::engineName((pd::EngineId)g_ui.engine), surf().name, stripName(g_ui.strip), g_ui.slot,
                g_ui.sustain, g_ui.tilt, effective(1), effective(2), g_synth.activeVoices(),
                (unsigned long)(s.loadAvgPermil / 10), (unsigned long)(s.loadAvgPermil % 10),
                (unsigned long)(s.loadPeakPermil / 10), (unsigned long)(s.loadPeakPermil % 10),
                (unsigned long)s.lateWrites, (g_ui.dacVol - 0xBF) * 0.5f, g_ui.octave, g_ui.root, g_ui.bpm,
                effective(0), effective(1), effective(2), effective(3));
}

int hexDigit(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
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
    if (g_ui.dirty && now - lastDraw >= kDrawMs) { lastDraw = now; draw(stats); g_ui.dirty = false; }
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
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  g_canvas.setColorDepth(16);
  const bool canvasOk = g_canvas.createSprite(240, 135) != nullptr;
  const bool keysOk = pd::keys::begin();
  pd::sineTable();   // build the shared table now, not inside the first FM note's block
  const bool audioOk = pd::audio::begin(render);
  const bool sdOk = pd::storage::begin();
  if (audioOk) setDac(kDacDefault);
  Serial.printf("pd begin: audio=%s keys=%s imu=%s sd=%s canvas=%s free_heap=%lu\n", audioOk ? "ok" : "FAIL",
                keysOk ? "ok" : "FAIL", M5.Imu.isEnabled() ? "ok" : "none", sdOk ? "ok" : "none",
                canvasOk ? "ok" : "FAIL", (unsigned long)ESP.getFreeHeap());
  g_ui.seed = pd::rng::hash32(micros());
  g_ui.arpGaps.Init(g_ui.seed | 1u);
  pushParams();
  xTaskCreatePinnedToCore(controlTask, "pd_ctrl", 8192, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelete(nullptr); }
