# PocketDynamo

An 8-voice old-school synth for the **M5Stack Cardputer ADV**.

- **Seven engines:**
  - ZX PULSE and ZX PHASE, from the ZX Spectrum's beeper
  - BROCOT FM, true FM on Stern-Brocot ratios
  - MODAL: bells, mallets, strings and gongs
  - SAMPLE, CLOUD and BREAK, three ways to play a sound you record from the mic
- **Effects:**
  - a tempo-synced tape echo that doubles as a small reverb
  - a filter
  - crush, 1-bit PWM and drive
  - wavefolding, ring mod and tremolo
- **A keyboard laid out for playing:**
  - scale-locked layouts, a chromatic one, and 49-step tunings
  - CHORD, RATCHET and ARP strips
- **Tilt** that can play any control or effect.
- **MIDI in** over USB and Bluetooth.
- **Also:** a looper, and patches saved to the SD card.

## Install

Download `PocketDynamo-CardputerADV.bin` from the
[latest release](https://github.com/ErnestG4/PocketDynamo/releases/latest), then either:

- copy it to the SD card and install it with **M5 Launcher**, or
- flash it with esptool, then press reset:
  `esptool --chip esp32s3 write-flash 0x0 PocketDynamo-CardputerADV.bin`

## Play

See the **[manual](docs/pocketdynamo.md)**. In short:
- **Notes:** columns 1–12 play notes.
- **Engine:** `\` changes it.
- **Layout:** `tab` changes it.
- **Controls:** hold `fn` for the four controls on `z x c v b n m ,`.
- **Effects:** FN+`p` switches those keys to the effects.

## Build

Build and flash with [PlatformIO](https://platformio.org): `tools/flash.sh`.
- The code lives in `src/pd`.
- Host tests sit next to the code as `*_test.cpp` and build with plain g++ (see `CLAUDE.md`).
- Env `pd-jtag` is the same firmware without USB MIDI, for reading crash backtraces.

## Credits

AGPL-3.0-or-later licensed. PocketDynamo is a fork of
[cardputer-synth](https://github.com/chongjiazhen/cardputer-synth) by Chong Jia
Zhen, whose original firmware is still in `src/synth`. It carries code from
Combust's own synth projects, and from Mutable Instruments. See
[NOTICE.md](NOTICE.md).
