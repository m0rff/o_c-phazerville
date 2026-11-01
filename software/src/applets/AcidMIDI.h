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

// AcidMIDI - clock-synced MIDI file player with TB-303 style accents & glides
//
// Plays one part (track/channel) of a Standard MIDI File as a monophonic step
// sequence. Every clock pulse advances one step (1/16 by default), so the
// pattern follows your clock instead of the tempo stored in the file.
//
//   Out A: pitch CV (1V/oct) with exponential 303-style glide
//   Out B: gate (3V normal, 5V accent) / plain gate / accent gate / velocity
//
// Accents come from note velocity (>= threshold) and can be sprinkled on top
// with a seeded, repeatable probability. Glides come from legato (overlapping)
// notes in the file, plus an optional seeded probability.
//
// Files: a few demo patterns are compiled in (see res/midi2header.py to embed
// your own). On Teensy 4.1 / ORN8, .mid files in the /MIDI folder of the SD
// card show up after the built-in ones.
//
// For several voices from one file, load AcidMIDI into several applet slots
// (e.g. 3 slots of Quadrants on ORN8), choose the same file and Part 1/2/3,
// and clock them from the same trigger. All parts share the file's length, so
// they stay in sync; the Reset input realigns them.

#include "../AcidMidiFile.h"
#include "AcidMIDIData.h"
#ifdef ARDUINO_TEENSY41
#include <SD.h>
#endif

namespace AcidMidiShared {
  // Scratch buffer for loading, only used from the main loop
  static AcidMidi::Song *scratch = nullptr;
  static constexpr size_t MAX_FILE_SIZE = 64 * 1024;

#ifdef ARDUINO_TEENSY41
  static constexpr int MAX_SD_FILES = 64;
  static constexpr int NAME_LEN = 40;
  DMAMEM static char sd_names[MAX_SD_FILES][NAME_LEN];
  static int sd_count = 0;
  static bool sd_scanned = false;

  static bool IsMidiFile(const char *name) {
    if (name[0] == '.') return false; // skip hidden files (e.g. macOS "._" files)
    const char *ext = strrchr(name, '.');
    return ext && (strcasecmp(ext, ".mid") == 0 || strcasecmp(ext, ".midi") == 0);
  }

  // Scan /MIDI for .mid files, sorted by name so indexes stay stable
  FLASHMEM static void ScanSD() {
    sd_scanned = true;
    sd_count = 0;
    if (!SDcard_Ready) return;
    File dir = SD.open("/MIDI");
    if (!dir || !dir.isDirectory()) return;
    while (sd_count < MAX_SD_FILES) {
      File f = dir.openNextFile();
      if (!f) break;
      const char *name = f.name();
      if (!f.isDirectory() && IsMidiFile(name) && strlen(name) < NAME_LEN) {
        // insertion sort
        int i = sd_count++;
        while (i > 0 && strcasecmp(sd_names[i - 1], name) > 0) {
          strcpy(sd_names[i], sd_names[i - 1]);
          --i;
        }
        strcpy(sd_names[i], name);
      }
      f.close();
    }
    dir.close();
  }
#endif

  static int FileCount() {
#ifdef ARDUINO_TEENSY41
    return AcidMidiData::file_count + sd_count;
#else
    return AcidMidiData::file_count;
#endif
  }
}

class AcidMIDI : public HemisphereApplet {
  APPLET_INTERFACE(AcidMIDI, "AcidMIDI", BEND_ICON);

  void OnButtonPress() override;
  void AuxButton() override;
  void Reset() override {
    // may be called from the ISR
    reset_pending = true;
  }

  enum AcidMIDICursor {
    FILE_SEL, PART, RESOLUTION, START_BAR, LOOP_BARS,
    ACC_THRESH, ACC_PROB, SLIDE_PROB, SEED,
    GLIDE, GATE_LEN, OUT_MODE, OCTAVE,

    CURSOR_LAST = OCTAVE
  };

  enum OutBMode : uint8_t {
    OUTB_GATE_ACCENT, // 3V gate, 5V on accents
    OUTB_GATE,        // plain 5V gate
    OUTB_ACCENT,      // 5V gate on accented notes only
    OUTB_VELOCITY,    // velocity as 0-5V CV (held between notes)
    OUTB_MODE_COUNT
  };

private:
  static constexpr int VISIBLE_ROWS = 4;
  static constexpr uint8_t SPQ_TABLE[] = {2, 3, 4, 6, 8};
  static constexpr const char *RES_NAMES[] = {"1/8", "1/8T", "1/16", "16T", "1/32"};
  static constexpr uint8_t LOOP_TABLE[] = {0, 1, 2, 4, 8, 16, 32};
  static constexpr const char *OUTB_NAMES[] = {"Gt+Ac", "Gate", "Accnt", "Velo"};
  static constexpr int GATE_NORMAL_CV = HEMISPHERE_3V_CV;
  static constexpr int GATE_ACCENT_CV = 5 * ONE_OCTAVE;

