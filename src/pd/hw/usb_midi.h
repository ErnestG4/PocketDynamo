// USB-MIDI device transport: the thin wire layer. TinyUSB frames USB-MIDI
// event packets; midi::fromUsbPacket and everything after it are shared
// with the other transports (midi/midi.h). Built with ARDUINO_USB_MODE=0
// (TinyUSB on the S3's OTG port, Serial a TinyUSB CDC beside the MIDI
// port); in USB-Serial/JTAG builds it compiles to nothing and poll() never
// returns a message.
#pragma once
#include "../midi/midi.h"

namespace pd {
namespace usbmidi {

void begin();                 // call from setup(): settles the USB task's priority
bool available();             // a USB-MIDI build, and a host has configured the port
// The next message waiting, if any, and when it arrived (micros): packets
// are stamped in the USB task as they land, so MIDI clock keeps its timing
// however late the control task reads them (control task only).
bool poll(midi::Msg* out, uint32_t* atUs);
void rebootToBootloader();    // into ROM download mode, ready to flash

}  // namespace usbmidi
}  // namespace pd
