# Playing PocketDynamo

PocketDynamo is the `pd` firmware (`pio run -e pd`): an 8-voice instrument for
the Cardputer ADV built around old-school synthesis techniques. Every engine
has the same four controls, HARMONICS, TIMBRE, MORPH and MACRO (MACRO 0.5 is
neutral), plus pitch.

## The keyboard

Columns 1–12 of all four rows play notes. The key next to `ctrl` on the bottom
row is the root. The two outer columns are controls:

| Left | | Right | |
|---|---|---|---|
| `` ` `` | **dice**: tap to nudge the controls, hold ½ s to roll new ones | `del` | **strip**: NOTES → CHORD → RATCHET → ARP |
| `tab` | next **surface** | `\` | next **engine** |
| `fn` | function layer | `enter` | all notes off |
| `ctrl` | **tilt** on/off | `space` | **sustain pedal** |

### Surfaces (`tab`)

Most surfaces are isomorphic: one column right is one scale step, and one row
up is a fourth. A chord shape is the same anywhere on the grid.

| Surface | What it is |
|---|---|
| MAJOR / MINOR / DORIAN 4THS | scale-locked: every key is in the scale |
| PENTA 4THS | minor pentatonic, two steps per row |
| CHROMA 4THS | chromatic, rows a fourth apart, like a bass |
| PIANO | the tracker layout: `z s x d c v…` lower octave, `q 2 w 3 e…` upper |
| 49 SUPERPYTH | a major scale built from 49-EDO's sharp fifth: wide whole tones, 49-cent half steps, a major third near 9/7 |
| 49 7x7 | 7 equal steps per octave across a row; each row up is one 49th of an octave sharper |
| 49 CHROMA | every 49-EDO step in order, one octave across the keyboard |

### Strips (`del`)

In CHORD, RATCHET or ARP the top row's 12 keys stop playing notes and become
hold keys. Hold one, then play:

- **CHORD:** the key plays that chord. On scale surfaces chords are built from
  scale degrees, so they stay in key. On chromatic surfaces they're the usual
  shapes snapped to the tuning; in 49-EDO each comes out bent its own way.
- **RATCHET:** held notes re-strike at that rate, from ¼ down to 1/64, with
  dotted and triplet values, synced to the tempo.
- **ARP:** held and sustained notes arpeggiate.
  - Slots 1–6 (up, down, up-down, as played, random, motif) step in 16ths.
  - Slots 7–12 are the same patterns as **chip arps**: one voice re-pitched at
    50 Hz, the way Follin's drivers stepped once per frame.
  - MOTIF takes both its rhythm and its notes from Lenticular's three-distance
    gap statistics.

**Double-tap** a strip key to latch it. Tap it again to let go. A latched slot
shows green.

### Tilt (`ctrl`)

While tilt is on, the device's angle sets two controls directly:
- **TIMBRE (side to side):** 0 on its left edge, 1 on its right.
- **MORPH (front/back):** 0 flat, 0.8 standing up, 1 tipped past upright.

Their bars turn green. Switching tilt off returns them to their set values.

### FN layer

| FN + | |
|---|---|
| `z`/`x` `c`/`v` `b`/`n` `m`/`,` | HARMONICS, TIMBRE, MORPH, MACRO down/up. Hold to sweep |
| `[` / `]` | volume, 0 to +12 dB |
| `` ` `` / `1` | octave down / up |
| `q` / `w` | root down / up one step of the current tuning |
| `9` / `0` | tempo |
| `tab` | previous surface |
| `\` | previous engine |
| `del` | undo the last dice (again to redo) |
| `r` (hold) | **record** from the mic into the take, up to 3.3 s |
| `l` | **looper**: record → play → overdub → play… |
| `k` | looper stop, then clear |
| `2`…`8` | load patch 2…8 from the SD card |
| `shift` + `2`…`8` | save patch 2…8 |

## Engines (`\`)

| Engine | HARMONICS | TIMBRE | MORPH | MACRO |
|---|---|---|---|---|
| **ZX PULSE**: Tim Follin-style pin trains | chord (Plaits' set) | pin width, which is also the level | builds the stack (lower half) / arp, its rate continuous (upper half) | exaggerated pitch pull (CCW) / echo (CW) |
| **ZX PHASE**: Phaser1-style 1-bit interference | mix method (XOR / OR / AND), plus a 1-bit flanger within each zone | PWM sweep on osc 2 | unison beating → interval ladder → LFSR noise | kick-slide drum (CCW) / ghost carrier (CW) |
| **BROCOT FM**: parallel true FM on Stern-Brocot ratios | trunk: harmonic → Avatar → Golden convergents | FM index (key-scaled) | DX feedback ← sine → Fritz double pulse | strike (CW) / swell (CCW) |
| **SAMPLE**: the take as a varispeed record | slice / start | length | forward, loop, ping-pong, reverse | tape-dull (CCW) / SP grit (CW) |
| **CLOUD**: the take as a granular cloud | position | grain size | timing: CHIP, SILK, MOTIF, LOOSE, CLUMP | density |
| **BREAK**: the take as a one-bar break, cut up live by Audiothurgist's chop grammar at the tempo | chop: verbatim → DJ edit → chops → rolls → micro bursts | pattern (32 seeds) | gate length | tape-dull / SP grit |

**The take:** FN+R records from the mic at 24 kHz (SP-1200 territory). It is
trimmed, normalized and sliced at onsets. Recording switches the engine to
SAMPLE if you weren't on SAMPLE, CLOUD or BREAK. C4 plays the take as
recorded. BREAK plays it at the tempo, so tempo is pitch (the turntable way),
and the key adds its own pitch on top. Every re-strike restarts the bar, so
RATCHET stutters the break.

## The looper

The looper records the events sent to the synth, not audio. That covers
notes, chords, arp and ratchet output, control sweeps and engine changes.

- The first pass snaps to whole beats at the current tempo.
- Overdubs join at the next lap.
- Looped notes never steal the gate of a note you're playing live.

## Patches

Patches live in `/pocketdynamo/pN.txt` on the SD card as plain `key=value`
text, so you can edit them by hand. The take goes alongside as `pN.wav`.
- Any mono 16-bit WAV dropped there loads, resampled to 24 kHz.
- Engines and surfaces are stored by name.

## Serial (115200, for scripts)

| Command | |
|---|---|
| `i` | state |
| `B` | silent CPU bench of every engine with 8 voices |
| `S` | SD card status |
| `[` / `]` | volume |
| `K<row><col-hex><+/->` | press or release a key |
