// Copyright (c) 2026, Morf
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Standard MIDI File (SMF) -> monophonic step grid converter for AcidMIDI.
//
// This file is plain C++ with no Arduino dependencies, so it can be unit
// tested on a host machine. It must only be called from the main loop (never
// from the ISR), since it allocates scratch memory and may take a while on
// large files.
//
// Conversion rules (TB-303 style):
// * A "part" is a (track, channel) pair that contains at least one note.
//   Format 0 files are split by channel, format 1 files by track + channel.
// * Notes are quantized to a step grid (steps_per_quarter, e.g. 4 = 16ths).
//   The clock input advances one step per pulse, so tempo info is ignored.
// * The grid is monophonic: if two notes land on the same step, the louder
//   one wins (ties go to the higher pitch).
// * A note longer than one step holds its gate over the following steps
//   (TIE), as long as no other note starts there.
// * A note whose note-off comes *after* the next note's note-on (overlap,
//   i.e. legato) is marked SLIDE: the gate stays high and pitch glides into
//   the next note. This is the usual convention for 303 lines in a DAW.
// * Velocity is kept per step, so the accent threshold can be adjusted live.
// * The pattern length is computed from the whole file (all parts), rounded
//   up to whole 4/4 bars, so several parts of one file stay in sync.

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// On Teensy, keep this (non-realtime) code in flash to save fast RAM
#if defined(FLASHMEM)
#define ACIDMIDI_COLD FLASHMEM __attribute__((noinline))
#else
#define ACIDMIDI_COLD
#endif

namespace AcidMidi {

#ifndef ACID_MIDI_MAX_STEPS
static constexpr int MAX_STEPS = 1024; // 64 bars of 16ths
#else
static constexpr int MAX_STEPS = ACID_MIDI_MAX_STEPS;
#endif
static constexpr int MAX_PARTS = 16;

enum StepFlags : uint8_t {
  STEP_GATE  = 1 << 0, // a note starts on this step
  STEP_TIE   = 1 << 1, // the previous note is held through this step
  STEP_SLIDE = 1 << 2, // (on a GATE step) glide into the next note
};

struct Step {
  uint8_t note;  // MIDI note number
  uint8_t vel;   // MIDI velocity 1..127 (0 for rests)
  uint8_t flags; // StepFlags
};

enum Result : int8_t {
  OK = 0,
  ERR_NOT_MIDI = -1,
  ERR_TRUNCATED = -2,
  ERR_SMPTE = -3,      // SMPTE time division is not supported
  ERR_NO_NOTES = -4,
  ERR_NO_PART = -5,    // requested part index doesn't exist
  ERR_NO_MEMORY = -6,
  ERR_NO_FILE = -7,    // file missing or unreadable (used by the applet)
};

struct Song {
  uint16_t length;     // steps in the loop
  uint8_t part_count;  // number of parts found in the file
  Step steps[MAX_STEPS];

  void Clear() {
    length = 0;
    part_count = 0;
    memset(steps, 0, sizeof(steps));
  }
};

// ---------------------------------------------------------------------------
namespace detail {

struct Reader {
  const uint8_t *p;
  const uint8_t *end;
  bool ok = true;

  bool has(size_t n) const { return ok && (size_t)(end - p) >= n; }
  uint8_t u8() {
    if (!has(1)) { ok = false; return 0; }
    return *p++;
  }
  uint16_t u16() { uint16_t v = u8() << 8; return v | u8(); }
  uint32_t u32() { uint32_t v = (uint32_t)u16() << 16; return v | u16(); }
  uint32_t vlq() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      uint8_t b = u8();
      v = (v << 7) | (b & 0x7f);
      if (!(b & 0x80)) return v;
    }
    return v;
  }
  void skip(uint32_t n) {
    if (!has(n)) { ok = false; p = end; return; }
    p += n;
  }
};

struct Track {
  const uint8_t *data;
  uint32_t len;
};

