# Notices and credits

PocketDynamo is Copyright 2026 Combust and is licensed under the GNU Affero
General Public License v3.0 or later (see `LICENSE`). It started as a fork of
[cardputer-synth](https://github.com/chongjiazhen/cardputer-synth) by Chong Jia
Zhen; the `src/pd` firmware is new code, and none of cardputer-synth's code
remains.

## Ported from the author's own projects

These projects are by Combust, and the code ported from them is AGPL-3.0:

- **Plaits-lab ZX engines** (ZxPulse48k, ZxPhase48k): `engines/zx_pulse.h`,
  `engines/zx_phase.h`, `dsp/pulse_core.h`.
- **Brocot:** `engines/brocot_fm.h`, `dsp/stern_brocot.h`.
- **Kyklophoria:** the modal engine (`engines/modal.h`) and `dsp/fastmath.h`.
- **Audiothurgist:** the slice voice and click fabric behind
  `engines/sampler.h` and `engines/break.h`, the chop grammar in
  `synth/chop.h`, and the event-looper design behind `synth/looper.h`.
- **Lenticular:** `dsp/ars.h` and `dsp/ars_luts.h` (ARS gap statistics), the
  gap melody in `engines/cloud.h`, and the motif arp.
- **Model:Cycles generator core:** `dsp/rng.h` (seeded PRNG with named
  streams) and the randomizer design behind `synth/dice.h`.

## Third-party

- **Émilie Gillet / Mutable Instruments (MIT):** the Plaits chord table and
  hysteresis quantizer in `dsp/chords.h`, which carries its notice. The grain
  idea in CLOUD follows Clouds; the code there is new.
- **Chris Wellons:** the lowbias32 hash constants in `dsp/rng.h` (public domain).
- **George Marsaglia:** xorshift32.
- **M5Stack:** M5Unified, M5GFX and M5Cardputer (MIT), linked as libraries. The
  keyboard key remap in `hw/keys.cpp` follows M5Cardputer's TCA8418 driver.
  `variants/pocketdynamo` is the Arduino-ESP32 (LGPL-2.1) `m5stack_stamp_s3`
  variant with its own USB product ID.
- **h2zero / NimBLE-Arduino (Apache-2.0)** and Apache Mynewt NimBLE: the
  Bluetooth LE stack behind BLE MIDI, linked as a library.
- **TinyUSB (MIT, Ha Thach), via Arduino-ESP32:** the USB device stack behind
  USB MIDI.
- **The BLE-MIDI and USB-MIDI specifications** (Apple / MIDI Association, USB-IF):
  followed in `midi/midi.h`; no code copied.

## Techniques credited, no code copied

- ZX Spectrum pin-pulse beeper engines, and Shiru's Phaser1.
- Ian Fritz's double-pulse waveform (BROCOT FM's MORPH).
- The Stern-Brocot tree (Stern 1858, Brocot 1861).
- DX-style two-sample-averaged feedback.
- Nick Collins' and Audiothurgist's breakbeat-cutting ideas.
- Rings' mode-sharing rule (modal engine).
