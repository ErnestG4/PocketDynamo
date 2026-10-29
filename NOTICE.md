# Notices and credits

PocketDynamo is licensed under AGPL-3.0-or-later (see `LICENSE`). cardputer-synth's code keeps its MIT notice (`LICENSE-cardputer-synth`). It is a fork of
[cardputer-synth](https://github.com/chongjiazhen/cardputer-synth) by Chong Jia
Zhen (MIT). The `src/pd` framework is new code.

## Ported from the author's own projects

These projects are by Combust. Code ported from them is AGPL-3.0:

- **Plaits-lab ZX engines** (ZxPulse48k, ZxPhase48k): `engines/zx_pulse.h`,
  `engines/zx_phase.h`.
- **Brocot** : `engines/brocot_fm.h`, `dsp/stern_brocot.h`.
- **Kyklophoria** : the modal engine.
- **Audiothurgist** : the slice voice and click fabric
  behind `engines/sampler.h`, and the event-looper design behind
  `synth/looper.h`.
- **Lenticular**: `dsp/ars.h` and `dsp/ars_luts.h` (ARS gap statistics,
  ported unchanged), the gap melody in `engines/cloud.h`, and the motif arp.
- **Model:Cycles** generator core: `dsp/rng.h` (seeded PRNG with named
  streams) and the randomizer design behind `synth/dice.h`.

## Third-party

- **Émilie Gillet / Mutable Instruments (MIT):** the Plaits chord table and
  hysteresis quantizer in `dsp/chords.h`, which carries its notice. The grain
  idea in CLOUD follows Clouds; the code there is new.
- **Chris Wellons:** the lowbias32 hash constants in `dsp/rng.h` (public domain).
- **George Marsaglia:** xorshift32.
- **M5Stack:** M5Unified, M5GFX and M5Cardputer (MIT). The keyboard key remap
  in `hw/keys.cpp` follows M5Cardputer's TCA8418 driver.

## Techniques credited, no code copied

- Tim Follin's pin-pulse beeper engines, and Shiru's Phaser1.
- Ian Fritz's double-pulse waveform (BROCOT FM's MORPH).
- The Stern-Brocot tree (Stern 1858, Brocot 1861).
- DX-style two-sample-averaged feedback.
- Nick Collins' and Audiothurgist's breakbeat-cutting ideas.
- Rings' mode-sharing rule (modal engine).