  // --- settings (saved)
  int cursor = 0;
  uint8_t file_idx = 0;
  uint8_t part = 0;          // 0-based
  uint8_t res_idx = 2;       // 1/16
  uint8_t start_bar = 0;     // 0-based
  uint8_t loop_idx = 0;      // 0 = whole file
  uint8_t acc_thresh = 100;  // velocity >= this is accented, 0 = off
  uint8_t acc_prob = 0;      // extra random accents, 5% units (0-20)
  uint8_t slide_prob = 0;    // extra random glides, 5% units (0-20)
  uint8_t seed = 0;
  uint8_t glide = 20;        // glide time constant, 3ms units
  uint8_t gate_len = 8;      // gate length 10% + 5% units
  uint8_t outb_mode = OUTB_GATE_ACCENT;
  int8_t octave = 0;

  // --- loaded pattern
  AcidMidi::Song song;
  uint8_t song_spq = 4;
  volatile bool loading = false;
  volatile bool load_pending = true;
  volatile bool rescan_pending = false;
  volatile bool service_queued = false;
  int8_t load_result = AcidMidi::OK;
  char file_name[9] = "";

  // --- playback state (ISR)
  volatile bool reset_pending = true;
  int pos = 0;           // position within the loop
  int play_step = 0;     // absolute step index in the song
  int loop_len = 0;
  bool gate_high = false;
  bool slide_armed = false;   // the next onset glides instead of retriggering
  bool onset_slides = false;  // the current note glides into the next one
  bool cur_accent = false;
  bool gliding = false;
  uint8_t cur_vel = 0;
  uint8_t cur_note = 0;
  uint32_t gate_off_tick = 0;
  uint32_t retrig_tick = 0;
  int32_t pitch_q = 0;   // current pitch, 16.16 fixed point
  int32_t target_q = 0;
  int32_t glide_coeff = 65536;

  void UpdateGlideCoeff() {
    // one-pole lag per ISR tick: coeff = 1 - exp(-1 / (tau * fs))
    const float tau = glide * 0.003f;
    if (tau <= 0.0f) {
      glide_coeff = 65536;
      return;
    }
    const float fs = 1000000.0f / OC_CORE_TIMER_RATE;
    glide_coeff = constrain(int32_t(65536.0f * (1.0f - expf(-1.0f / (tau * fs)))), 1, 65536);
  }

