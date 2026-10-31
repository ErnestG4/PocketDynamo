// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// BLE-MIDI peripheral transport (NimBLE): advertises as PocketDynamo with
// the standard BLE-MIDI service and queues each packet the host writes,
// stamped on arrival. midi::BleDeframer and everything after it are
// shared with the other transports (midi/midi.h).
#pragma once
#include <cstddef>
#include <cstdint>

namespace pd {
namespace blemidi {

constexpr size_t kMaxPacket = 128;
struct Packet { uint8_t data[kMaxPacket]; uint8_t len; uint32_t at; };

bool begin();            // start the stack and advertising; false if it couldn't
bool connected();
bool poll(Packet* out);  // the next packet waiting, if any (control task)

}  // namespace blemidi
}  // namespace pd
