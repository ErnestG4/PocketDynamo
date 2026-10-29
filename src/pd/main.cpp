// PocketDynamo — step 1: the audio foundation.
// Core 1: pd_audio render task (hw/audio_io). Core 0: control task (serial for
// now). The Arduino loop task is deleted so nothing else competes on core 1.
//
// Serial commands: t = test tone on/off, p = mic passthrough on/off (keep the
// volume low: speaker and mic sit centimetres apart), +/- = tone pitch,
// a = cycle tone amplitude, [ / ] = codec DAC volume down/up 3 dB.
#include <Arduino.h>
#include <M5Cardputer.h>

#include "dsp/tables.h"
#include "hw/audio_io.h"

namespace {

volatile bool     g_tone = false;
volatile bool     g_pass = false;
volatile float    g_toneAmp = 0.15f;
volatile uint32_t g_toneInc = pd::phaseIncrement(440.0f, pd::audio::kSampleRate);

// Mic meter, accumulated in the render, read + reset from core 0.
volatile float    g_micSumSq = 0.0f, g_micPeak = 0.0f;
volatile uint32_t g_micCount = 0;

void render(const float* in, float* out, size_t n) {
  static uint32_t phase = 0;
  float sumSq = 0.0f, peak = 0.0f;
  const bool tone = g_tone, pass = g_pass;
  const uint32_t inc = g_toneInc;
  for (size_t i = 0; i < n; ++i) {
    const float x = in[i];
    sumSq += x * x;
    const float a = x < 0.0f ? -x : x;
    if (a > peak) peak = a;
    float y = 0.0f;
    if (tone) { y += g_toneAmp * pd::sinePhase(phase); phase += inc; }
    if (pass) y += 0.5f * x;
    out[i] = y;
  }
  g_micSumSq += sumSq;
  g_micCount += n;
  if (peak > g_micPeak) g_micPeak = peak;
}

void controlTask(void*) {
  uint32_t lastReport = millis();
  float toneHz = 440.0f;
  int dacVol = 0xBF;   // 0 dB
  for (;;) {
    while (Serial.available()) {
      const int c = Serial.read();
      if (c == 't') g_tone = !g_tone;
      else if (c == 'p') g_pass = !g_pass;
      else if (c == 'a') g_toneAmp = g_toneAmp < 0.2f ? 0.5f : (g_toneAmp < 0.6f ? 1.0f : 0.15f);
      else if (c == '[' || c == ']') {
        dacVol += (c == ']') ? 6 : -6;
        dacVol = dacVol < 0 ? 0 : (dacVol > 0xFF ? 0xFF : dacVol);
        pd::audio::setDacVolume((uint8_t)dacVol);
      }
      else if (c == '+' || c == '-') {
        toneHz *= (c == '+') ? 1.0594631f : 1.0f / 1.0594631f;
        g_toneInc = pd::phaseIncrement(toneHz, pd::audio::kSampleRate);
      }
    }
    if (millis() - lastReport >= 1000) {
      lastReport = millis();
      const pd::audio::Stats s = pd::audio::takeStats();
      const uint32_t cnt = g_micCount ? g_micCount : 1;
      const float rms = sqrtf(g_micSumSq / cnt), pk = g_micPeak;
      g_micSumSq = 0.0f; g_micCount = 0; g_micPeak = 0.0f;
      Serial.printf("pd blocks=%lu load=%lu.%lu%% peak=%lu.%lu%% late=%lu rxshort=%lu "
                    "mic_rms=%.5f mic_peak=%.4f tone=%d amp=%.2f dac_db=%+.1f pass=%d hz=%.1f\n",
                    (unsigned long)s.blocks,
                    (unsigned long)(s.loadAvgPermil / 10), (unsigned long)(s.loadAvgPermil % 10),
                    (unsigned long)(s.loadPeakPermil / 10), (unsigned long)(s.loadPeakPermil % 10),
                    (unsigned long)s.lateWrites, (unsigned long)s.rxShort,
                    rms, pk, (int)g_tone, g_toneAmp, (dacVol - 0xBF) * 0.5f, (int)g_pass, toneHz);
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  cfg.internal_spk = false;   // we own the codec and I2S
  cfg.internal_mic = false;
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.drawString("PocketDynamo", 8, 8);
  pd::sineTable();            // build before the audio task needs it
  const bool ok = pd::audio::begin(render);
  M5Cardputer.Display.drawString(ok ? "audio: ok" : "audio: FAIL", 8, 40);
  Serial.printf("pd audio begin: %s\n", ok ? "ok" : "FAIL");
  xTaskCreatePinnedToCore(controlTask, "pd_ctrl", 4096, nullptr, 2, nullptr, 0);
}

void loop() { vTaskDelete(nullptr); }