  static uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
  }
  bool Chance(int step, uint32_t salt, int percent) const {
    if (percent <= 0) return false;
    if (percent >= 100) return true;
    return int(Hash((uint32_t(seed) << 20) ^ (salt << 16) ^ uint32_t(step)) % 100) < percent;
  }
  int AcidMod() const {
    // CV2 adds to both probabilities (in percent)
    return Proportion(DetentedIn(1), HEMISPHERE_MAX_INPUT_CV, 100);
  }
  bool IsAccent(int s, const AcidMidi::Step &st, int mod) const {
    if (acc_thresh > 0 && st.vel >= acc_thresh) return true;
    return Chance(s, 1, acc_prob * 5 + mod);
  }
  bool SlidesOut(int s, const AcidMidi::Step &st, const AcidMidi::Step &next, int mod) const {
    if (!(next.flags & AcidMidi::STEP_GATE)) return false;
    if (st.flags & AcidMidi::STEP_SLIDE) return true;
    return Chance(s, 2, slide_prob * 5 + mod);
  }

  void ComputeLoop(int &start, int &len) const {
    const int spb = 4 * song_spq;
    start = start_bar * spb;
    if (start >= song.length) start = 0;
    const int avail = song.length - start;
    len = LOOP_TABLE[loop_idx] * spb;
    if (len <= 0 || len > avail) len = avail;
  }

  void StartNote(const AcidMidi::Step &st, int s, int mod, uint32_t now) {
    cur_note = st.note;
    cur_vel = st.vel;
    cur_accent = IsAccent(s, st, mod);
    target_q = MIDIQuantizer::CV(st.note) * 65536;
    if (!slide_armed) {
      pitch_q = target_q;
      // force a short low gap if the gate is somehow still high
      if (gate_high) retrig_tick = now + HEMISPHERE_CLOCK_TICKS;
    }
    gate_high = true;
  }

  void AdvanceStep(uint32_t now) {
    int start, len;
    ComputeLoop(start, len);
    loop_len = len;
    if (len <= 0) return;

    if (reset_pending) {
      pos = 0;
      reset_pending = false;
      slide_armed = false;
    } else {
      pos = (pos + 1) % len;
    }

    const int s = start + pos;
    const int s_next = start + (pos + 1) % len;
    const AcidMidi::Step &st = song.steps[s];
    const AcidMidi::Step &nx = song.steps[s_next];
    const int mod = AcidMod();
    play_step = s;

    if (st.flags & AcidMidi::STEP_GATE) {
      StartNote(st, s, mod, now);
      onset_slides = SlidesOut(s, st, nx, mod);
    } else if (st.flags & AcidMidi::STEP_TIE) {
      // a tie with nothing to hold (e.g. right after a reset): play it
      if (!gate_high) {
        slide_armed = false;
        StartNote(st, s, mod, now);
        onset_slides = false;
      }
    } else {
      // rest
      gate_high = false;
      gate_off_tick = 0;
      slide_armed = false;
      onset_slides = false;
      return;
    }

    // hold the gate into the next step for ties and glides
    bool hold = false;
    if (nx.flags & AcidMidi::STEP_TIE) {
      hold = true;
      slide_armed = false;
    } else if ((nx.flags & AcidMidi::STEP_GATE) && onset_slides) {
      hold = true;
      slide_armed = true;
    } else {
      slide_armed = false;
    }

    if (hold) {
      gate_off_tick = 0;
    } else {
      uint32_t cycle = ClockCycleTicks(0);
      if (cycle == 0 || cycle > 100000) cycle = 2000; // ~120ms until a clock is known
      uint32_t ticks = cycle * (10 + gate_len * 5) / 100;
      if (ticks < HEMISPHERE_CLOCK_TICKS) ticks = HEMISPHERE_CLOCK_TICKS;
      gate_off_tick = now + ticks;
      if (gate_off_tick == 0) gate_off_tick = 1;
    }
  }

  int OutBValue(uint32_t now) const {
    const bool retrig = retrig_tick && int32_t(now - retrig_tick) < 0;
    const bool on = gate_high && !retrig;
    switch (outb_mode) {
      case OUTB_GATE:     return on ? GATE_ACCENT_CV : 0;
      case OUTB_ACCENT:   return (on && cur_accent) ? GATE_ACCENT_CV : 0;
      case OUTB_VELOCITY: return Proportion(cur_vel, 127, GATE_ACCENT_CV);
      default:            return on ? (cur_accent ? GATE_ACCENT_CV : GATE_NORMAL_CV) : 0;
    }
  }

  // File I/O and parsing are too slow for the ISR, and some callers (e.g.
  // OnDataReceive during a preset jump) run inside it. So requests only set
  // a flag, and the work is deferred to the main loop via the core task queue.
  void QueueService() {
    if (service_queued) return;
    service_queued = true;
    OC::CORE::DeferTask([this]() {
      service_queued = false;
      ServiceLoads();
    });
  }
  void RequestLoad() {
    load_pending = true;
    QueueService();
  }
  void RequestRescan() {
    rescan_pending = true;
    QueueService();
  }
  void ServiceLoads();
  void LoadFile();
  const char *FileName(int idx, char *buf) const;
  void DrawRow(int row, int y);
  void DrawStatus();
};

// ---------------------------------------------------------------------------

FLASHMEM void AcidMIDI::Start() {
  file_idx = 0;
  part = 0;
  res_idx = 2;
  start_bar = 0;
  loop_idx = 0;
  acc_thresh = 100;
  acc_prob = 0;
  slide_prob = 0;
  seed = random(256);
  glide = 20;
  gate_len = 8;
  outb_mode = OUTB_GATE_ACCENT;
  octave = 0;
  UpdateGlideCoeff();

  song.length = 0;
  song.part_count = 0;
  reset_pending = true;
  RequestLoad();
}

// Main loop context (deferred task): file I/O and parsing happen here
FLASHMEM void AcidMIDI::ServiceLoads() {
#ifdef ARDUINO_TEENSY41
  if (rescan_pending || !AcidMidiShared::sd_scanned) {
    rescan_pending = false;
    AcidMidiShared::ScanSD();
  }
#endif
  if (load_pending) {
    load_pending = false;
    LoadFile();
  }
}

