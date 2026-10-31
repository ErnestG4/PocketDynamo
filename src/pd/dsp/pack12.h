// 12-bit sample storage, two samples in three bytes: the take and the echo's
// tape. at()/put() speak 16-bit values whose low 4 bits are zero; put()
// rounds to the nearest 12-bit step and clamps. Pure — host-testable.
#pragma once
#include <cstddef>
#include <cstdint>

namespace pd {

template <uint32_t kSamples>
struct Pack12 {
  static_assert(kSamples % 2 == 0, "samples come in pairs");

  int16_t at(uint32_t i) const {
    const uint8_t* p = bytes + (i >> 1) * 3;
    return (i & 1) ? (int16_t)((uint16_t)p[2] << 8 | (p[1] & 0xF0))
                   : (int16_t)((uint16_t)(p[1] & 0x0F) << 12 | (uint16_t)p[0] << 4);
  }
  // Samples i and i + 1 together (an interpolated read): one address, one
  // parity test. Same values as at(i), at(i + 1).
  inline void pair(uint32_t i, int* a, int* b) const {
    const uint8_t* p = bytes + (i >> 1) * 3;
    if (i & 1) {   // i is the second of its pair; i + 1 starts the next
      *a = (int16_t)((uint16_t)p[2] << 8 | (p[1] & 0xF0));
      *b = (int16_t)((uint16_t)(p[4] & 0x0F) << 12 | (uint16_t)p[3] << 4);
    } else {       // both in these three bytes
      *a = (int16_t)((uint16_t)(p[1] & 0x0F) << 12 | (uint16_t)p[0] << 4);
      *b = (int16_t)((uint16_t)p[2] << 8 | (p[1] & 0xF0));
    }
  }
  void put(uint32_t i, int v) {
    int r = (v + 8) >> 4;
    r = r > 2047 ? 2047 : (r < -2048 ? -2048 : r);
    const uint16_t u = (uint16_t)(r << 4);
    uint8_t* p = bytes + (i >> 1) * 3;
    if (i & 1) { p[1] = (uint8_t)((p[1] & 0x0F) | (u & 0xF0)); p[2] = (uint8_t)(u >> 8); }
    else       { p[0] = (uint8_t)(u >> 4); p[1] = (uint8_t)((p[1] & 0xF0) | (u >> 12)); }
  }

  uint8_t bytes[kSamples / 2 * 3] = {};
};

}  // namespace pd
