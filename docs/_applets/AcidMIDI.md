---
layout: default
---
# AcidMIDI

**AcidMIDI** plays a part of a Standard MIDI File (`.mid`) as a monophonic step sequence, locked to your clock, with TB-303 style accents and glides. Every clock pulse advances one step (1/16 by default), so the pattern follows your clock rather than the tempo stored in the file.

Available on Teensy 4.x builds (O_C T40 and ORN8 T41).

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

The bottom line shows the current step, the playing note, `!` for an accent and a glide icon while sliding.

## Loading MIDI files

### SD card (ORN8 / Teensy 4.1)
Copy `.mid` files into a folder named `MIDI` at the root of the SD card. They appear after the built-in files, sorted by name. Use the MTP boot slot to copy files over USB, or put the card in a computer. After adding files, press the AuxButton on _File_ to rescan.

### Built-in files
Files can also be compiled into the firmware (handy on O_C without an SD card). From the `software/` folder:

```
python3 res/midi2header.py my_lines/*.mid -o src/applets/AcidMIDIData.h
```

The script lists the parts it finds in each file, so you know which _Part_ number to pick. File names are shortened to 8 characters.

### Preparing files
* Only PPQN timing is supported (the usual DAW export). Tempo changes are ignored.
* Format 0 files are split into parts by MIDI channel; format 1 files by track and channel.
* The loop length is the length of the whole file, rounded up to whole 4/4 bars, so all parts of one file stay in sync.
* For a slide, let a note overlap the start of the next one. Use velocity for accents (e.g. 127 accented, 90 normal).
* Up to 1024 steps are used (64 bars of 1/16). Use _Start_ and _Bars_ to pick a section of a longer file.

## Three (or more) voices on ORN8

Each applet has two outputs, so each AcidMIDI instance is one voice: pitch + gate (with the accent folded into the gate level).
In [Quadrants](Quadrants) you can run up to four of them:

1. Load AcidMIDI into slots A, B and C (and D for a fourth voice).
2. Choose the same _File_ in each slot and set _Part_ to 1, 2 and 3.
3. Map each slot's clock input to the same trigger (or the internal clock) and its reset to the same trigger. All parts share the file length, so they stay in sync, and a reset lines them up.
4. Outputs: slot A = outputs A/B, slot B = C/D, slot C = E/F. Slot D (G/H) is free for something else, for example another AcidMIDI on Part 1 with _OutB_ set to _Accnt_ to get a separate accent gate for the bass voice, or a Slew/envelope applet.