FLASHMEM const char *AcidMIDI::FileName(int idx, char *buf) const {
  if (idx < AcidMidiData::file_count) return AcidMidiData::files[idx].name;
#ifdef ARDUINO_TEENSY41
  idx -= AcidMidiData::file_count;
  if (idx < AcidMidiShared::sd_count) {
    // show the name without extension, max 8 chars
    const char *name = AcidMidiShared::sd_names[idx];
    int n = 0;
    while (n < 8 && name[n] && name[n] != '.') { buf[n] = name[n]; ++n; }
    buf[n] = '\0';
    return buf;
  }
#endif
  return "---";
}

FLASHMEM void AcidMIDI::LoadFile() {
  using namespace AcidMidiShared;
  if (!scratch) {
    scratch = (AcidMidi::Song *)extmem_malloc(sizeof(AcidMidi::Song));
    if (!scratch) {
      load_result = AcidMidi::ERR_NO_MEMORY;
      return;
    }
  }

  const uint8_t *data = nullptr;
  size_t size = 0;
  uint8_t *buf = nullptr;
  const int idx = file_idx;

  if (idx >= FileCount()) {
    // e.g. a preset that points at an SD file while no card is present
  } else if (idx < AcidMidiData::file_count) {
    data = AcidMidiData::files[idx].data;
    size = AcidMidiData::files[idx].size;
  }
#ifdef ARDUINO_TEENSY41
  else {
    char path[8 + NAME_LEN];
    snprintf(path, sizeof(path), "/MIDI/%s", sd_names[idx - AcidMidiData::file_count]);
    File f = SD.open(path);
    if (f) {
      size = f.size();
      if (size > 0 && size <= MAX_FILE_SIZE) {
        // extmem_malloc falls back to regular heap if there's no PSRAM
        buf = (uint8_t *)extmem_malloc(size);
        if (buf && f.read(buf, size) == size) data = buf;
      }
      f.close();
    }
  }
#endif

  char namebuf[9];
  strncpy(file_name, FileName(idx, namebuf), sizeof(file_name) - 1);
  file_name[sizeof(file_name) - 1] = '\0';

  const uint8_t spq = SPQ_TABLE[res_idx];
  AcidMidi::Result res = AcidMidi::ERR_NO_FILE;
  if (data) {
    res = AcidMidi::Parse(data, size, part, spq, *scratch);
  } else {
    scratch->Clear();
  }
  if (buf) extmem_free(buf);

  // swap in the new pattern; Controller() stays out while we copy
  loading = true;
  __asm__ volatile("" ::: "memory"); // don't let the copy move above the flag
  if (res == AcidMidi::OK) {
    memcpy(&song, scratch, sizeof(AcidMidi::Song));
  } else {
    song.length = 0;
    song.part_count = scratch->part_count;
  }
  song_spq = spq;
  if (song.length > 0) {
    int start, len;
    ComputeLoop(start, len);
    if (pos >= len) pos = len - 1; // keep playing roughly in place
  }
  load_result = res;
  __asm__ volatile("" ::: "memory");
  loading = false;
}

void AcidMIDI::Controller() {
  const uint32_t now = HS::get_tick();
  if (Clock(1)) reset_pending = true;

  if (loading || song.length == 0) {
    gate_high = false;
    gate_off_tick = 0;
    Out(1, outb_mode == OUTB_VELOCITY ? OutBValue(now) : 0);
    return;
  }

  if (Clock(0)) AdvanceStep(now);

  if (gate_off_tick && int32_t(now - gate_off_tick) >= 0) {
    gate_off_tick = 0;
    gate_high = false;
  }
  if (retrig_tick && int32_t(now - retrig_tick) >= 0) retrig_tick = 0;

  // 303-style exponential glide
  gliding = false;
  if (pitch_q != target_q) {
    const int32_t diff = target_q - pitch_q;
    if (abs(diff) < 64) {
      pitch_q = target_q;
    } else {
      int32_t d = int32_t((int64_t(diff) * glide_coeff) >> 16);
      if (d == 0) d = (diff > 0) ? 1 : -1;
      pitch_q += d;
      gliding = true;
    }
  }

  const int transpose = SemitoneIn(0) * 128 + octave * ONE_OCTAVE;
  Out(0, (pitch_q >> 16) + transpose);
  Out(1, OutBValue(now));
}

