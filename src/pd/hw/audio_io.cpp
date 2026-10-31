// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include "audio_io.h"

#include <Arduino.h>
#include <M5Cardputer.h>
#include <driver/i2s.h>

namespace pd {
namespace audio {
namespace {

// Wiring (M5Unified's ADV tables): ES8311 at 0x18 on the internal I2C bus
// (SDA 8, SCL 9); I2S BCK 41, WS 43, DOUT 42, DIN 46. No MCLK pin: the codec
// derives MCLK from BCLK.
constexpr uint8_t    kCodecAddr = 0x18;
constexpr i2s_port_t kPort      = I2S_NUM_1;
constexpr int kPinBck = 41, kPinWs = 43, kPinDout = 42, kPinDin = 46;

// ES8311 bring-up with DAC and ADC both powered: the union of M5Unified's
// separate speaker and mic sequences, which is the one thing it never does.
// MCLK = BCLK x8 (MULT_PRE=3); 16-bit stereo frames give BCLK = 32 fs, so
// the internal clock is 256 fs at any rate.
const uint8_t kCodecInit[][2] = {
  {0x00, 0x80},  // reset / CSM power on
  {0x01, 0xBF},  // MCLK from BCLK, all DAC + ADC clocks on
  {0x02, 0x18},  // MULT_PRE = 3 (x8)
  {0x0D, 0x01},  // power up analog
  {0x0E, 0x02},  // analog PGA + ADC modulator on
  {0x12, 0x00},  // DAC power up
  {0x13, 0x10},  // HP drive on (feeds the speaker amp)
  {0x14, 0x14},  // ADC input Mic1p-Mic1n, PGA +12 dB (a clap peaks ~0.6 FS)
  {0x17, 0xBF},  // ADC volume 0 dB
  {0x1C, 0x6A},  // ADC EQ bypass, digital DC cancel
  {0x32, 0xBF},  // DAC volume 0 dB
  {0x37, 0x08},  // DAC EQ bypass
};

RenderFn g_render = nullptr;

volatile uint32_t g_blocks = 0, g_cyclesSum = 0, g_cyclesPeak = 0, g_windowBlocks = 0;
volatile uint32_t g_late = 0, g_rxShort = 0, g_underruns = 0;
QueueHandle_t g_i2sEvents = nullptr;   // the driver's DMA events: underruns show up here
volatile float g_slotSq[2] = {0.0f, 0.0f};
volatile uint32_t g_slotN = 0;

constexpr uint32_t kCyclesPerBlock =
    (uint32_t)((uint64_t)240000000ULL * kBlock / kSampleRate);

void audioTask(void*) {
  static int16_t rx[kBlock * 2], tx[kBlock * 2];
  static float in[kBlock], out[kBlock];
  for (;;) {
    size_t got = 0;
    i2s_read(kPort, rx, sizeof(rx), &got, portMAX_DELAY);
    if (got != sizeof(rx)) ++g_rxShort;
    const uint32_t c0 = ESP.getCycleCount();
    // The ES8311 is mono: its ADC lands in the left slot.
    float sq0 = 0.0f, sq1 = 0.0f;
    for (size_t i = 0; i < kBlock; ++i) {
      const float a = rx[2 * i] * (1.0f / 32768.0f), b = rx[2 * i + 1] * (1.0f / 32768.0f);
      sq0 += a * a; sq1 += b * b;
      in[i] = a;
    }
    g_slotSq[0] += sq0; g_slotSq[1] += sq1; g_slotN += kBlock;
    g_render(in, out, kBlock);
    for (size_t i = 0; i < kBlock; ++i) {
      float s = out[i];
      s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
      const int16_t v = (int16_t)(s * 32767.0f);
      tx[2 * i] = v;
      tx[2 * i + 1] = v;
    }
    const uint32_t dc = ESP.getCycleCount() - c0;
    g_cyclesSum += dc;
    if (dc > g_cyclesPeak) g_cyclesPeak = dc;
    ++g_windowBlocks;
    ++g_blocks;
    size_t put = 0;
    i2s_write(kPort, tx, sizeof(tx), &put, 0);
    if (put != sizeof(tx)) {
      ++g_late;
      i2s_write(kPort, (const uint8_t*)tx + put, sizeof(tx) - put, &put, portMAX_DELAY);
    }
    // TX_Q_OVF: the DMA finished a buffer and found none newly written, so
    // it replayed silence (tx_desc_auto_clear). That is what you hear.
    i2s_event_t ev;
    while (g_i2sEvents && xQueueReceive(g_i2sEvents, &ev, 0) == pdTRUE)
      if (ev.type == I2S_EVENT_TX_Q_OVF) ++g_underruns;
  }
}

}  // namespace

bool begin(RenderFn fn) {
  g_render = fn;
  for (auto& r : kCodecInit) {
    if (!M5.In_I2C.writeRegister8(kCodecAddr, r[0], r[1], 400000)) return false;
  }
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = kBlock;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(kPort, &cfg, 16, &g_i2sEvents) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = kPinBck;
  pins.ws_io_num = kPinWs;
  pins.data_out_num = kPinDout;
  pins.data_in_num = kPinDin;
  if (i2s_set_pin(kPort, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(kPort);
  return xTaskCreatePinnedToCore(audioTask, "pd_audio", 4096, nullptr,
                                 configMAX_PRIORITIES - 1, nullptr, 1) == pdPASS;
}

bool setDacVolume(uint8_t reg) {
  return M5.In_I2C.writeRegister8(kCodecAddr, 0x32, reg, 400000);
}

Stats takeStats() {
  Stats s;
  const uint32_t n = g_windowBlocks ? g_windowBlocks : 1;
  s.blocks = g_blocks;
  s.loadAvgPermil = (uint32_t)((uint64_t)g_cyclesSum * 1000 / ((uint64_t)n * kCyclesPerBlock));
  s.loadPeakPermil = (uint32_t)((uint64_t)g_cyclesPeak * 1000 / kCyclesPerBlock);
  s.lateWrites = g_late;
  s.underruns = g_underruns;
  s.rxShort = g_rxShort;
  const uint32_t sn = g_slotN ? g_slotN : 1;
  s.slotRms[0] = sqrtf(g_slotSq[0] / sn); s.slotRms[1] = sqrtf(g_slotSq[1] / sn);
  g_slotSq[0] = 0.0f; g_slotSq[1] = 0.0f; g_slotN = 0;
  g_cyclesSum = 0; g_cyclesPeak = 0; g_windowBlocks = 0;
  return s;
}

}  // namespace audio
}  // namespace pd
