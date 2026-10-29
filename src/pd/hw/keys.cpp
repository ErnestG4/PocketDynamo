#include "keys.h"

#include <Arduino.h>
#include <M5Unified.h>

namespace pd {
namespace keys {
namespace {

constexpr uint8_t kAddr = 0x34;
constexpr int     kIntPin = 11;
constexpr uint8_t kRegCfg = 0x01, kRegIntStat = 0x02, kRegKeyLckEc = 0x03,
                  kRegKeyEventA = 0x04, kRegKpGpio1 = 0x1D, kRegKpGpio2 = 0x1E,
                  kRegKpGpio3 = 0x1F;
constexpr uint32_t kI2cHz = 400000;

uint8_t read8(uint8_t reg) { return M5.In_I2C.readRegister8(kAddr, reg, kI2cHz); }
bool write8(uint8_t reg, uint8_t v) { return M5.In_I2C.writeRegister8(kAddr, reg, v, kI2cHz); }

}  // namespace

bool begin() {
  // 7 rows x 8 columns of keypad matrix, nothing as GPIO.
  if (!write8(kRegKpGpio1, 0x7F)) return false;
  write8(kRegKpGpio2, 0xFF);
  write8(kRegKpGpio3, 0x00);
  while (read8(kRegKeyLckEc) & 0x0F) read8(kRegKeyEventA);   // flush the FIFO
  write8(kRegIntStat, 0x03);
  write8(kRegCfg, read8(kRegCfg) | 0x01);                     // KE_IEN: key events drive INT
  pinMode(kIntPin, INPUT);
  return true;
}

int poll(KeyEvent* out, int max) {
  if (digitalRead(kIntPin) != LOW) return 0;   // INT is active low; idle = no I2C traffic
  int n = 0;
  while (n < max) {
    const uint8_t raw = read8(kRegKeyEventA);
    if (raw == 0) break;
    // Chip numbering: key = row*10 + col + 1 on its 7x8 matrix. M5's remap
    // (M5Cardputer TCA8418.cpp, MIT) folds that onto the 4x14 Cardputer grid.
    const uint8_t k = (raw & 0x7F) - 1, mr = k / 10, mc = k % 10;
    KeyEvent& e = out[n++];
    e.pressed = (raw & 0x80) != 0;
    e.col = (uint8_t)(mr * 2 + (mc > 3 ? 1 : 0));
    e.row = (uint8_t)(mc % 4);
  }
  write8(kRegIntStat, 0x01);
  return n;
}

}  // namespace keys
}  // namespace pd
