# PocketDynamo

An old-school synthesis instrument for the **M5Stack Cardputer ADV** (ESP32-S3):
8 voices, full-duplex audio at 48 kHz, and a keyboard laid out for playing
rather than typing.

- **Engines:** ZX PULSE (Follin pin trains), ZX PHASE (Phaser1 1-bit
  interference), BROCOT FM (parallel true FM on Stern-Brocot ratios), SAMPLE
  (the mic take as a varispeed record) and CLOUD (the take as an ARS-timed
  granular cloud).
- **Playing surfaces:** scale-locked layouts in fourths, chromatic, the tracker
  piano, and three 49-EDO surfaces.
- **Hold strips:** CHORD, RATCHET and ARP (including 50 Hz chip arps), each
  latchable.
- **Also:** a sustain pedal, tilt control of TIMBRE and MORPH, a dice key with
  undo, an event looper with overdub, mic recording, and patches on SD.

Build and flash: `pio run -e pd -t upload`. **How to play:**
[docs/pocketdynamo.md](docs/pocketdynamo.md). Credits and licences:
[NOTICE.md](NOTICE.md).

PocketDynamo forks [cardputer-synth](https://github.com/chongjiazhen/cardputer-synth).
Its original firmware is still here under `src/synth` (`pio run -e synth`) and
is described below.

---

# cardputer-synth (original firmware, `src/synth`)

A polyphonic synthesizer firmware for the **M5Stack Cardputer ADV** (ESP32-S3).
Six-voice subtractive engine with a resonant filter, modulation matrix, a mic
sampler, IMU tilt expression, and USB-MIDI + BLE-MIDI output.

> Work in progress / hobby project. It plays and sounds like a synth; it is not
> a finished commercial instrument. Built in phases — see the roadmap.

## Features

- **6-voice polyphony** — fixed voice pool, oldest-voice stealing.
- **Waveforms:** sine / saw / square / triangle (keys `1`–`4`), loudness-matched;
  saw and square are anti-aliased (PolyBLEP).
- **Resonant filter** — per-voice state-variable filter (LP/HP/BP), cutoff on
  `,` / `.` and resonance on `6` / `7`.
- **Dual envelopes** — an amp ADSR and a filter/mod ADSR (default: filter
  envelope opens the cutoff on attack).
- **Dual LFOs + modulation matrix** — routes envelopes / LFOs / velocity /
  key-track to pitch, cutoff, resonance, and amp.
- **Mic sampler** — record ~1 s from the mic (`r`), play it pitched across the
  keyboard (`5`); `8` toggles **tape** (varispeed) vs **grain** (pitch-shift that
  preserves length).
- **IMU expression** — accel tilt → note velocity (latched per note) + vibrato
  depth; gyro twist → pitch bend (self-centering); graceful no-op if IMU absent.
- **MIDI out** — USB-MIDI and BLE-MIDI (device "Cardputer Synth"): note on/off
  with tilt velocity + CC 1 (vibrato) + pitch-bend.
- **Arpeggiator** — `Fn+a` toggles it on; while on, held note-keys are
  arpeggiated (Up / Down / Up-Down / Random, `Fn+s` cycles pattern, `Fn+-`/`Fn+=`
  adjust tempo) instead of sounding directly. Sends MIDI note on/off per step.

## Controls

```
z x c v b n m   white notes       1-4  waveform          , .  cutoff down/up
s d g h j       black notes       [ ]  octave down/up     6 7  resonance down/up
- =  volume down/up               5    play sampler       8    tape / grain
r    record ~1s from mic          `    panic (kill all)

Fn+a  arp on/off      Fn+s  cycle arp pattern      Fn+-/Fn+=  arp tempo down/up
```
IMU: tilt fwd/back → velocity · tilt left/right → vibrato · gyro twist → pitch bend.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -e synth            # audio only
pio run -e synth-usb-midi   # audio + USB MIDI
pio run -e synth-ble-midi   # audio + BLE MIDI
pio run -e synth -t upload  # flash
```

## Host tests

All DSP/logic lives in pure headers (Arduino-free) and is host-testable with g++:

```bash
for t in oscillator adsr notes imu_map filter lfo modmatrix voice sampler; do
  g++ -std=c++17 -I src/synth src/synth/${t}_test.cpp -o /tmp/t && /tmp/t
done
```

## Play on desktop (no flashing)

A small audio harness plays the synth on a PC so you can iterate by ear without a
flash cycle. See [host/README.md](host/README.md):

```bash
bash host/build.sh && host/desktop_synth [sample.wav]
```

## Simulate (Wokwi)

Open in VS Code with the Wokwi extension, or upload `firmware.bin` + `diagram.json`
to https://wokwi.com. **Limits:** Wokwi proves the firmware boots but does not
emulate audio, MIDI, or the IMU, and models the classic Cardputer (not the ADV),
so the key map isn't 1:1. To actually hear it, flash real hardware and follow
[docs/hardware-verification.md](docs/hardware-verification.md).

## Design & roadmap

Full architecture and the phased build plan are in
[docs/design/architecture.md](docs/design/architecture.md) and
[docs/roadmap.md](docs/roadmap.md).

## License

AGPL-3.0-or-later — see [LICENSE](LICENSE). cardputer-synth's own code is MIT ([LICENSE-cardputer-synth](LICENSE-cardputer-synth)).
