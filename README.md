# PocketDynamo

An 8-voice old-school synth for the **M5Stack Cardputer ADV**.

- **Seven engines:**
  - ZX PULSE and ZX PHASE (ZX Spectrum beeper techniques)
  - BROCOT FM (true FM on Stern-Brocot ratios)
  - MODAL (struck resonators: bells, mallets, gongs)
  - SAMPLE, CLOUD and BREAK (three ways to play a sound you record from the mic)
- **A keyboard laid out for playing:** scale-locked layouts, a chromatic one, and 49-EDO tunings.
- **Hold strips:** CHORD, RATCHET and ARP.
- **Also:** USB MIDI in, tilt control, a looper, and patches saved to the SD card.

## Install

Download `PocketDynamo-CardputerADV.bin` from the
[latest release](https://github.com/ErnestG4/PocketDynamo/releases/latest), then either:

- copy it to the SD card and install it with **M5 Launcher**, or
- flash it with esptool: `esptool --chip esp32s3 write-flash 0x0 PocketDynamo-CardputerADV.bin`

## Play

See the **[manual](docs/pocketdynamo.md)**. In short:
- Columns 1–12 play notes.
- `\` changes engine and `tab` changes layout.
- Hold `fn` for the four controls on `z x c v b n m ,`.

## Build

With [PlatformIO](https://platformio.org): `tools/flash.sh` (it asks the
running firmware to drop into the bootloader, then runs `pio run -e pd -t
upload`). Env `pd-jtag` is the same firmware without USB MIDI, for debugging:
it prints crash backtraces over USB.

The code lives in `src/pd`. Host tests sit next to the code as `*_test.cpp` and
build with plain g++ (see `CLAUDE.md`).

## Credits

AGPL-3.0-or-later licensed. PocketDynamo is a fork of
[cardputer-synth](https://github.com/chongjiazhen/cardputer-synth) by Chong Jia
Zhen, whose original firmware is still in `src/synth`. It carries techniques
from Combust's Brocot, Kyklophoria, Audiothurgist, Lenticular and Model:Cycles
projects, and from Mutable Instruments. See [NOTICE.md](NOTICE.md).
