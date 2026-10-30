# Playing PocketDynamo

PocketDynamo is an 8-voice instrument for the M5Stack Cardputer ADV, built
around old-school synthesis techniques. Every engine has the same four
controls, **HARMONICS, TIMBRE, MORPH and MACRO** (MACRO's middle is neutral).
The keys give the pitch.

It starts on ZX PULSE, the MAJOR 4THS surface, root C3, 120 bpm, at +6 dB.

## The keyboard

Columns 1–12 of all four rows play notes. The bottom-left note key (`opt`) is
the root. The two outer columns are controls:

| Left | | Right | |
|---|---|---|---|
| `` ` `` | **dice**: tap to nudge the controls, hold ½ s to roll new ones | `del` | **strip**: NOTES → CHORD → RATCHET → ARP |
| `tab` | next **surface** | `\` | next **engine** |
| `fn` | function layer | `enter` | all notes off |
| `ctrl` | **tilt**: tap for on/off, hold + a control key to route it | `space` | **sustain pedal** |

The keys aren't velocity-sensitive, and all notes play at one level. Releasing a
key starts its engine's release: short for the buzzers and stabs, about 3 s
for MODAL so bells ring out.

### Surfaces (`tab`, FN+`tab` back)

Most surfaces are isomorphic: one column right is one scale step, and one row
up is a fourth. A chord or melody shape is the same anywhere on the grid.

| Surface | What it is |
|---|---|
| MAJOR / MINOR / DORIAN 4THS | scale-locked: every key is in the scale |
| PENTA 4THS | minor pentatonic, rows a fourth apart |
| CHROMA 4THS | chromatic, rows a fourth apart, like a bass |
| PIANO | the tracker layout: `z s x d c v…` lower octave, `q 2 w 3 e…` upper. While a strip is on, the number row's sharps become strip keys |
| 49 SUPERPYTH | a major scale from 49-EDO's sharp fifth: wide whole tones, 49-cent half steps, a major third near 9/7 |
| 49 7x7 | 7 equal steps per octave across a row; each row up is one 49th of an octave sharper, so shapes across rows shimmer |
| 49 CHROMA | every 49-EDO step in order, about one octave across the keyboard |

### Strips (`del`)

In CHORD, RATCHET or ARP the number row's 12 keys stop playing notes and
become hold keys (their labels are on screen). Hold one, then play:

- **CHORD:** each key plays that chord. On scale surfaces chords are stacked
  from scale degrees, so they stay in key: 5, TRI, S2, S4, 6, 7, 9, AD9, 4TH,
  OPN, OCT, CLS. On chromatic surfaces they're the usual shapes snapped to the
  tuning: 5, M, m, S2, S4, M7, m7, 7, m9, M9, 69, and 7:4, a natural-seventh
  chord. In 49-EDO each comes out bent its own way.
- **RATCHET:** held notes re-strike at that rate, synced to the tempo: 4, 8.,
  4t, 8, 16., 8t, 16, 32., 16t, 32, 32t, 64.
- **ARP:** held and sustained notes arpeggiate one at a time.
  - Keys 1–6 (UP, DN, UD, ORD as played, RND, MTF) step in 16ths at the tempo.
  - Keys 7–12 are the same patterns as **chip arps**: one voice re-pitched at
    50 Hz, the way ZX Spectrum music drivers stepped once per frame.
  - MOTIF takes both its rhythm and its notes from Lenticular's three-distance
    gap statistics.

**Double-tap** a strip key to latch it, and tap it again to let go. A held key
shows cyan; a latched one shows green.

### Sustain (`space`)

Notes you let go of while `space` is down keep sounding until you release
`space`. Playing a sustained key again re-strikes it. Sustained notes stay in
the ARP's pool.

### Tilt (`ctrl`)

Tap `ctrl` to switch tilt on or off. While it's on, the device's angle sets
two controls directly:
- **Side to side:** 0 on its left edge, 1 on its right. TIMBRE by default.
- **Front/back:** 0 flat, 0.8 standing up, 1 tipped past upright. MORPH by
  default.

**Choosing what tilt drives:** hold `ctrl` and tap a control's FN key. Each
control has a down/up pair; the left key of the pair puts it on side-to-side,
the right key on front/back:

| Hold `ctrl` + | side to side | front/back |
|---|---|---|
| HARMONICS | `z` | `x` |
| TIMBRE | `c` | `v` |
| MORPH | `b` | `n` |
| MACRO | `m` | `,` |
| echo feedback | `d` | `f` |
| echo mix | `s` | `g` |

On the FX page the same keys put the effects on tilt instead: `z`/`x` TONE,
`c`/`v` GRIT, `b`/`n` SHAPE, `m`/`,` SPACE.

Routing a control switches tilt on. A control already on the other axis
swaps over. Tilted bars turn green, and the echo label turns cyan while tilt
drives the echo. Switching tilt off returns everything to its set value. A
patch saves the routing (not whether tilt is on).

### Dice (`` ` ``)

- **Tap:** mutate. Each control has even odds of a small nudge, and at least
  one always moves.
