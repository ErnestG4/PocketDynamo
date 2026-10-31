// Copyright 2026 Combust.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#include "usb_midi.h"

#include <Arduino.h>
#include <atomic>
#include <cstring>

#include "soc/rtc_cntl_reg.h"

#if !ARDUINO_USB_MODE
#include "esp32-hal-tinyusb.h"

namespace {

constexpr UBaseType_t kUsbTaskPriority = 5;   // render: max - 1; control: 2
bool g_descriptorLoaded = false;

// USB task -> control task: packets with their arrival time.
struct Stamped { uint8_t p[4]; uint32_t at; };
constexpr uint32_t kRing = 64;
Stamped g_ring[kRing];
std::atomic<uint32_t> g_head{0}, g_tail{0};

// The MIDI interface pair (audio control + streaming) and its endpoints,
// added to the composite device beside the CDC serial port.
extern "C" uint16_t pdMidiDescriptor(uint8_t* dst, uint8_t* itf) {
  if (g_descriptorLoaded) return 0;
  g_descriptorLoaded = true;
  const uint8_t name = tinyusb_add_string_descriptor("PocketDynamo");
  const uint8_t ep = tinyusb_get_free_duplex_endpoint();
  if (ep == 0) return 0;
  const uint8_t desc[TUD_MIDI_DESC_LEN] = {TUD_MIDI_DESCRIPTOR(*itf, name, ep, (uint8_t)(0x80 | ep), 64)};
  *itf += 2;
  memcpy(dst, desc, sizeof desc);
  return sizeof desc;
}

// Interfaces must be enabled before USB.begin(), which the core calls in
// app_main before setup(): a static constructor runs first.
struct Register {
  Register() { tinyusb_enable_interface(USB_INTERFACE_MIDI, TUD_MIDI_DESC_LEN, pdMidiDescriptor); }
} g_register;

}  // namespace

// TinyUSB calls this from its task as packets arrive.
extern "C" void tud_midi_rx_cb(uint8_t) {
  Stamped s;
  while (tud_midi_packet_read(s.p)) {
    s.at = micros();
    const uint32_t h = g_head.load(std::memory_order_relaxed);
    if (h - g_tail.load(std::memory_order_acquire) >= kRing) continue;   // full: drop
    g_ring[h % kRing] = s;
    g_head.store(h + 1, std::memory_order_release);
  }
}

namespace pd {
namespace usbmidi {

// The core starts TinyUSB's task at the top priority with no core
// affinity, level with the render task: on core 1 the scheduler would
// time-slice the two and cut into renders. Below the render task it still
// runs while audio waits on I2S, and it stays above the control task.
void begin() {
  if (TaskHandle_t usb = xTaskGetHandle("usbd")) vTaskPrioritySet(usb, kUsbTaskPriority);
}

bool available() { return tud_mounted(); }

bool poll(midi::Msg* out, uint32_t* atUs) {
  for (;;) {
    const uint32_t t = g_tail.load(std::memory_order_relaxed);
    if (t == g_head.load(std::memory_order_acquire)) return false;
    const Stamped s = g_ring[t % kRing];
    g_tail.store(t + 1, std::memory_order_release);
    if (midi::fromUsbPacket(s.p, out)) { *atUs = s.at; return true; }
  }
}

void rebootToBootloader() { usb_persist_restart(RESTART_BOOTLOADER); }

}  // namespace usbmidi
}  // namespace pd

#else  // USB-Serial/JTAG build: no MIDI port

namespace pd {
namespace usbmidi {

void begin() {}
bool available() { return false; }
bool poll(midi::Msg*, uint32_t*) { return false; }
void rebootToBootloader() {
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

}  // namespace usbmidi
}  // namespace pd

#endif