// Callback receives (tick, status, data1, data2) for channel voice messages.
// Returns end tick of the track (time of the last event).
template <typename F>
uint32_t WalkTrack(const Track &t, F &&on_event) {
  Reader r{t.data, t.data + t.len};
  uint32_t tick = 0;
  uint8_t running = 0;
  while (r.has(1)) {
    tick += r.vlq();
    if (!r.ok) break;
    uint8_t status = r.u8();
    if (status < 0x80) { // running status
      if (!running) break;
      --r.p; // the byte is data1
      status = running;
    }

    if (status == 0xff) { // meta
      uint8_t type = r.u8();
      uint32_t len = r.vlq();
      r.skip(len);
      if (type == 0x2f) break; // end of track
      continue;
    }
    if (status == 0xf0 || status == 0xf7) { // sysex
      r.skip(r.vlq());
      continue;
    }
    if (status >= 0xf0) { // other system messages shouldn't appear; bail out
      break;
    }

    running = status;
    const uint8_t hi = status & 0xf0;
    uint8_t d1 = r.u8() & 0x7f;
    uint8_t d2 = 0;
    if (hi != 0xc0 && hi != 0xd0) d2 = r.u8() & 0x7f;
    if (!r.ok) break;
    on_event(tick, status, d1, d2);
  }
  return tick;
}

struct NoteSlot {
  uint32_t start;
  uint32_t end;
};

} // namespace detail

