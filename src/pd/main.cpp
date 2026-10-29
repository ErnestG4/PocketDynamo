// PocketDynamo — playable: keyboard -> event queue -> instrument -> codec.
// Core 1: pd_audio render task (hw/audio_io), drains the event queue each
// block. Core 0: control task — keyboard, display, serial. The Arduino loop
// task is deleted so nothing else competes on core 1.
//
// Keys: two tracker-style piano octaves (ui/keymap.h). TAB next engine,
// ENTER all notes off. Hold FN: z/x harmonics, c/v timbre, b/n morph,
// m/, macro (down/up), [/] codec volume, ` / 1 octave down/up.
// Serial: [ / ] codec volume, i = print state, B = silent CPU bench of a
// six-voice chord per engine (core 0, separate instrument, nothing audible).
#include <Arduino.h>
#include <M5Cardputer.h>

#include "hw/audio_io.h"
#include "hw/keys.h"
#include "synth/events.h"
#include "synth/instrument.h"
#include "ui/keymap.h"

namespace {

constexpr int kVoices = 6;
using Synth = pd::Instrument<kVoices, pd::audio::kBlock>;

Synth g_synth;
pd::SpscQueue<64> g_events;

void render(const float* in, float* out, size_t n) {
  (void)in; (void)n;
  pd::Event e;
  while (g_events.pop(&e)) g_synth.handle(e);
  g_synth.render(out);
}

// --- control side (core 0) ---------------------------------------------------

struct Ui {
  int   octave = 4;          // lower C = MIDI 12 * (octave + 1): C4 = 60
  int   dacVol = 0xBF;       // 0 dB
  float params[(int)pd::Param::Count] = {0.0f, 0.75f, 0.0f, 0.5f};
  int   engine = 0;
  bool  fn = false;
  uint8_t held[4][14] = {};  // note sounding per key (0 = none), so octave
                             // changes never strand a note
  bool  dirty = true;
};
Ui g_ui;

void send(pd::EventType t, uint8_t a, uint8_t b = 0, float v = 0.0f) {
  g_events.push(pd::Event{t, a, b, v});
}

void nudgeParam(pd::Param p, float d) {
  float& v = g_ui.params[(int)p];
  v = pd::clamp(v + d, 0.0f, 1.0f);
  send(pd::EventType::Param, (uint8_t)p, 0, v);
  g_ui.dirty = true;
}

void setDac(int reg) {
  g_ui.dacVol = pd::clamp(reg, 0, 0xFF);
  pd::audio::setDacVolume((uint8_t)g_ui.dacVol);
  g_ui.dirty = true;
}

void onKey(const pd::keys::KeyEvent& k) {
  const int r = k.row, c = k.col;
  if (r > 3 || c > 13) return;
  if (pd::keymap::isFn(r, c)) { g_ui.fn = k.pressed; return; }

  // Releases always end whatever that key started.
  if (!k.pressed) {
    if (g_ui.held[r][c]) { send(pd::EventType::NoteOff, g_ui.held[r][c]); g_ui.held[r][c] = 0; }
    return;
  }

  if (g_ui.fn) {
    constexpr float kStep = 1.0f / 32.0f;
    switch (pd::keymap::kLabel[r][c]) {
      case 'z': nudgeParam(pd::Param::Harmonics, -kStep); return;
      case 'x': nudgeParam(pd::Param::Harmonics, +kStep); return;
      case 'c': nudgeParam(pd::Param::Timbre, -kStep); return;
      case 'v': nudgeParam(pd::Param::Timbre, +kStep); return;
      case 'b': nudgeParam(pd::Param::Morph, -kStep); return;
      case 'n': nudgeParam(pd::Param::Morph, +kStep); return;
      case 'm': nudgeParam(pd::Param::Macro, -kStep); return;
      case ',': nudgeParam(pd::Param::Macro, +kStep); return;
      case '[': setDac(g_ui.dacVol - 6); return;
      case ']': setDac(g_ui.dacVol + 6); return;
      case '`': g_ui.octave = pd::clamp(g_ui.octave - 1, 0, 8); g_ui.dirty = true; return;
      case '1': g_ui.octave = pd::clamp(g_ui.octave + 1, 0, 8); g_ui.dirty = true; return;
      default: return;
    }
  }

  if (pd::keymap::isTab(r, c)) {
    g_ui.engine = (g_ui.engine + 1) % (int)pd::EngineId::Count;
    send(pd::EventType::Engine, (uint8_t)g_ui.engine);
    g_ui.dirty = true;
    return;
  }
  if (r == 2 && c == 13) { send(pd::EventType::AllOff, 0); return; }   // ENTER

  const int semi = pd::keymap::kNote[r][c];
  if (semi < 0) return;
  const int note = pd::clamp(12 * (g_ui.octave + 1) + semi, 1, 127);
  g_ui.held[r][c] = (uint8_t)note;
  send(pd::EventType::NoteOn, (uint8_t)note, 100);
}

void drawBar(int y, const char* name, float v) {
  auto& d = M5Cardputer.Display;
  d.setCursor(4, y + 2);
  d.print(name);
  d.drawRect(64, y, 170, 12, TFT_DARKGREY);
  d.fillRect(65, y + 1, 168, 10, TFT_BLACK);
  d.fillRect(65, y + 1, (int)(168 * v), 10, TFT_ORANGE);
}

void draw(const pd::audio::Stats& s) {
  auto& d = M5Cardputer.Display;
  d.fillRect(0, 0, 240, 30, TFT_BLACK);
  d.setTextSize(2);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  d.setCursor(4, 4);
  d.print(pd::engineName((pd::EngineId)g_ui.engine));
  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.setCursor(150, 4);
  d.printf("oct %d  vol %+d", g_ui.octave, (g_ui.dacVol - 0xBF) / 2);
  d.setCursor(150, 16);
  d.printf("cpu %lu.%lu%%", (unsigned long)(s.loadAvgPermil / 10), (unsigned long)(s.loadAvgPermil % 10));
  drawBar(40, "HARMONICS", g_ui.params[0]);
  drawBar(60, "TIMBRE", g_ui.params[1]);
  drawBar(80, "MORPH", g_ui.params[2]);
  drawBar(100, "MACRO", g_ui.params[3]);
  d.setCursor(4, 122);
  d.print("TAB eng  FN+zx cv bn m, params  FN+[] vol");
}

void cpuBench() {
  static Synth bench;   // static: a voice pool is too big for the task stack
  constexpr int kBlocks = 1000;
  constexpr uint32_t kCyclesPerBlock = 240000000ULL * pd::audio::kBlock / pd::audio::kSampleRate;
  for (int eng = 0; eng < (int)pd::EngineId::Count; ++eng) {
    bench.handle({pd::EventType::AllOff, 0, 0, 0.0f});
    for (int i = 0; i < 2000; ++i) { float o[pd::audio::kBlock]; bench.render(o); }   // drain tails
    bench.handle({pd::EventType::Engine, (uint8_t)eng, 0, 0.0f});
    for (int p = 0; p < (int)pd::Param::Count; ++p)
      bench.handle({pd::EventType::Param, (uint8_t)p, 0, g_ui.params[p]});
    for (int n = 0; n < kVoices; ++n) bench.handle({pd::EventType::NoteOn, (uint8_t)(48 + 4 * n), 100, 0.0f});
    float o[pd::audio::kBlock];
    uint32_t sum = 0, peak = 0;
    for (int b = 0; b < kBlocks; ++b) {
      const uint32_t c0 = ESP.getCycleCount();
      bench.render(o);
      const uint32_t dc = ESP.getCycleCount() - c0;
      sum += dc; if (dc > peak) peak = dc;
    }
    Serial.printf("bench engine=%s voices=%d avg=%.1f%% peak=%.1f%% of one core per block (%lu cycles avg)\n",
                  pd::engineName((pd::EngineId)eng), bench.activeVoices(),
                  100.0f * sum / kBlocks / kCyclesPerBlock, 100.0f * peak / kCyclesPerBlock,
                  (unsigned long)(sum / kBlocks));
  }
  bench.handle({pd::EventType::AllOff, 0, 0, 0.0f});
}

void controlTask(void*) {
  uint32_t lastStats = millis();
  pd::audio::Stats stats = {};
  pd::keys::KeyEvent ev[10];
  for (;;) {
    const int n = pd::keys::poll(ev, 10);
    for (int i = 0; i < n; ++i) onKey(ev[i]);
    while (Serial.available()) {
      const int c = Serial.read();
      if (c == '[') setDac(g_ui.dacVol - 6);
      else if (c == ']') setDac(g_ui.dacVol + 6);
      else if (c == 'i') g_ui.dirty = true;
      else if (c == 'B') cpuBench();
    }
    if (millis() - lastStats >= 1000) {
      lastStats = millis();
      stats = pd::audio::takeStats();
      Serial.printf("pd engine=%s voices=%d load=%lu.%lu%% peak=%lu.%lu%% late=%lu rxshort=%lu "
                    "dac_db=%+.1f oct=%d h=%.2f t=%.2f m=%.2f x=%.2f\n",
                    pd::engineName((pd::EngineId)g_ui.engine), g_synth.activeVoices(),
                    (unsigned long)(stats.loadAvgPermil / 10), (unsigned long)(stats.loadAvgPermil % 10),
                    (unsigned long)(stats.loadPeakPermil / 10), (unsigned long)(stats.loadPeakPermil % 10),
                    (unsigned long)stats.lateWrites, (unsigned long)stats.rxShort,
                    (g_ui.dacVol - 0xBF) * 0.5f, g_ui.octave,
                    g_ui.params[0], g_ui.params[1], g_ui.params[2], g_ui.params[3]);
      g_ui.dirty = true;
    }
    if (g_ui.dirty) { draw(stats); g_ui.dirty = false; }
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
  const bool keysOk = pd::keys::begin();
  const bool audioOk = pd::audio::begin(render);
  Serial.printf("pd begin: audio=%s keys=%s\n", audioOk ? "ok" : "FAIL", keysOk ? "ok" : "FAIL");
  for (int p = 0; p < (int)pd::Param::Count; ++p)
    send(pd::EventType::Param, (uint8_t)p, 0, g_ui.params[p]);
  xTaskCreatePinnedToCore(controlTask, "pd_ctrl", 6144, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelete(nullptr); }
