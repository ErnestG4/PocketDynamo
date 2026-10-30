# CLAUDE.md — PocketDynamo

Polyphonic old-school synth firmware for the M5Stack Cardputer ADV (ESP32-S3,
8 MB flash, no PSRAM, single-precision FPU). PlatformIO. A fork of
cardputer-synth. The original firmware is in `src/synth`; new work is in `src/pd`.

## Layout (`src/pd`)

- `hw/`: the only Arduino/M5 code.
  - `audio_io`: ES8311 + I2S full duplex, render task on core 1.
  - `keys`: TCA8418 event FIFO.
  - `storage`: microSD on SPI2; the display owns SPI3.
- `dsp/`: shared DSP (sine LUT, chords, ARS gap statistics, PRNG, Stern-Brocot).
- `engines/`: `pd::Engine` implementations (Plaits-shaped: note +
  harmonics/timbre/morph/macro + trigger, rendered in 32-sample blocks at
  48 kHz).
- `midi/`: MIDI in, shared by every transport.
- `synth/`: the instrument (voice pool), events + SPSC queue, sample store,
  looper, dice.
- `ui/`: keymap, playing surfaces, arp patterns, patch format, tilt mapping.
- `main.cpp`: control task (core 0) and display.
- `*_test.cpp`: g++ host tests, `g++ -std=c++17 -O2 -I src/pd <test> -o t && ./t`.

## Conventions

- Everything outside `hw/` and `main.cpp` stays free of Arduino/M5 includes and
  host-tests with g++.
- Float only in audio code: `double` is soft-float on the LX7. No heap, no libm
  per sample (per block is fine), uint32 phase accumulators.
- Bench on the device with serial `B` (8 voices per engine, % of one core per
  32-sample block). Keep every engine's peak well under 100%.
- Build/flash: `tools/flash.sh` (env `pd`), with the deps pinned in
  `platformio.ini` (unpinned espressif32 8.x + M5Unified 0.2.28 crash on boot).
  Env `pd` runs TinyUSB (serial CDC + USB MIDI), so a plain `pio upload` only
  knocks it into the bootloader on the first try; the script sends serial `U`
  and waits for the bootloader's port. The CDC port only talks once the host
  raises DTR. Env `pd-jtag` keeps the USB-Serial/JTAG port, for backtraces.
- MIDI: `midi/` is the shared, transport-free part (parser, USB packets,
  clock tempo, the mapping); `hw/usb_midi` is only the wire. A new transport
  (BLE) should hand bytes to `midi::Parser`, nothing more.
- Player docs: `docs/pocketdynamo.md`. Credits: `NOTICE.md`.