// ---------------------------------------------------------------------------
// Convert one part of an SMF into a step grid.
//   part: 0-based part index
//   steps_per_quarter: grid resolution (2 = 8ths, 3 = 8th triplets,
//                      4 = 16ths, 6 = 16th triplets, 8 = 32nds)
ACIDMIDI_COLD static Result Parse(const uint8_t *data, size_t size, int part,
                    int steps_per_quarter, Song &out) {
  using namespace detail;
  out.Clear();

  Reader r{data, data + size};
  if (!r.has(14) || memcmp(r.p, "MThd", 4) != 0) return ERR_NOT_MIDI;
  r.skip(4);
  uint32_t hlen = r.u32();
  if (hlen < 6) return ERR_NOT_MIDI;
  const uint8_t *after_header = r.p + hlen;
  /* uint16_t format = */ r.u16();
  uint16_t ntracks = r.u16();
  uint16_t division = r.u16();
  if (!r.ok) return ERR_TRUNCATED;
  if (division & 0x8000) return ERR_SMPTE;
  if (division == 0) return ERR_NOT_MIDI;
  if (after_header > r.end) return ERR_TRUNCATED;
  r.p = after_header;

  if (steps_per_quarter < 1) steps_per_quarter = 1;
  const uint32_t tps = division / steps_per_quarter; // ticks per step
  if (tps == 0) return ERR_NOT_MIDI;

  // locate track chunks
  static constexpr int MAX_TRACKS = 64;
  Track tracks[MAX_TRACKS];
  int track_count = 0;
  while (r.has(8) && track_count < ntracks && track_count < MAX_TRACKS) {
    const bool is_track = memcmp(r.p, "MTrk", 4) == 0;
    r.skip(4);
    uint32_t len = r.u32();
    if (!r.has(len)) len = (uint32_t)(r.end - r.p); // tolerate truncation
    if (is_track) tracks[track_count++] = Track{r.p, len};
    r.skip(len);
  }
  if (track_count == 0) return ERR_TRUNCATED;

  // --- Pass 1: enumerate parts and find the end of the song
  uint32_t part_key[MAX_PARTS];
  int part_count = 0;
  uint32_t song_end = 0;   // latest note-off/note-on tick
  uint32_t last_onset = 0; // latest note-on tick
  for (int t = 0; t < track_count; ++t) {
    WalkTrack(tracks[t], [&](uint32_t tick, uint8_t status, uint8_t, uint8_t d2) {
      const uint8_t hi = status & 0xf0;
      if (hi == 0x90 && d2 > 0) {
        const uint32_t key = (uint32_t(t) << 4) | (status & 0x0f);
        bool found = false;
        for (int i = 0; i < part_count; ++i) found = found || part_key[i] == key;
        if (!found && part_count < MAX_PARTS) part_key[part_count++] = key;
        if (tick > last_onset) last_onset = tick;
      }
      if (hi == 0x80 || hi == 0x90) {
        if (tick > song_end) song_end = tick;
      }
    });
  }
  out.part_count = part_count;
  if (part_count == 0) return ERR_NO_NOTES;
  if (part < 0 || part >= part_count) return ERR_NO_PART;

  // song length in steps, rounded up to whole bars of 4/4
  const uint32_t steps_per_bar = 4 * steps_per_quarter;
  // (a note-off hanging less than half a step past a bar line is ignored)
  uint32_t length = (song_end + tps / 2) / tps;
  const uint32_t min_length = last_onset / tps + 1;
  if (length < min_length) length = min_length;
  length = ((length + steps_per_bar - 1) / steps_per_bar) * steps_per_bar;
  if (length > (uint32_t)MAX_STEPS) length = MAX_STEPS;
  out.length = length;

  // --- Pass 2: collect notes of the selected part onto the grid
  NoteSlot *slots = (NoteSlot*)malloc(sizeof(NoteSlot) * length);
  if (!slots) return ERR_NO_MEMORY;

  const uint32_t sel_track = part_key[part] >> 4;
  const uint8_t sel_chan = part_key[part] & 0x0f;

  struct Active { uint32_t start; uint8_t vel; bool on; };
  Active active[128];
  memset(active, 0, sizeof(active));

  auto emit = [&](uint8_t note, uint8_t vel, uint32_t start, uint32_t end) {
    const uint32_t s = (start + tps / 2) / tps;
    if (s >= length) return;
    Step &st = out.steps[s];
    if (st.flags & STEP_GATE) {
      // monophonic: keep the louder (or higher) note
      if (vel < st.vel || (vel == st.vel && note <= st.note)) return;
    }
    st.note = note;
    st.vel = vel;
    st.flags = STEP_GATE;
    slots[s].start = start;
    slots[s].end = end;
  };

  uint32_t track_end = WalkTrack(tracks[sel_track],
    [&](uint32_t tick, uint8_t status, uint8_t d1, uint8_t d2) {
      if ((status & 0x0f) != sel_chan) return;
      const uint8_t hi = status & 0xf0;
      if (hi == 0x90 && d2 > 0) {
        if (active[d1].on) emit(d1, active[d1].vel, active[d1].start, tick);
        active[d1] = Active{tick, d2, true};
      } else if (hi == 0x80 || (hi == 0x90 && d2 == 0)) {
        if (active[d1].on) {
          emit(d1, active[d1].vel, active[d1].start, tick);
          active[d1].on = false;
        }
      }
    });
  // close hanging notes
  for (int n = 0; n < 128; ++n) {
    if (active[n].on) emit(n, active[n].vel, active[n].start, track_end);
  }

  // --- Pass 3: ties and slides
  for (uint32_t s = 0; s < length; ++s) {
    if (!(out.steps[s].flags & STEP_GATE)) continue;

    // find next onset (no wrap-around: the loop end is a phrase boundary)
    uint32_t next = s + 1;
    while (next < length && !(out.steps[next].flags & STEP_GATE)) ++next;

    const uint32_t start_q = s * tps;
    const uint32_t end = slots[s].end;
    // duration in steps, measured from the quantized start
    uint32_t dur = (end > start_q) ? (end - start_q + tps / 2) / tps : 0;
    if (dur < 1) dur = 1;

    // hold over following empty steps
    for (uint32_t k = s + 1; k < s + dur && k < next; ++k) {
      out.steps[k].note = out.steps[s].note;
      out.steps[k].vel = out.steps[s].vel;
      out.steps[k].flags = STEP_TIE;
    }

    // legato overlap into the next note = slide
    if (next < length && end > slots[next].start) {
      out.steps[s].flags |= STEP_SLIDE;
    }
  }

  free(slots);
  return OK;
}

ACIDMIDI_COLD static const char *ResultString(int r) {
  switch (r) {
    case OK:            return "OK";
    case ERR_NOT_MIDI:  return "Not MIDI";
    case ERR_TRUNCATED: return "Truncated";
    case ERR_SMPTE:     return "SMPTE n/a";
    case ERR_NO_NOTES:  return "No notes";
    case ERR_NO_PART:   return "No part";
    case ERR_NO_MEMORY: return "No memory";
    case ERR_NO_FILE:   return "No file";
    default:            return "Error";
  }
}

} // namespace AcidMidi