// ---------------------------------------------------------------------------
// UI

FLASHMEM void AcidMIDI::DrawRow(int row, int y) {
  char buf[12];
  const char *label = "";
  const char *value = buf;
  buf[0] = '\0';

  switch (row) {
    case FILE_SEL: {
      gfxIcon(1, y, MIDI_ICON);
      char namebuf[9];
      gfxPrint(11, y, FileName(file_idx, namebuf));
      if (cursor == FILE_SEL) gfxCursor(11, y + 8, 50);
      return;
    }
    case PART:
      label = "Part";
      if (song.part_count) snprintf(buf, sizeof(buf), "%d/%d", part + 1, song.part_count);
      else snprintf(buf, sizeof(buf), "%d", part + 1);
      break;
    case RESOLUTION: label = "Step"; value = RES_NAMES[res_idx]; break;
    case START_BAR:  label = "Start"; snprintf(buf, sizeof(buf), "%d", start_bar + 1); break;
    case LOOP_BARS:
      label = "Bars";
      if (LOOP_TABLE[loop_idx]) snprintf(buf, sizeof(buf), "%d", LOOP_TABLE[loop_idx]);
      else value = "All";
      break;
    case ACC_THRESH:
      label = "AccV>";
      if (acc_thresh) snprintf(buf, sizeof(buf), "%d", acc_thresh);
      else value = "Off";
      break;
    case ACC_PROB:   label = "Acc%"; snprintf(buf, sizeof(buf), "+%d", acc_prob * 5); break;
    case SLIDE_PROB: label = "Gld%"; snprintf(buf, sizeof(buf), "+%d", slide_prob * 5); break;
    case SEED:       label = "Seed"; snprintf(buf, sizeof(buf), "%d", seed); break;
    case GLIDE:      label = "Glide"; snprintf(buf, sizeof(buf), "%dms", glide * 3); break;
    case GATE_LEN:   label = "Gate"; snprintf(buf, sizeof(buf), "%d%%", 10 + gate_len * 5); break;
    case OUT_MODE:   label = "OutB"; value = OUTB_NAMES[outb_mode]; break;
    case OCTAVE:     label = "Oct"; snprintf(buf, sizeof(buf), "%+d", octave); break;
  }
  gfxPrint(1, y, label);
  gfxPrint(33, y, value);
  if (cursor == row) gfxCursor(33, y + 8, 30);
}

FLASHMEM void AcidMIDI::DrawStatus() {
  const int y = 55;
  gfxLine(0, 52, 63, 52);
  if (loading) {
    gfxPrint(1, y, "Loading");
    return;
  }
  if (load_result != AcidMidi::OK || song.length == 0) {
    gfxPrint(1, y, AcidMidi::ResultString(load_result));
    return;
  }

  gfxPrint(1, y, pos + 1);
  // note name while the gate is open
  if (gate_high) {
    gfxPrint(31, y, midi_note_numbers[cur_note]);
    if (cur_accent) gfxPrint(25, y, "!");
  }
  if (gliding || slide_armed) gfxIcon(55, y, BEND_ICON);

  // loop progress
  if (loop_len > 0) gfxLine(0, 63, (pos * 63) / loop_len, 63);
}

FLASHMEM void AcidMIDI::View() {
  int first = cursor - (VISIBLE_ROWS - 1);
  if (first < 0) first = 0;
  for (int r = 0; r < VISIBLE_ROWS; ++r) {
    const int row = first + r;
    if (row > CURSOR_LAST) break;
    DrawRow(row, 14 + r * 10);
  }
  DrawStatus();
}

FLASHMEM void AcidMIDI::OnButtonPress() {
#ifdef ARDUINO_TEENSY41
  // pick up new files on the card when starting to edit the file selector
  if (cursor == FILE_SEL && !EditMode()) RequestRescan();
#endif
  CursorToggle();
}

FLASHMEM void AcidMIDI::AuxButton() {
  if (cursor == SEED) {
    seed = random(256);
  } else if (cursor == FILE_SEL || cursor == PART) {
    // reload (e.g. after copying a new version of the file to the card)
    rescan_pending = true;
    RequestLoad();
  }
  CancelEdit();
}

