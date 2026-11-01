---
layout: default
---
# AcidMIDI

**AcidMIDI** plays a part of a Standard MIDI File (`.mid`) as a monophonic step sequence, locked to your clock, with TB-303 style accents and glides. Every clock pulse advances one step (1/16 by default), so the pattern follows your clock rather than the tempo stored in the file.

Available on Teensy 4.x builds (O_C T40 and ORN8 T41).

## Quick start

1. Load **AcidMIDI** into an applet slot (it's listed under Sequencer and MIDI).
2. Patch a clock into the slot's first trigger input. Each pulse is one 1/16 step, so send 4 pulses per beat (or use the internal clock with a x4 multiplier).
3. Patch output A (pitch) to your VCO's 1V/oct input, and output B (gate) to your envelope or VCA.
4. The built-in demo _Acid1_ starts playing. Turn _File_ to pick another file and _Part_ to pick a track.
5. Optional: patch a reset trigger into the second trigger input to restart the pattern on the downbeat.

### I/O

|        | 1/3 | 2/4 |
| ------ | :-: | :-: |
| TRIG   | Clock (one step per pulse) | Reset (next clock plays step 1) |
| CV INs | Transpose (semitones) | Acid amount (adds % to random accents & glides) |
| OUTs   | Pitch CV (1V/oct, with 303-style glide) | Gate (3V normal, 5V accent), or see _OutB_ |

### Where the accents and glides come from

* **Accents**: notes with velocity at or above the _AccV>_ threshold are accented. _Acc%_ adds extra accents with a seeded probability.
* **Glides**: notes that overlap the next note in the file (legato) glide into it, with the gate held high, just like a 303 slide. _Gld%_ adds extra glides between neighbouring notes with a seeded probability.
* The random extras are deterministic for a given _Seed_, so the loop repeats exactly. Change the seed (or press the AuxButton on _Seed_) for a new variation.
* Notes longer than one step hold the gate over the following steps (ties).
* If notes overlap on the same step, the loudest one wins (the line is monophonic).

### Parameters

| Param | Description |
| ----- | ----------- |
| File  | MIDI file. Built-in demos first, then files from the SD card. AuxButton: reload / rescan the card |
| Part  | Which part to play (a track/channel that has notes), shown as _current/total_ |
| Step  | Grid resolution: 1/8, 1/8T, 1/16, 16T (1/16 triplets), 1/32 |
| Start | First bar of the loop |
| Bars  | Loop length in bars (All, 1, 2, 4, 8, 16, 32) |
| AccV> | Velocity threshold for accents (Off disables velocity accents) |
| Acc%  | Extra random accents, 0 to 100% |
| Gld%  | Extra random glides, 0 to 100% |
| Seed  | Seed for the random extras. AuxButton: new random seed |
| Glide | Glide time constant (0 to 189ms, 60ms default, roughly a 303) |
| Gate  | Gate length as % of the clock period |
| OutB  | Output B mode: _Gt+Ac_ (3V gate, 5V accent), _Gate_ (5V), _Accnt_ (5V gate on accents only), _Velo_ (velocity as 0-5V) |
| Oct   | Octave offset for the pitch output |

The bottom line shows the current step, the playing note, `!` for an accent and a glide icon while sliding. A thin bar along the bottom edge shows the position in the loop.

### UI Controls
* Turn the encoder to move between parameters. The list scrolls, four rows at a time.
* Press the encoder to edit the highlighted parameter, press again to finish.
* AuxButton (the select button while a parameter is highlighted for editing):
  - on _File_ or _Part_: rescan the SD card and reload the file
  - on _Seed_: pick a new random seed

Changing _File_, _Part_ or _Step_ reloads the pattern. Loading happens in the background (outside the realtime loop), so the clock keeps running; the gate stays low for the moment it takes to swap patterns.

### Status messages
Instead of the step counter, the bottom line can show:

| Message | Meaning |
| ------- | ------- |
| Loading   | the file is being read |
| No file   | the selected file isn't there (e.g. a preset points to an SD file but no card is inserted) |
| Not MIDI  | the file isn't a Standard MIDI File |
| Truncated | the file is damaged or incomplete |
| SMPTE n/a | the file uses SMPTE timing; re-export it with PPQN (beats) timing |
| No notes  | the file contains no notes |
| No part   | the selected _Part_ doesn't exist in this file; turn _Part_ down |
| No memory | not enough RAM to load the file |

## How playback works

* **Pitch**: MIDI note numbers are converted the same way as the [MIDI In](MIDI-Input) applet, so C4 (note 60) gives the same voltage there and here. Use _Oct_ and the Transpose CV input to shift it.
* **Gate**: a normal note opens the gate for _Gate_ % of the clock period. A tie holds the gate across steps. A glide holds the gate into the next note without retriggering, and the pitch slides with an exponential curve (like the 303's slide circuit). The glide continues after the gate closes, so short glide times sound tight and long ones sound lazy.
* **Accent**: in _Gt+Ac_ mode the gate goes to 5V instead of 3V. A tied note keeps the accent of the note that started it. If your envelope or VCA has no velocity input, use _Accnt_ mode on a second instance to get a separate accent gate.
* **Velo** mode outputs the note velocity as 0-5V and holds it between notes, useful for filter cutoff or envelope amount.
* **Reset**: the next clock after a reset pulse plays the first step of the loop. Reset and clock can arrive at the same time.
* **Loop**: _Start_ and _Bars_ choose a section of the file. If the section runs past the end of the file, it is shortened.

## Loading MIDI files

### Built-in demo files
* **Acid1**: a 2-bar A minor acid bassline (one part) with accents and slides.
* **Acid3P**: a 4-bar, 3-part demo for multi-voice setups. Part 1 is a bassline, Part 2 a slower lead line with ties, Part 3 an arpeggio.
* **BellaCia**: _Bella Ciao_, 17 bars, the original 9-instrument arrangement (Parts 1-9).
* **Bella3V**: the same song merged into 3 voices: Part 1 melody, Part 2 bass, Part 3 high counter line (try _Oct_ -2 on it). All notes have the same velocity, so use _Acc%_ for accents.

### SD card (ORN8 / Teensy 4.1)
Copy `.mid` files into a folder named `MIDI` at the root of the SD card. They appear after the built-in files, sorted by name. Use the MTP boot slot to copy files over USB, or put the card in a computer. After adding files, press the AuxButton on _File_ to rescan.

### Built-in files
Files can also be compiled into the firmware (handy on O_C without an SD card). From the `software/` folder:

```
python3 res/midi2header.py my_lines/*.mid -o src/applets/AcidMIDIData.h
```

The script lists the parts it finds in each file, so you know which _Part_ number to pick. File names are shortened to 8 characters. Then rebuild and flash the firmware. Built-in files are listed before the SD card files.

SD card files can be up to 64KB each, and up to 64 files are listed.

### Files with many instruments
Every track/channel that has notes is one _Part_ (up to 16), numbered in the order they first appear; `midi2header.py` prints the list. Each AcidMIDI instance plays one Part, so a band arrangement gives you a choice of instruments per voice. Drum tracks work too: use _OutB_ = _Gate_ and ignore the pitch output.

Arrangements often pass a role between instruments, e.g. the melody moves from guitar to koto halfway through, so a single Part goes quiet for half the song. `midi2header.py` can merge Parts into voices when embedding a file:

```
python3 res/midi2header.py "res/acid_midi/Bella_Ciao.mid@1+7,3+4,8+9:Bella3V" ... -o src/applets/AcidMIDIData.h
```

This builds a file whose Part 1 is the original Parts 1+7, Part 2 is 3+4 and Part 3 is 8+9, named _Bella3V_. Merged voices are still monophonic: where two source Parts play at the same time, the louder (then higher) note wins on each step. Do the same for SD card files by merging the tracks in your DAW before exporting.

### Preparing files
* Only PPQN timing is supported (the usual DAW export). Tempo changes are ignored.
* Format 0 files are split into parts by MIDI channel; format 1 files by track and channel.
* The loop length is the length of the whole file, rounded up to whole 4/4 bars, so all parts of one file stay in sync.
* For a slide, let a note overlap the start of the next one (in most DAWs: extend the note end a little past the next note). Notes that only touch are not slides.
* Use velocity for accents (e.g. 127 accented, 90 normal), then set _AccV>_ between the two (e.g. 100).
* Notes are snapped to the nearest step of the _Step_ grid. Pick 16T or 1/8T for triplet material.
* Chords are reduced to one note per step (the loudest, then the highest).
* Up to 1024 steps are used (64 bars of 1/16). Use _Start_ and _Bars_ to pick a section of a longer file.

## Three (or more) voices on ORN8

Each applet has two outputs, so each AcidMIDI instance is one voice: pitch + gate (with the accent folded into the gate level).
In [Quadrants](Quadrants) you can run up to four of them:

1. Load AcidMIDI into slots A, B and C (and D for a fourth voice).
2. Choose the same _File_ in each slot and set _Part_ to 1, 2 and 3.
3. Map each slot's clock input to the same trigger (or the internal clock) and its reset to the same trigger. All parts share the file length, so they stay in sync, and a reset lines them up.
4. Outputs: slot A = outputs A/B, slot B = C/D, slot C = E/F. Slot D (G/H) is free for something else, for example another AcidMIDI on Part 1 with _OutB_ set to _Accnt_ to get a separate accent gate for the bass voice, or a Slew/envelope applet.

Tip: give each voice its own _Seed_ and _Gld%_ so the random glides don't all land on the same steps.

## Troubleshooting
* **Pattern runs too fast or slow**: AcidMIDI moves one step per clock pulse. With the default 1/16 grid you need 4 pulses per beat; use a clock multiplier/divider on the trigger input to adjust.
* **Voices drift apart**: make sure every slot gets the same clock and reset, and that all of them play parts of the same file (different files can have different lengths).
* **My SD files don't show up**: check they're in `/MIDI` (not a subfolder), end in `.mid` or `.midi`, are under 64KB, then press the AuxButton on _File_ to rescan.
* **No glides**: the notes in the file probably don't overlap. Extend the note ends, or raise _Gld%_.
