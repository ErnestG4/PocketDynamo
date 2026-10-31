// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Cardputer ADV keyboard: the TCA8418 keypad scanner on the internal I2C bus.
// It queues hardware press/release events in a 10-deep FIFO and pulls INT
// (GPIO11) low while any are pending, so we get true note-on/note-off edges
// with no matrix diffing. Call poll() from the control task only — it shares
// the I2C bus with the codec.
#pragma once
#include <cstdint>

namespace pd {
namespace keys {

struct KeyEvent {
  uint8_t row;      // 0..3, top (number row) to bottom (space row)
  uint8_t col;      // 0..13, left to right
  bool    pressed;
};

bool begin();
// Drains pending events into out[], returns how many (<= max).
int poll(KeyEvent* out, int max);

}  // namespace keys
}  // namespace pd
