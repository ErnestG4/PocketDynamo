// Full-duplex audio for the Cardputer ADV: ES8311 codec over I2C, I2S1 with
// DAC out and mic in on one shared clock, and a render task pinned to core 1
// at top priority. The task blocks on the RX DMA, so the codec clock paces
// the render; the callback gets the mic block and fills the output block.
#pragma once
#include <cstddef>
#include <cstdint>

namespace pd {
namespace audio {

constexpr uint32_t kSampleRate = 48000;
constexpr size_t   kBlock      = 32;   // frames per render (0.67 ms @ 48 kHz)

// in: mic, mono, -1..1 (kBlock samples). out: mono, -1..1, written by callback.
using RenderFn = void (*)(const float* in, float* out, size_t n);

struct Stats {
  uint32_t blocks;         // rendered since boot
  uint32_t loadAvgPermil;  // render cycles / block period, last report window
  uint32_t loadPeakPermil;
  uint32_t lateWrites;     // TX writes that had to wait on a full DMA (normal)
  uint32_t rxShort;        // RX reads that came back short (should stay 0)
  float    slotRms[2];     // raw RMS of each I2S RX slot, for finding the mic
};

bool begin(RenderFn fn);
// Codec DAC digital volume, 0.5 dB steps: 0xBF = 0 dB, 0xFF = +32 dB.
// I2C write — call from the control side, never from the render.
bool setDacVolume(uint8_t reg);
// Snapshot + reset the averaging window. Call from the UI side.
Stats takeStats();

}  // namespace audio
}  // namespace pd
