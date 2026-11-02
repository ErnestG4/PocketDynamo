# PocketDynamo manual

PocketDynamo is an 8-voice synthesizer for the M5Stack Cardputer ADV. It has
seven engines, effects, a looper and MIDI in over USB and Bluetooth.

## Quick start

- **Play:** columns 1–12 of the keyboard are notes. The bottom-left note key
  (`opt`) is the root, C3 to start.
- **Change the sound:** `\` steps through the engines. Hold `fn` and use
  `z`/`x`, `c`/`v`, `b`/`n` and `m`/`,` to turn the four controls down and up.
- **Roll the dice:** tap `` ` `` to nudge the sound, or hold it for a new one.
- **Add an echo:** FN+`e`.
- **Hold a chord:** press `del` to reach the CHORD strip, hold a number-row
  key, then play.
- **Tilt it:** tap `ctrl`, then tip the Cardputer.

It starts on ZX PULSE, the MAJOR 4THS layout, 120 bpm, at 0 dB. That's
quiet on the speaker and comfortable in headphones: turn it up with FN+`]`.

## The keyboard

The outer columns are controls:

| Left | | Right | |
|---|---|---|---|
| `` ` `` | **dice**: tap to nudge the sound, hold ½ s for a new one | `del` | **strip**: NOTES → CHORD → RATCHET → ARP |
| `tab` | next **layout** | `\` | next **engine** |
| `fn` | the function layer (see the end of this manual) | `enter` | all notes off |
| `ctrl` | **tilt**: tap for on/off, hold + a control key to route it | `space` | **sustain pedal** |

The keys play at a fixed velocity. Letting go of a key starts its engine's
release: short for the buzzers and stabs, about 3 s for MODAL, so bells ring.
To shape notes yourself, switch on the ADSR (see [The ENV page](#the-env-page-fnp-twice)).

### Layouts (`tab`, FN+`tab` back)

Most layouts are isomorphic: one column right is one scale step, and one row
up is a fourth. A shape you learn plays the same anywhere on the grid. The
last three leave the usual tuning behind for pure ratios.

| Layout | |
|---|---|
| MAJOR / MINOR / DORIAN 4THS | Scale-locked: every key is in the scale. |
| PENTA 4THS | Minor pentatonic. |
| CHROMA 4THS | Chromatic, rows a fourth apart, like a bass. |
| PIANO | The tracker layout: `z s x d c v…` is the lower octave, `q 2 w 3 e…` the upper. While a strip is on, the number row's sharps become strip keys. |
| 49 SUPERPYTH | A major scale in 49 equal steps per octave: wide whole tones, narrow half steps, a major third near 9/7. |
| 49 7x7 | 7 equal steps across a row, each row one 49th of an octave sharper, so shapes across rows shimmer. |
| 49 CHROMA | Every 49-step pitch in order, about an octave across the keyboard. |
| 5THS PYTHAG | Pure 3:2 fifths: each key a fifth from its neighbour, so any cluster is open fifths and fourths. A row holds the 12 notes of the fifths chain; each row up continues it an octave higher, a little sharper (a Pythagorean comma), so a shape on two rows shimmers. |
| STERN-BROCOT | Pure ratios of the root from the Stern-Brocot tree. The bottom row is the simplest twelve, a just scale: 1/1 6/5 5/4 9/7 4/3 7/5 10/7 3/2 8/5 5/3 7/4 9/5. Each row up is the next twelve, a level deeper and stranger, in the same octave. The screen shows each key's ratio. |
| IMPRESSION | The first key you press is the root. While it's held, every other key plays what its place suggests (see below). |

**IMPRESSION.** Hold a root, then reach around it. Keys in pure intervals of
the root light up in each direction:
- **Right** brightens it: a major third, a major seventh, a ninth.
- **Left** darkens it: a minor third, a soft seventh, an eleventh.
- **Up** opens it: a fifth, an octave, a twelfth.
- **Down** goes below it.
- **The diagonals** play two-note shapes: major colours up and right, minor
  up and left, suspended down and right, and soft septimal ones down and left.

Each direction has three steps; keys farther out wrap around to the same
colours an octave higher, so whatever you reach for sounds right with the
root. Let go of every key and the next one you press is a new root. CHORD
works on the root key.

### Strips (`del`)

In CHORD, RATCHET or ARP, the number row's 12 keys stop playing notes and hold
a setting instead. Their labels show on screen. Hold one, then play.
**Double-tap** a strip key to latch it, and tap it again to let it go. A held
key shows cyan, a latched one green.

- **CHORD:** each key plays a chord.
  - On scale layouts, chords stack scale degrees, so they stay in key: 5, TRI,
    S2, S4, 6, 7, 9, AD9, 4TH, OPN, OCT, CLS.
  - On chromatic layouts they're the familiar shapes: 5, M, m, S2, S4, M7, m7,
    7, m9, M9, 69, and 7:4, a natural-seventh chord.
  - On the 49-step layouts each chord comes out bent its own way.
- **RATCHET:** held notes re-strike at a rate synced to the tempo: 4, 8., 4t,
  8, 16., 8t, 16, 32., 16t, 32, 32t, 64.
- **ARP:** held and sustained notes play one at a time.
  - Keys 1–6 (UP, DN, UD, ORD in the order you played them, RND, MTF) step in
    16ths at the tempo.
  - Keys 7–12 are the same patterns as **chip arps**: one voice re-pitched 50
    times a second, the way 8-bit music drivers did it.
  - MOTIF (MTF) takes its rhythm and its notes from a golden-ratio sequence,
    so the rhythm is the melody.

### Sustain (`space`)

Notes you let go of while `space` is down keep sounding until you release it.
Playing a sustained key again re-strikes it. Sustained notes stay in the
ARP's pool.

### Dice (`` ` ``)