FLASHMEM void AcidMIDI::OnEncoderMove(int direction) {
  if (!EditMode()) {
    MoveCursor(cursor, direction, CURSOR_LAST);
    return;
  }

  switch (cursor) {
    case FILE_SEL: {
      const int count = AcidMidiShared::FileCount();
      file_idx = (file_idx + direction + count) % count;
      part = 0;
      RequestLoad();
      break;
    }
    case PART: {
      const int max_part = song.part_count ? song.part_count : AcidMidi::MAX_PARTS;
      part = constrain(part + direction, 0, max_part - 1);
      RequestLoad();
      break;
    }
    case RESOLUTION:
      res_idx = constrain(res_idx + direction, 0, (int)sizeof(SPQ_TABLE) - 1);
      RequestLoad();
      break;
    case START_BAR:  start_bar = constrain(start_bar + direction, 0, 63); break;
    case LOOP_BARS:  loop_idx = constrain(loop_idx + direction, 0, (int)sizeof(LOOP_TABLE) - 1); break;
    case ACC_THRESH: acc_thresh = constrain(acc_thresh + direction, 0, 127); break;
    case ACC_PROB:   acc_prob = constrain(acc_prob + direction, 0, 20); break;
    case SLIDE_PROB: slide_prob = constrain(slide_prob + direction, 0, 20); break;
    case SEED:       seed = uint8_t(seed + direction); break;
    case GLIDE:
      glide = constrain(glide + direction, 0, 63);
      UpdateGlideCoeff();
      break;
    case GATE_LEN:   gate_len = constrain(gate_len + direction, 0, 15); break;
    case OUT_MODE:   outb_mode = constrain(outb_mode + direction, 0, OUTB_MODE_COUNT - 1); break;
    case OCTAVE:     octave = constrain(octave + direction, -3, 3); break;
  }
}

uint64_t AcidMIDI::OnDataRequest() {
  uint64_t data = 0;
  Pack(data, PackLocation{0, 8}, file_idx);
  Pack(data, PackLocation{8, 4}, part);
  Pack(data, PackLocation{12, 3}, res_idx);
  Pack(data, PackLocation{15, 6}, start_bar);
  Pack(data, PackLocation{21, 3}, loop_idx);
  Pack(data, PackLocation{24, 7}, acc_thresh);
  Pack(data, PackLocation{31, 5}, acc_prob);
  Pack(data, PackLocation{36, 5}, slide_prob);
  Pack(data, PackLocation{41, 8}, seed);
  Pack(data, PackLocation{49, 6}, glide);
  Pack(data, PackLocation{55, 4}, gate_len);
  Pack(data, PackLocation{59, 2}, outb_mode);
  Pack(data, PackLocation{61, 3}, octave + 3);
  return data;
}

// May be called from the ISR (preset jumps), so only set values here and
// leave the actual file loading to the deferred ServiceLoads()
void AcidMIDI::OnDataReceive(uint64_t data) {
  file_idx   = Unpack(data, PackLocation{0, 8});
  part       = Unpack(data, PackLocation{8, 4});
  res_idx    = constrain(Unpack(data, PackLocation{12, 3}), 0, (int)sizeof(SPQ_TABLE) - 1);
  start_bar  = Unpack(data, PackLocation{15, 6});
  loop_idx   = constrain(Unpack(data, PackLocation{21, 3}), 0, (int)sizeof(LOOP_TABLE) - 1);
  acc_thresh = Unpack(data, PackLocation{24, 7});
  acc_prob   = constrain(Unpack(data, PackLocation{31, 5}), 0, 20);
  slide_prob = constrain(Unpack(data, PackLocation{36, 5}), 0, 20);
  seed       = Unpack(data, PackLocation{41, 8});
  glide      = Unpack(data, PackLocation{49, 6});
  gate_len   = Unpack(data, PackLocation{55, 4});
  outb_mode  = Unpack(data, PackLocation{59, 2});
  octave     = constrain(int(Unpack(data, PackLocation{61, 3})) - 3, -3, 3);
  UpdateGlideCoeff();
  reset_pending = true;
  RequestLoad();
}

void AcidMIDI::SetHelp() {
  //                    "-------" <-- Label size guide
  help[HELP_DIGITAL1] = "Clock";
  help[HELP_DIGITAL2] = "Reset";
  help[HELP_CV1]      = "Transp";
  help[HELP_CV2]      = "Acid%";
  help[HELP_OUT1]     = "Pitch";
  help[HELP_OUT2]     = OUTB_NAMES[outb_mode];
  help[HELP_EXTRA1]   = "MIDI file player w/";
  help[HELP_EXTRA2]   = "303 accent + glide";
  //                    "---------------------" <-- Extra text size guide
}