- **Hold ½ s:** roll a fresh sound. HARMONICS leans low and TIMBRE stays clear
  of the sparse bottom.
- **FN+`del`:** undo the last dice; press it again to redo.

### FN layer (hold `fn`)

| FN + | |
|---|---|
| `z`/`x` `c`/`v` `b`/`n` `m`/`,` | HARMONICS, TIMBRE, MORPH, MACRO down/up. Hold to sweep |
| `[` / `]` | volume in 3 dB steps, up to +12 dB. Hold to repeat |
| `` ` `` / `1` | octave down / up |
| `q` / `w` | root down / up one step of the current tuning (a semitone, or 1/49 octave on 49 surfaces) |
| `9` / `0` | tempo, 40–300 bpm |
| `e` | **echo** time: off → 1/8 → dotted 1/8 → 1/4 → off |
| `d` / `f` | echo feedback down / up. Hold to repeat |
| `s` / `g` | echo mix down / up. Hold to repeat |
| `p` | **FX page**: the four bars (and FN+`z`…`,`) switch between the sound and the effects |
| `t` / `y` / `u` | pick the effect in TONE / GRIT / SHAPE |
| `tab` / `\` | previous surface / previous engine |
| `del` | undo / redo the last dice |
| `r` (hold) | **record** from the mic into the take |
| `l` | **looper**: record → play → overdub → play… |
| `k` | looper stop; press again to clear |
| `2`…`8` | load patch 2…8 from the SD card |
| `shift` + `2`…`8` | save patch 2…8 |

## Engines (`\`)

| Engine | HARMONICS | TIMBRE | MORPH | MACRO (CCW / CW) |
|---|---|---|---|---|
| **ZX PULSE**: ZX Spectrum beeper pin trains | chord (Plaits' set) | pin width, which is also the level | builds the stack (lower half), then an arp whose rate rises (upper half) | exaggerated pitch pull / echo |
| **ZX PHASE**: Phaser1-style 1-bit interference | mix method (XOR / OR / AND), with a 1-bit flanger inside each zone that steps up the harmonic series | PWM sweep on the second oscillator | unison beating → just ratios (4/3, 3/2, 2, 3, 4) → noise | kick-slide drum / ghost carrier |
| **BROCOT FM**: parallel true FM on Stern-Brocot ratios | ratio family: harmonic → Avatar → Golden convergents (sweep it for the zipper) | FM index, tamed on high notes | DX feedback ← sine (middle) → Fritz double pulse | brass swell / bell strike |
| **MODAL**: struck resonators | the object: string, stiff string, marimba, glockenspiel, kalimba / Rhodes tine, drum, bell, gong | the mallet, felt → metal (shapes the next strike) | decay, 50 ms → 20 s | metallic noise wash / Rhodes pickup bark |
| **SAMPLE**: the take as a varispeed record | slice (or start point) | length, 20 ms → the rest of the take | forward, loop, ping-pong, reverse | tape-dull / SP grit |
| **CLOUD**: the take as a granular cloud | where in the take | grain size, 15 → 400 ms | timing: CHIP, SILK, MOTIF, LOOSE, CLUMP | density, 2 → 120 grains/s |
| **BREAK**: the take as a one-bar break, cut up live | chop: verbatim → DJ edit → chops → rolls → micro bursts | which of 32 patterns | gate length | tape-dull / SP grit |

## The echo (FN+`e`)

A tape echo on everything you play, synced to the tempo. FN+`e` steps the
time through 1/8, dotted 1/8 and 1/4, then off; FN+`d`/`f` set how long the
repeats go on, and FN+`s`/`g` how loud they are. Both can go on tilt. Its tape is 0.6 s long: at slow tempos a time that won't fit
halves until it does. It behaves like tape:
- Repeats get darker, thinner and grittier as they come round.
- A change of tempo or time bends the repeats as the head glides.
- Switching it off stops new sound reaching the tape, but the tail rings out.

The echo's time, feedback and mix are saved with a patch. The status line shows `E` and the
time, green while the echo is on.

## The FX page (FN+`p`)

FN+`p` flips the four bars from the sound's controls to four effects on
everything you play, and back. On the FX page FN+`z`/`x`, `c`/`v`, `b`/`n`
and `m`/`,` set the effects' amounts instead; each is off at 0. The bars turn
cyan so you can tell which page you're on.

| Bar | FN+ | Effects | What the amount does |
|---|---|---|---|
| **TONE** | `t` | LP / BP / HP filter | LP closes from fully open, HP opens up from the lows, BP sweeps its peak from 100 Hz to 8 kHz; resonance rises with it |
| **GRIT** | `y` | CRUSH / PWM / DRIVE | CRUSH: fewer bits and a lower sample rate. PWM: the sound re-encoded as a one-bit pulse train, the way the Spectrum's beeper played samples, its carrier dropping as you turn up. DRIVE: saturation |
| **SHAPE** | `u` | FOLD / RING / TREM | FOLD: a wavefolder. RING: ring modulation, 30 Hz–2 kHz. TREM: tremolo in 8th notes at the tempo |
| **SPACE** | | reverb | With the echo on, smears each repeat into a wash. With the echo off, turns the tape into a small room |

They run in the order SHAPE → GRIT → TONE → echo. On the FX page, holding
`ctrl` and tapping a bar's key puts that effect on tilt, as on the sound page.
MIDI CC 20–23 set the four amounts, and a patch saves the effects and their
amounts.

## The take (FN+`r`)

Hold FN+`r` to record from the mic, for up to 2.5 s at 24 kHz and 12 bits (SP-1200
territory). Sound stops while you record, so the speaker isn't recorded.
- The take is trimmed, normalized and sliced at its onsets. The bottom line
  then shows its length and slice count.
- Recording switches to SAMPLE unless you're on SAMPLE, CLOUD or BREAK.
- **SAMPLE and CLOUD:** C4 plays the take as recorded.
- **BREAK:** the take is one bar on a 16th grid at the tempo. Tempo is pitch,
  the turntable way, and the key adds its own pitch on top. Every re-strike
  restarts the bar, so RATCHET stutters the break.
- **CLOUD in CHIP or MOTIF:** grains take chord tones (root, fifth, octave,
  octave + fifth) from the timing statistics.

## The looper (FN+`l`, FN+`k`)

The looper records what you send the synth, not audio: notes, chords, arp and
ratchet output, control sweeps and engine changes.
- The first pass snaps to whole beats at the current tempo.
- Overdubs join at the next lap.
- Looped notes never cut off a note you're playing live.
- The bottom line shows the state, lap length and a progress bar.

## Patches (FN+`2`…`8`)

A patch saves the engine, surface, root, octave, tempo, the four controls, the
echo, the FX page and the tilt routing, plus the take. It doesn't save the strip, tilt or volume.
- Patches are plain `key=value` text in `/pocketdynamo/pN.txt` on the SD card,
  so you can edit them by hand. The take goes alongside as `pN.wav`.
- Any mono 16-bit WAV saved as `pN.wav` next to a patch loads, resampled to
  24 kHz.

## MIDI (USB and Bluetooth)

PocketDynamo listens for MIDI two ways, both at once, on every channel:

- **USB:** plug it into a computer, phone or tablet and it shows up as a
  USB-MIDI device called **PocketDynamo** (beside its serial port).
- **Bluetooth LE:** it advertises as **PocketDynamo** whenever it's on.
  Connect from a BLE-MIDI app or your system's Bluetooth MIDI settings (on
  macOS, Audio MIDI Setup → MIDI Studio → Bluetooth; on iOS, from the app).
  **BT** in the top right is grey while it waits and cyan once connected.
  Bluetooth MIDI clock keeps its tempo: messages are placed by the sender's
  own timestamps, not by when the radio delivers them.

- **Notes** play as if they were keys, so they take the strip you're holding:
  CHORD, RATCHET and ARP all work on MIDI notes, and so do sustain and the
  looper. A note that's on the current surface plays exactly like that key,
  so CHORD stacks stay in the scale; a note off it (off the scale, or off a
  49-EDO grid) plays its own pitch with the chord's chromatic shape.
- **Velocity** sets the level (the keys play at a fixed 100).
- **CC 64** is the sustain pedal; **CC 120/123** all notes off.
- **The four controls:** CC 16–19 are HARMONICS, TIMBRE, MORPH and MACRO.
  The usual performance CCs land on them too: 71 (resonance) HARMONICS,
  74 (brightness) TIMBRE, 1 (mod wheel) MORPH, 2 (breath) MACRO.
- **CC 91** (the usual effects send) is the echo mix; **CC 20–23** are the
  FX page's TONE, GRIT, SHAPE and SPACE.
- **Pitch bend** bends everything ±2 semitones.
- **Program change** 0–6 picks the engine (in the `\` order).
- **MIDI clock** sets the tempo, so the arp, ratchets, BREAK and the looper
  follow your DAW.

`MIDI` lights in the top right while messages come in, from either.

## The screen

- **Top:** the engine, the surface, then root/octave, tempo and volume.
- **Status line:** the strip, `SUS` and `TILT` (lit when on), the take (or REC
  with a timer), the echo time and CPU load.
- **Middle:** the four control bars. Green means tilt is driving that bar.
- **Strip row:** the 12 strip keys, when a strip is on.
- **Bottom line:** messages (MUTATE, SAVED 3, TAKE 1.2s 6 SLICES…), the looper's
  state, or a key hint.

## Serial (115200, for scripts)

| Command | |
|---|---|
| `i` | print state |
| `B` | silent CPU bench of every engine with 8 voices |
| `S` | SD card status |
| `[` / `]` | volume |
| `K<row><col-hex><+/->` | press or release a key (row 0–3 from the top, column 0–d from the left) |
| `M<hex bytes>.` | raw MIDI bytes, handled as if they came in over USB (`M90 3C 64.`) |
| `U` | reboot into the bootloader, ready to flash |