- **Tap:** mutate. Each control has even odds of a small nudge, and at least
  one always moves.
- **Hold ½ s:** roll a fresh sound.
- **FN+`del`:** undo the last dice. Press it again to redo.

## The engines (`\`, FN+`\` back)

Every engine has the same four controls: **HARMONICS, TIMBRE, MORPH and
MACRO**. MACRO's middle is neutral; it does one thing below the middle and
another above.

| Engine | HARMONICS | TIMBRE | MORPH | MACRO (below / above the middle) |
|---|---|---|---|---|
| **ZX PULSE**: ZX Spectrum beeper pulse trains | which chord | pulse width, which is also the level | builds the chord up (lower half), then arpeggiates it faster and faster (upper half) | an exaggerated pitch drop / an echo |
| **ZX PHASE**: two 1-bit oscillators interfering | how they combine (XOR / OR / AND), with a 1-bit flanger inside each that steps up the harmonic series | pulse-width sweep on the second oscillator | unison beating → just intervals (4/3, 3/2, 2, 3, 4) → noise | a kick-drum pitch slide / a supersonic ghost tone |
| **BROCOT FM**: FM on ratios from the Stern-Brocot tree | the ratio family, from simple harmonics to dense golden-ratio sets (sweep it to hear it zip between them) | FM depth, tamed on high notes | sawlike feedback ← pure sine (middle) → hollow double pulse | a brass-like swell / a bell strike |
| **MODAL**: struck resonators | the object: string, stiff string, marimba, glockenspiel, kalimba, electric-piano tine, drum, bell, gong | the mallet, felt → metal | the decay, 50 ms → 20 s | a metallic noise wash / an electric-piano bark |
| **SAMPLE**: the take as a varispeed tape | which slice (or start point) | length, 20 ms → the rest of the take | forward, loop, ping-pong, reverse | dull tape / sampler grit |
| **CLOUD**: the take as a cloud of grains | where in the take | grain size, 15 → 400 ms | timing: CHIP, SILK, MOTIF, LOOSE, CLUMP | density, 2 → 120 grains a second |
| **BREAK**: the take as a one-bar break, cut up live | how much it's cut: as recorded → edits → chops → rolls → bursts | which of 32 patterns | gate length | dull tape / sampler grit |

## Recording a take (FN+`r`)

Hold FN+`r` to record from the mic, for up to 2.5 s. Sound stops while you
record, so the speaker isn't in the take.

- The take is trimmed, normalized and sliced where its sounds start. The
  bottom line shows its length and slice count.
- Recording switches to SAMPLE, unless you're already on SAMPLE, CLOUD or
  BREAK.
- **SAMPLE and CLOUD:** C4 plays the take at its recorded pitch.
- **BREAK:** the take becomes one bar on a 16th grid at the tempo. Tempo is
  pitch, the way a turntable works, and the key adds its own pitch on top.
  Every re-strike restarts the bar, so RATCHET stutters the break.
- **CLOUD in CHIP or MOTIF:** grains play chord tones (root, fifth, octave,
  octave and fifth).

The take is stored at 24 kHz and 12 bits, for a bit of vintage-sampler grit.

## Effects

### The echo (FN+`e`)

A tape echo on everything you play, synced to the tempo.

- **FN+`e`** steps the time: 1/8 → dotted 1/8 → 1/4 → off. The tape is 1.5 s
  long, so a 1/4 fits down to 40 bpm; below that, a time that won't fit
  halves until it does.
- **FN+`d`/`f`:** feedback, which is how long the repeats go on.
- **FN+`s`/`g`:** mix, which is how loud they are.

It behaves like tape: repeats get darker and grittier as they come round, a
change of tempo bends their pitch as the head glides, and switching it off
lets the tail ring out.

### The FX page (FN+`p`)

FN+`p` once, from the SOUND page, turns the four bars into four effects. Two
more pages follow, ENV and LFO (see [Envelope and LFO](#envelope-and-lfo)).
On the FX page FN+`z`/`x`, `c`/`v`, `b`/`n` and `m`/`,` set the effect amounts.
Every effect is off at 0. The FX bars are cyan, so you can tell which page
you're on.

| Bar | Pick with | Effects | What turning it up does |
|---|---|---|---|
| **TONE** | FN+`t` | LP / BP / HP filter | LP closes from fully open, HP thins from the bottom, BP sweeps its peak from 100 Hz to 8 kHz. |
| **GRIT** | FN+`y` | CRUSH / PWM / DRIVE | CRUSH: fewer bits and a lower sample rate. PWM: the sound turned into a stream of 1-bit pulses, the way the Spectrum's beeper played samples, its carrier dropping as you turn up. DRIVE: saturation. |
| **SHAPE** | FN+`u` | FOLD / RING / TREM | FOLD: a wavefolder. RING: ring modulation, 30 Hz–2 kHz. TREM: tremolo in 8th notes at the tempo. |
| **SPACE** | | reverb | With the echo on, it smears each repeat into a wash. With the echo off, it turns the tape into a small room. |

The effects run in the order SHAPE → GRIT → TONE → echo. While any of them is
on, a limiter at the end of the chain keeps peaks clean.

## Envelope and LFO

FN+`p` steps through four pages: SOUND → FX → ENV → LFO → SOUND. Each one
puts its own four bars on FN+`z`/`x`, `c`/`v`, `b`/`n` and `m`/`,`.

### The ENV page (FN+`p` twice)

An ADSR for the volume of each note, instead of the engine's own envelope.

| Bar | Keys | Range |
|---|---|---|
| **ATTACK** | FN+`z`/`x` | 2 ms – 5 s |
| **DECAY** | FN+`c`/`v` | 5 ms – 10 s, down to the sustain level |
| **SUSTAIN** | FN+`b`/`n` | the level while you hold the key, 0–100% |
| **RELEASE** | FN+`m`/`,` | 5 ms – 10 s after you let go |

- **FN+`a`** switches it on and off. Moving a bar switches it on.
- **It starts as** a 10 ms attack, a 330 ms decay to 50% and a 700 ms
  release: a pluck that settles and rings on, so you hear it the moment it's
  on.
- **Off,** each engine keeps its own envelope, as before. The bars turn grey.
- **The switch is heard at once:** held notes take the ADSR, or go back to
  their engine's envelope, as you press FN+`a`. Notes you've already let go
  of finish as they started. Moving a bar reshapes the notes it's playing.
- **Sustain at 0** makes a pluck that dies away while you hold the key.

### The LFO page (FN+`p` three times)

One LFO for the whole instrument, which can wobble any control or the pitch.

| Bar | Keys | What it does |
|---|---|---|
| **RATE** | FN+`z`/`x` | 0.05–20 Hz, or synced: 4 bars down to 1/32, with triplets |
| **DEPTH** | FN+`c`/`v` | how far it swings. At 0 it does nothing |
| **SHAPE** | FN+`b`/`n` | SINE, TRI, SAW, SQR (square), S&H (a random step each cycle) |
| **DELAY** | FN+`m`/`,` | 0–5 s: how long it takes to fade in after you start playing from silence |

- **Routing:** on this page, hold `ctrl` and tap a key to choose what the LFO
  drives. Tilt keeps its routing.

  | Hold `ctrl` + | the LFO drives |
  |---|---|
  | `z` or `x`, `c` or `v`, `b` or `n`, `m` or `,` | HARMONICS, TIMBRE, MORPH, MACRO |
  | `t`, `y`, `u`, `i` | TONE, GRIT, SHAPE, SPACE |
  | `d` or `f`, `s` or `g` | echo feedback, echo mix |
  | `q` or `w` | pitch (vibrato) |

  The same key again routes it nowhere. The bottom line shows where it goes,
  and the control's label on the SOUND or FX page turns magenta.
- **FN+`o`:** RATE free-running in Hz, or synced to the tempo.
- **It swings around the control's set value,** so the bar still sets where
  the wobble is centred. On pitch, low depths give a gentle vibrato and full
  depth swings an octave.

## Tilt (`ctrl`)

Tap `ctrl` to switch tilt on or off. While it's on, the Cardputer's angle
sets two things directly:

- **Side to side:** 0 on its left edge, 1 on its right. TIMBRE to start.
- **Front/back:** 0 flat, 0.8 standing up, 1 tipped past upright. MORPH to
  start.

**Choosing what tilt drives:** hold `ctrl` and tap a control's down or up key.
The down key puts that control on side-to-side, the up key on front/back.

| Hold `ctrl` + | side to side | front/back |
|---|---|---|
| HARMONICS (on the FX page: TONE) | `z` | `x` |
| TIMBRE (GRIT) | `c` | `v` |
| MORPH (SHAPE) | `b` | `n` |
| MACRO (SPACE) | `m` | `,` |
| echo feedback | `d` | `f` |
| echo mix | `s` | `g` |

- Routing something switches tilt on.
- On the LFO page, `ctrl` plus a key routes the LFO instead. Go to another
  page to route tilt.
- Anything already on the other axis swaps over.
- Tilted bars turn green. The echo label turns cyan while tilt drives the echo.
- Switching tilt off returns everything to its set value.

Try echo mix on one axis and feedback on the other, then lean into it.

## The looper (FN+`l`, FN+`k`)

The looper records what you play, not the audio: notes, chords, the arp and
ratchet, control moves and engine changes.

- **FN+`l`:** record → play → overdub → play…
- **FN+`k`:** stop. Press it again to clear.
- The first pass snaps to whole beats at the tempo.
- Overdubs join from the next time round.
- Looped notes never cut off a note you're playing live.
- The bottom line shows the looper's state, the loop length and a progress
  bar.

## Patches (FN+`2`…`8`)

FN+`2`…`8` loads a patch from the SD card, and FN+`shift`+`2`…`8` saves one.

- **What a patch holds:** the engine, layout, root, octave and tempo, the four
  controls, the echo, the FX page and the tilt routing, plus the take.
- **What it doesn't:** the strip, whether tilt is on, or the volume.
- Patches are plain text in `/pocketdynamo/pN.txt`, so you can edit them by
  hand. The take goes alongside as `pN.wav`.
- Any mono 16-bit WAV saved as `pN.wav` next to a patch loads, up to 2.5 s.

## MIDI (USB and Bluetooth)

PocketDynamo listens for MIDI over USB and Bluetooth at once, on every
channel.

- **USB:** plug it into a computer, phone or tablet. It shows up as a MIDI
  device called **PocketDynamo**.
- **Bluetooth LE:** it advertises as **PocketDynamo** whenever it's on.
  Connect from a Bluetooth MIDI app, or on a Mac from Audio MIDI Setup → MIDI
  Studio → Bluetooth. **BT** in the top right is grey while it waits and cyan
  once connected.

What it responds to:

- **Notes** play as if they were keys, so they take the strip you're
  holding: CHORD, RATCHET, ARP, sustain and the looper all work on them. A
  note that's on the current layout plays exactly like that key. A note off
  it plays its own pitch with the chord's plain shape.
- **Velocity** sets the level.
- **CC 64** is sustain, and **CC 120/123** is all notes off.
- **Every bar has a CC,** so you can play from a keyboard without
  touching the Cardputer:

  | CC | |
  |---|---|
  | 16–19 | HARMONICS, TIMBRE, MORPH, MACRO (also 71 HARMONICS, 74 TIMBRE, 2 breath MACRO) |
  | 20–23 | TONE, GRIT, SHAPE, SPACE |
  | 24–27 | ATTACK, DECAY, SUSTAIN, RELEASE (also 73 attack, 75 decay, 72 release). Moving one switches ENV on |
  | 28–31 | LFO RATE, DEPTH, SHAPE, DELAY (also 76 rate, 77 depth, 78 delay, and 1, the mod wheel, for depth) |
  | 7 | volume: −47.5 dB to +12 dB, and 0 mutes |
  | 12 | echo feedback |
  | 13 | echo time: off, 1/8, dotted 1/8, 1/4 in four equal steps |
  | 91 | echo mix |

  The mod wheel used to drive MORPH. It's on CC 18 now.
- **Page knobs:** four knobs can reach every bar. CC 102–105 move the four
  bars of whichever page is on screen. CC 106 picks the page (its range in
  four equal parts: SOUND, FX, ENV, LFO), and CC 107 at 64 or above steps to
  the next page, for a button.
  - **Pickup:** a page knob only takes over a bar once it reaches the bar's
    value, by starting within a step of it or turning past it. So changing
    page, or nudging the bar from the keyboard, never makes the first turn
    jump. Until then the bottom line shows where the bar is, for example
    `TIMBRE 75%: TURN TO IT`.
  - This works with knobs that are encoders, as long as they send a value
    (0–127) rather than up/down steps.
- **Pitch bend** covers ±2 semitones.
- **Program change** 0–6 picks the engine.
- **MIDI clock** sets the tempo, so the arp, ratchets, BREAK, the echo and the
  looper follow your DAW. Over Bluetooth, clock keeps steady time despite the
  radio's jitter.

`MIDI` lights in the top right while messages arrive.

## The screen

- **Top:** the engine, the layout, root and octave, tempo, volume, and the
  `MIDI` and `BT` lights.
- **Status line:** the strip, `SUS` and `TILT` (lit when on), the take (or REC
  with a timer), the echo time, and CPU load.
- **Bars:** the four controls, or the FX, ENV or LFO page. Green means tilt
  is driving a bar, and a magenta label means the LFO is.
- **Strip row:** the 12 strip keys, while a strip is on.
- **Bottom line:** messages (MUTATE, SAVED 3, TAKE 1.2s 6 SLICES…), the
  looper, or a key hint.

## The function layer (hold `fn`)

| FN + | |
|---|---|
| `z`/`x` `c`/`v` `b`/`n` `m`/`,` | the four controls (or FX amounts) down/up. Hold to sweep |
| `p` | the next page: SOUND → FX → ENV → LFO |
| `a` | ENV (the ADSR) on/off |
| `o` | the LFO's RATE free or synced to the tempo |
| `t` / `y` / `u` | pick the TONE / GRIT / SHAPE effect |
| `e` | echo time: 1/8 → 1/8. → 1/4 → off |
| `d` / `f` | echo feedback down / up |
| `s` / `g` | echo mix down / up |
| `` ` `` / `1` | octave down / up |
| `q` / `w` | root down / up one step (a semitone, or a 49th of an octave on the 49 layouts) |
| `9` / `0` | tempo down / up, 40–300 bpm |
| `[` / `]` | volume down / up in 3 dB steps, from 0 dB at power-on up to +12 dB |
| `tab` / `\` | previous layout / previous engine |
| `del` | undo / redo the last dice |
| `r` (hold) | record a take |
| `l` / `k` | looper: record/play/overdub, and stop/clear |
| `2`…`8` | load a patch |
| `shift` + `2`…`8` | save a patch |

Holding a key that adjusts something repeats it.

## Serial (115200 baud, for scripts)

| Command | |
|---|---|
| `i` | print the state line (engine, voices, CPU load and peak, dropouts as `xrun`, the page, `env` on/off, `lfo` destination/depth, heap, the least free stack of the audio and control tasks as `stack`, NaN catches as `faults`, uptime…) |
| `B` | a silent CPU bench: each engine plays 8 held notes on the live instrument for a second, with the effects off, and reports its load, its peak (including the strikes) and any dropouts |
| `S` | SD card status |
| `[` / `]` | volume |
| `K<row><col-hex><+/->` | press or release a key (row 0–3 from the top, column 0–d from the left) |
| `M<hex bytes>.` | raw MIDI bytes, handled as if they came in over USB (`M90 3C 64.`) |
| `U` | reboot into the bootloader, ready to flash |
| `L` | the licence notice, the version and commit it was built from, and the full licence text |
