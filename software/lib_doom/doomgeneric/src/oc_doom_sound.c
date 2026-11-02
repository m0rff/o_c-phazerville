// oc_doom_sound.c - sound effects and music for the Ornament & Crime DOOM app.
//
// Everything is rendered by ocd_audio_render(), which the app calls from the
// Teensy audio library's update interrupt. Doom's main loop only changes the
// channel and song state below, inside ocd_audio_lock()/ocd_audio_unlock().
//
// Sound effects: the WAD's DMX lumps (8-bit unsigned, usually 11025 Hz) are
// mixed on up to 16 channels with linear interpolation and Doom's stereo
// separation.
//
// Music: the score (MIDI, or MUS converted to MIDI) is played by a small 2-operator FM synth that uses
// the instrument settings in the WAD's GENMIDI lump (the same data the OPL
// chip used), with simplified envelopes. Close in character, not exact.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "memio.h"
#include "mus2mid.h"
#include "oc_doom.h"

// Unused config variables that the sound config code binds
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static int out_rate = 44100;

void ocd_audio_set_rate(int hz) { if (hz > 8000) out_rate = hz; }

// ---------------------------------------------------------------------------
// Sound effects
// ---------------------------------------------------------------------------

#define NUM_SFX_CHANNELS 16

typedef struct {
  const uint8_t *data;
  uint32_t len;       // samples
  uint32_t pos;       // 16.16 position
  uint32_t step;      // 16.16 increment per output sample
  int gain_l, gain_r; // 0..254
  volatile int active;
} sfx_channel_t;

static sfx_channel_t sfx_ch[NUM_SFX_CHANNELS];
static boolean sfx_prefix = true;

static void SetGains(sfx_channel_t *c, int vol, int sep) {
  if (vol < 0) vol = 0;
  if (vol > 127) vol = 127;
  if (sep < 0) sep = 0;
  if (sep > 254) sep = 254;
  c->gain_l = ((254 - sep) * vol) / 127;
  c->gain_r = (sep * vol) / 127;
}

static boolean Snd_Init(boolean use_sfx_prefix) {
  sfx_prefix = use_sfx_prefix;
  memset(sfx_ch, 0, sizeof(sfx_ch));
  return true;
}

static void Snd_Shutdown(void) {
  ocd_audio_lock();
  for (int i = 0; i < NUM_SFX_CHANNELS; i++) sfx_ch[i].active = 0;
  ocd_audio_unlock();
}

static int Snd_GetSfxLumpNum(sfxinfo_t *sfx) {
  char name[16];
  if (sfx->link) sfx = sfx->link;
  if (sfx_prefix)
    snprintf(name, sizeof(name), "ds%s", sfx->name);
  else
    snprintf(name, sizeof(name), "%s", sfx->name);
  return W_CheckNumForName(name);
}

static void Snd_Update(void) {}

static void Snd_UpdateSoundParams(int channel, int vol, int sep) {
  if (channel < 0 || channel >= NUM_SFX_CHANNELS) return;
  ocd_audio_lock();
  SetGains(&sfx_ch[channel], vol, sep);
  ocd_audio_unlock();
}

static int Snd_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
  if (channel < 0 || channel >= NUM_SFX_CHANNELS) return -1;
  int lump = sfx->lumpnum;
  if (lump < 0) return -1;

  // Sound lumps stay cached for the whole session (a few hundred KB of PSRAM)
  const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);
  uint32_t lumplen = W_LumpLength(lump);
  if (lumplen < 8 || data[0] != 3 || data[1] != 0) return -1;
  uint32_t rate = data[2] | (data[3] << 8);
  uint32_t n = data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24);
  if (n > lumplen - 8) n = lumplen - 8;
  // DMX skips 16 padding bytes at each end
  const uint8_t *samples = data + 8;
  if (n > 32) { samples += 16; n -= 32; }
  if (n < 2 || rate == 0) return -1;

  ocd_audio_lock();
  sfx_channel_t *c = &sfx_ch[channel];
  c->data = samples;
  c->len = n;
  c->pos = 0;
  c->step = (uint32_t)(((uint64_t)rate << 16) / (uint32_t)out_rate);
  SetGains(c, vol, sep);
  c->active = 1;
  ocd_audio_unlock();
  return channel;
}

static void Snd_StopSound(int channel) {
  if (channel < 0 || channel >= NUM_SFX_CHANNELS) return;
  ocd_audio_lock();
  sfx_ch[channel].active = 0;
  ocd_audio_unlock();
}

static boolean Snd_SoundIsPlaying(int channel) {
  if (channel < 0 || channel >= NUM_SFX_CHANNELS) return false;
  return sfx_ch[channel].active != 0;
}

static void Snd_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
  (void)sounds; (void)num_sounds;
}

static snddevice_t sound_devices[] = {
  SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
  SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
  sound_devices, arrlen(sound_devices),
  Snd_Init, Snd_Shutdown, Snd_GetSfxLumpNum, Snd_Update,
  Snd_UpdateSoundParams, Snd_StartSound, Snd_StopSound,
  Snd_SoundIsPlaying, Snd_CacheSounds,
};

// Adds the sound effects into the float mix buffers
static void MixSfx(float *mix_l, float *mix_r, int n) {
  for (int c = 0; c < NUM_SFX_CHANNELS; c++) {
    sfx_channel_t *ch = &sfx_ch[c];
    if (!ch->active) continue;
    const uint8_t *d = ch->data;
    const uint32_t len = ch->len, step = ch->step;
    uint32_t pos = ch->pos;
    const float gl = ch->gain_l * (1.0f / (128.0f * 254.0f));
    const float gr = ch->gain_r * (1.0f / (128.0f * 254.0f));
    for (int i = 0; i < n; i++) {
      uint32_t idx = pos >> 16;
      if (idx + 1 >= len) { ch->active = 0; break; }
      int frac = (pos >> 8) & 0xff;
      int s0 = d[idx] - 128, s1 = d[idx + 1] - 128;
      float s = (float)(s0 * 256 + (s1 - s0) * frac) * (1.0f / 256.0f);
      mix_l[i] += s * gl;
      mix_r[i] += s * gr;
      pos += step;
    }
    ch->pos = pos;
  }
}

// ---------------------------------------------------------------------------
// Music: MUS sequencer + GENMIDI 2-op FM synth
// ---------------------------------------------------------------------------

#define NUM_VOICES 18
#define SINE_BITS 10
#define SINE_SIZE (1 << SINE_BITS)

typedef struct __attribute__((packed)) {
  uint8_t tremolo;   // AM, VIB, EG type, KSR, multiplier
  uint8_t attack;    // attack rate << 4 | decay rate
  uint8_t sustain;   // sustain level << 4 | release rate
  uint8_t waveform;
  uint8_t scale;     // key scale level
  uint8_t level;     // total level (attenuation)
} genmidi_op_t;

typedef struct __attribute__((packed)) {
  genmidi_op_t modulator;
  uint8_t feedback;
  genmidi_op_t carrier;
  uint8_t unused;
  int16_t base_note_offset;
} genmidi_voice_t;

typedef struct __attribute__((packed)) {
  uint16_t flags;
  uint8_t fine_tuning;
  uint8_t fixed_note;
  genmidi_voice_t voices[2];
} genmidi_instr_t;

#define GENMIDI_FLAG_FIXED 0x0001
#define GENMIDI_FLAG_2VOICE 0x0004
#define GENMIDI_NUM_INSTRS 175

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
  float phase, inc;       // cycles
  float level;            // 0..1 envelope amplitude
  float att_db;           // attenuation during decay/release
  float attack_inc;       // amplitude per sample
  float decay_db, release_db, sustain_db; // dB per sample / dB
  float tl;               // total-level gain
  float out1, out2;       // last outputs (feedback)
  uint8_t wave, sustaining, state;
} fm_op_t;

typedef struct {
  fm_op_t mod, car;
  float fb;               // feedback, cycles per unit output
  uint8_t additive;
  int8_t channel;
  uint8_t note;           // MUS note (key)
  float gain_l, gain_r;
  uint8_t velocity;
  uint8_t second;         // second voice of a double-voice instrument
  float fine;             // pitch offset of the second voice, semitones
  uint32_t age;
  uint8_t keyon;
  uint8_t active;
  const genmidi_voice_t *patch;
  float base_freq;        // before pitch bend
} fm_voice_t;

typedef struct {
  uint8_t instrument;
  uint8_t volume;         // 0..127
  uint8_t pan;            // 0..127
  uint8_t expression;
  int bend;               // -8192..8191
} mus_channel_t;

#define MAX_TRACKS 32

// A Standard MIDI File (MUS scores are converted to MIDI when registered)
typedef struct {
  const uint8_t *data;
  uint32_t len;
  uint8_t *owned;         // converted data we must free, or NULL
  int ntracks;
  uint16_t division;      // ticks per quarter note
  uint32_t track_start[MAX_TRACKS], track_end[MAX_TRACKS];
} mus_song_t;

typedef struct {
  uint32_t pos, end;
  uint32_t next_tick;
  uint8_t status;
  uint8_t done;
} midi_track_t;

static float sine_table[SINE_SIZE];
static const genmidi_instr_t *genmidi = NULL;
static fm_voice_t voices[NUM_VOICES];
static mus_channel_t mus_ch[16];
static mus_song_t *cur_song = NULL;
static midi_track_t tracks[MAX_TRACKS];
static uint32_t song_tick;
static uint32_t samples_to_event;
static float samples_per_tick;
static float tick_frac;
static boolean song_looping, song_paused, song_playing;
static int music_volume = 127;
static uint32_t voice_clock;

static const float kMult[16] = {0.5f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15};

static float AttackTimeMs(int r) {
  if (r == 0) return 1e9f;
  if (r == 15) return 0.0f;
  return 2826.0f / (float)(1 << r) * 16.0f;   // 2826 ms at rate 4, halving
}
static float DecayTimeMs(int r) {
  if (r == 0) return 1e9f;
  return 39280.0f / (float)(1 << r) * 16.0f;  // 39.3 s at rate 4, halving
}

static inline float WaveSample(int wave, float phase) {
  // phase in cycles, any range
  int idx = (int)(phase * SINE_SIZE) & (SINE_SIZE - 1);
  float s = sine_table[idx];
  switch (wave & 3) {
    case 1: return s > 0 ? s : 0;                          // half sine
    case 2: return s > 0 ? s : -s;                         // abs sine
    case 3: return ((idx >> (SINE_BITS - 2)) & 1) ? 0 : (s > 0 ? s : -s);  // quarter sine pulses
    default: return s;
  }
}

static void OpSetup(fm_op_t *op, const genmidi_op_t *p, float freq, float extra_gain) {
  op->inc = freq * kMult[p->tremolo & 0x0f] / (float)out_rate;
  op->wave = p->waveform & 3;
  op->sustaining = (p->tremolo & 0x20) != 0;
  float at = AttackTimeMs(p->attack >> 4);
  op->attack_inc = at <= 0.0f ? 1.0f : 1000.0f / (at * out_rate);
  op->decay_db = 96.0f * 1000.0f / (DecayTimeMs(p->attack & 0x0f) * out_rate);
  op->release_db = 96.0f * 1000.0f / (DecayTimeMs(p->sustain & 0x0f) * out_rate);
  int sl = p->sustain >> 4;
  op->sustain_db = sl == 15 ? 93.0f : sl * 3.0f;
  op->tl = powf(10.0f, -0.75f * (p->level & 0x3f) / 20.0f) * extra_gain;
  op->phase = 0;
  op->level = 0;
  op->att_db = 0;
  op->out1 = op->out2 = 0;
  op->state = ENV_ATTACK;
}

static inline float OpEnvelope(fm_op_t *op) {
  switch (op->state) {
    case ENV_ATTACK:
      op->level += op->attack_inc;
      if (op->level >= 1.0f) { op->level = 1.0f; op->att_db = 0; op->state = ENV_DECAY; }
      return op->level;
    case ENV_DECAY:
      op->att_db += op->decay_db;
      if (op->att_db >= op->sustain_db) {
        op->att_db = op->sustain_db;
        op->state = op->sustaining ? ENV_SUSTAIN : ENV_RELEASE;
      }
      break;
    case ENV_SUSTAIN:
      break;
    case ENV_RELEASE:
      op->att_db += op->release_db;
      if (op->att_db >= 96.0f) { op->state = ENV_OFF; op->level = 0; return 0; }
      break;
    default:
      return 0;
  }
  return op->level;   // recomputed from att_db at control rate
}

static inline void OpUpdateLevel(fm_op_t *op) {
  if (op->state != ENV_ATTACK && op->state != ENV_OFF)
    op->level = powf(10.0f, -op->att_db * (1.0f / 20.0f));
}

// DMX's frequency table puts A440 at note 57, an octave above MIDI
static float NoteFreq(float note) {
  return 440.0f * powf(2.0f, (note - 57.0f) * (1.0f / 12.0f));
}

// DMX's volume curve (from Chocolate Doom's OPL music code)
static const uint8_t volume_mapping[128] = {
  0, 1, 3, 5, 6, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23,
  25, 26, 27, 29, 30, 32, 33, 34, 36, 37, 39, 41, 43, 45, 47, 49,
  50, 52, 54, 55, 57, 59, 60, 61, 63, 64, 66, 67, 68, 69, 71, 72,
  73, 74, 75, 76, 77, 79, 80, 81, 82, 83, 84, 84, 85, 86, 87, 88,
  89, 90, 91, 92, 92, 93, 94, 95, 96, 96, 97, 98, 99, 99, 100, 101,
  101, 102, 103, 103, 104, 105, 105, 106, 107, 107, 108, 109, 109, 110, 110, 111,
  112, 112, 113, 113, 114, 114, 115, 115, 116, 117, 117, 118, 118, 119, 119, 120,
  120, 121, 121, 122, 122, 123, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127,
};

static inline float LevelGain(int tl) {   // OPL total level: 0.75 dB steps
  return powf(10.0f, -0.75f * tl / 20.0f);
}

// Sets the carrier level from note velocity and channel volume, as DMX did,
// plus the stereo position.
static void VoiceSetPan(fm_voice_t *v) {
  const mus_channel_t *ch = &mus_ch[v->channel];
  int chvol = ch->volume * ch->expression / 127;
  if (chvol > music_volume) chvol = music_volume;
  int midi_volume = 2 * (volume_mapping[chvol] + 1);
  int full_volume = (volume_mapping[v->velocity & 0x7f] * midi_volume) >> 9;
  int car_tl = 0x3f - full_volume;
  v->car.tl = LevelGain(car_tl);
  if (v->additive) {
    int mod_tl = v->patch->modulator.level & 0x3f;
    if (mod_tl != 0x3f && mod_tl < car_tl) mod_tl = car_tl;
    v->mod.tl = LevelGain(mod_tl);
  }
  float pan = ch->pan / 127.0f;
  v->gain_l = sqrtf(1.0f - pan);
  v->gain_r = sqrtf(pan);
}

static void VoiceSetPitch(fm_voice_t *v) {
  float bend = mus_ch[v->channel].bend / 4096.0f + v->fine;   // +/-2 semitones
  float f = v->base_freq * powf(2.0f, bend / 12.0f);
  const genmidi_voice_t *p = v->patch;
  v->mod.inc = f * kMult[p->modulator.tremolo & 0x0f] / (float)out_rate;
  v->car.inc = f * kMult[p->carrier.tremolo & 0x0f] / (float)out_rate;
}

static void NoteOff(int channel, int note) {
  for (int i = 0; i < NUM_VOICES; i++) {
    fm_voice_t *v = &voices[i];
    if (v->active && v->keyon && v->channel == channel && v->note == note) {
      v->keyon = 0;
      if (v->mod.state != ENV_OFF) v->mod.state = ENV_RELEASE;
      if (v->car.state != ENV_OFF) v->car.state = ENV_RELEASE;
    }
  }
}

static void AllNotesOff(int channel) {
  for (int i = 0; i < NUM_VOICES; i++) {
    fm_voice_t *v = &voices[i];
    if (v->active && (channel < 0 || v->channel == channel)) {
      v->keyon = 0;
      if (v->mod.state != ENV_OFF) v->mod.state = ENV_RELEASE;
      if (v->car.state != ENV_OFF) v->car.state = ENV_RELEASE;
    }
  }
}

static fm_voice_t *AllocVoice(void) {
  fm_voice_t *best = NULL;
  // free voice first, then the oldest released one, then the oldest
  for (int i = 0; i < NUM_VOICES; i++)
    if (!voices[i].active) return &voices[i];
  for (int i = 0; i < NUM_VOICES; i++)
    if (!voices[i].keyon && (!best || voices[i].age < best->age)) best = &voices[i];
  if (best) return best;
  for (int i = 0; i < NUM_VOICES; i++)
    if (!best || voices[i].age < best->age) best = &voices[i];
  return best;
}

static void VoiceOn(int channel, int key, int note, int velocity,
                    const genmidi_instr_t *instr, int second) {
  const genmidi_voice_t *p = &instr->voices[second];
  int n = note;
  if (!(instr->flags & GENMIDI_FLAG_FIXED)) n += (int16_t)p->base_note_offset;
  while (n < 0) n += 12;
  while (n > 95) n -= 12;

  fm_voice_t *v = AllocVoice();
  v->channel = channel;
  v->note = key;
  v->velocity = velocity;
  v->second = second;
  v->fine = second ? ((instr->fine_tuning / 2) - 64) / 32.0f : 0.0f;
  v->patch = p;
  v->base_freq = NoteFreq((float)n);
  v->additive = p->feedback & 1;
  int fb = (p->feedback >> 1) & 7;
  v->fb = fb ? (float)(1 << fb) / 64.0f : 0.0f;
  OpSetup(&v->mod, &p->modulator, v->base_freq, 1.0f);
  OpSetup(&v->car, &p->carrier, v->base_freq, 1.0f);
  VoiceSetPitch(v);
  VoiceSetPan(v);
  v->age = ++voice_clock;
  v->keyon = 1;
  v->active = 1;
}

static void NoteOn(int channel, int note, int velocity) {
  if (!genmidi) return;
  const genmidi_instr_t *instr;
  int key = note;
  if (channel == 9) {
    if (note < 35 || note > 81) return;
    instr = &genmidi[128 + note - 35];
  } else {
    instr = &genmidi[mus_ch[channel].instrument & 0x7f];
  }
  if (instr->flags & GENMIDI_FLAG_FIXED) note = instr->fixed_note;
  VoiceOn(channel, key, note, velocity, instr, 0);
  if (instr->flags & GENMIDI_FLAG_2VOICE)
    VoiceOn(channel, key, note, velocity, instr, 1);
}

static void ResetChannels(void) {
  for (int i = 0; i < 16; i++) {
    mus_ch[i].instrument = 0;
    mus_ch[i].volume = 100;
    mus_ch[i].pan = 64;
    mus_ch[i].expression = 127;
    mus_ch[i].bend = 0;
  }
}

static void StopAllVoices(void) {
  for (int i = 0; i < NUM_VOICES; i++) voices[i].active = 0;
}

static void UpdateChannelVoices(int channel, int pitch) {
  for (int i = 0; i < NUM_VOICES; i++) {
    fm_voice_t *v = &voices[i];
    if (v->active && v->channel == channel) {
      if (pitch) VoiceSetPitch(v);
      else VoiceSetPan(v);
    }
  }
}

static inline int TrackByte(midi_track_t *t) {
  if (t->pos >= t->end) { t->done = 1; return 0; }
  return cur_song->data[t->pos++];
}

static uint32_t ReadVarLen(midi_track_t *t) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    int b = TrackByte(t);
    v = (v << 7) | (b & 0x7f);
    if (!(b & 0x80)) break;
  }
  return v;
}

static void SetTempo(uint32_t us_per_quarter) {
  if (us_per_quarter == 0) us_per_quarter = 500000;
  samples_per_tick = (float)out_rate * (us_per_quarter / 1e6f) / cur_song->division;
}

static void Controller(int channel, int ctrl, int val) {
  mus_channel_t *ch = &mus_ch[channel];
  switch (ctrl) {
    case 7: ch->volume = val; UpdateChannelVoices(channel, 0); break;
    case 10: ch->pan = val; UpdateChannelVoices(channel, 0); break;
    case 11: ch->expression = val; UpdateChannelVoices(channel, 0); break;
    case 120: case 123: AllNotesOff(channel); break;
    case 121: ch->expression = 127; ch->bend = 0; UpdateChannelVoices(channel, 1); break;
    default: break;
  }
}

// Runs one event of a track. Returns 0 when the whole song should end.
static int TrackEvent(midi_track_t *t) {
  int b = TrackByte(t);
  int status;
  if (b & 0x80) { status = b; b = -1; }
  else status = t->status;            // running status
  if (status < 0x80) { t->done = 1; return 1; }
  int type = status & 0xf0, channel = status & 0x0f;
  if (status < 0xf0) t->status = status;
  int d1 = 0, d2 = 0;
  if (type != 0xf0) {
    d1 = b >= 0 ? b : TrackByte(t);
    if (type != 0xc0 && type != 0xd0) d2 = TrackByte(t);
  }
  switch (type) {
    case 0x80: NoteOff(channel, d1); break;
    case 0x90: if (d2) NoteOn(channel, d1, d2); else NoteOff(channel, d1); break;
    case 0xb0: Controller(channel, d1, d2); break;
    case 0xc0: mus_ch[channel].instrument = d1; break;
    case 0xe0:
      mus_ch[channel].bend = ((d2 << 7) | d1) - 8192;
      UpdateChannelVoices(channel, 1);
      break;
    case 0xf0:
      if (status == 0xff) {
        int meta = TrackByte(t);
        uint32_t len = ReadVarLen(t);
        if (meta == 0x51 && len == 3) {
          const uint8_t *d = cur_song->data + t->pos;
          SetTempo((d[0] << 16) | (d[1] << 8) | d[2]);
        } else if (meta == 0x2f) {
          t->done = 1;
        }
        t->pos += len;
      } else if (status == 0xf0 || status == 0xf7) {
        t->pos += ReadVarLen(t);
      }
      break;
    default: break;   // aftertouch: data bytes already read
  }
  if (!t->done) t->next_tick += ReadVarLen(t);
  return 1;
}

// Processes all events due now. Returns ticks until the next event, or -1
// when every track has ended.
static int ProcessEvents(void) {
  for (;;) {
    uint32_t next = 0xffffffff;
    for (int i = 0; i < cur_song->ntracks; i++) {
      midi_track_t *t = &tracks[i];
      while (!t->done && t->next_tick <= song_tick) TrackEvent(t);
      if (!t->done && t->next_tick < next) next = t->next_tick;
    }
    if (next == 0xffffffff) return -1;
    if (next > song_tick) {
      int ticks = next - song_tick;
      song_tick = next;
      return ticks;
    }
  }
}

static void RestartSong(void) {
  for (int i = 0; i < cur_song->ntracks; i++) {
    tracks[i].pos = cur_song->track_start[i];
    tracks[i].end = cur_song->track_end[i];
    tracks[i].status = 0;
    tracks[i].done = 0;
    tracks[i].next_tick = 0;
    tracks[i].next_tick = ReadVarLen(&tracks[i]);
  }
  song_tick = 0;
  SetTempo(500000);
  ResetChannels();
  samples_to_event = 0;
  tick_frac = 0;
}

static void MixMusic(float *mix_l, float *mix_r, int n) {
  int done = 0;
  while (done < n) {
    if (song_playing && !song_paused && cur_song) {
      while (samples_to_event == 0) {
        int ticks = ProcessEvents();
        if (ticks < 0) {
          if (song_looping) {
            RestartSong();
            ticks = ProcessEvents();
            if (ticks < 0) { song_playing = false; break; }
          } else {
            song_playing = false;
            AllNotesOff(-1);
            break;
          }
        }
        float s = ticks * samples_per_tick + tick_frac;
        samples_to_event = (uint32_t)s;
        tick_frac = s - samples_to_event;
      }
    }
    int chunk = n - done;
    if (song_playing && !song_paused && samples_to_event > 0 && (uint32_t)chunk > samples_to_event)
      chunk = samples_to_event;
    if (chunk > 16) chunk = 16;   // envelope control rate

    for (int i = 0; i < NUM_VOICES; i++) {
      fm_voice_t *v = &voices[i];
      if (!v->active) continue;
      OpUpdateLevel(&v->mod);
      OpUpdateLevel(&v->car);
      const float gl = v->gain_l * 0.25f;
      const float gr = v->gain_r * 0.25f;
      fm_op_t *m = &v->mod, *c = &v->car;
      for (int k = 0; k < chunk; k++) {
        float me = OpEnvelope(m) * m->tl;
        float ce = OpEnvelope(c) * c->tl;
        float fbmod = (m->out1 + m->out2) * 0.5f * v->fb;
        float mo = WaveSample(m->wave, m->phase + fbmod) * me;
        m->out2 = m->out1;
        m->out1 = mo;
        float co;
        if (v->additive)
          co = WaveSample(c->wave, c->phase) * ce + mo;
        else
          co = WaveSample(c->wave, c->phase + mo * 4.0f) * ce;
        m->phase += m->inc; if (m->phase >= 1.0f) m->phase -= 1.0f;
        c->phase += c->inc; if (c->phase >= 1.0f) c->phase -= 1.0f;
        mix_l[done + k] += co * gl;
        mix_r[done + k] += co * gr;
      }
      if (c->state == ENV_OFF && (!v->additive || m->state == ENV_OFF)) v->active = 0;
    }
    done += chunk;
    if (song_playing && !song_paused && samples_to_event > 0) samples_to_event -= chunk;
  }
}

static boolean Mus_Init(void) {
  for (int i = 0; i < SINE_SIZE; i++)
    sine_table[i] = sinf(2.0f * 3.14159265f * i / SINE_SIZE);
  return true;
}

static void LoadGenmidi(void) {
  if (genmidi) return;
  int lump = W_CheckNumForName("GENMIDI");
  if (lump < 0) return;
  if (W_LumpLength(lump) < 8 + GENMIDI_NUM_INSTRS * sizeof(genmidi_instr_t)) return;
  const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);
  if (memcmp(d, "#OPL_II#", 8) != 0) return;
  genmidi = (const genmidi_instr_t *)(d + 8);
}

static void Mus_Shutdown(void) {
  ocd_audio_lock();
  song_playing = false;
  cur_song = NULL;
  StopAllVoices();
  ocd_audio_unlock();
}

static void Mus_SetMusicVolume(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 127) volume = 127;
  ocd_audio_lock();
  music_volume = volume;
  for (int i = 0; i < NUM_VOICES; i++)
    if (voices[i].active) VoiceSetPan(&voices[i]);
  ocd_audio_unlock();
}

static void Mus_PauseMusic(void) {
  ocd_audio_lock();
  song_paused = true;
  AllNotesOff(-1);
  ocd_audio_unlock();
}

static void Mus_ResumeMusic(void) {
  song_paused = false;
}

static uint32_t Be32(const uint8_t *d) { return ((uint32_t)d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]; }

static void *Mus_RegisterSong(void *data, int len) {
  const uint8_t *d = data;
  uint8_t *owned = NULL;
  if (len >= 4 && memcmp(d, "MUS\x1a", 4) == 0) {
    // Convert MUS to MIDI, as Chocolate Doom does
    MEMFILE *in = mem_fopen_read(data, len);
    MEMFILE *out = mem_fopen_write();
    boolean bad = mus2mid(in, out);
    if (!bad) {
      void *buf; size_t buflen;
      mem_get_buf(out, &buf, &buflen);
      owned = Z_Malloc(buflen, PU_STATIC, NULL);
      memcpy(owned, buf, buflen);
      d = owned;
      len = buflen;
    }
    mem_fclose(in);
    mem_fclose(out);
    if (bad) return NULL;
  }
  if (len < 14 || memcmp(d, "MThd", 4) != 0) { if (owned) Z_Free(owned); return NULL; }

  mus_song_t *s = Z_Malloc(sizeof(mus_song_t), PU_STATIC, NULL);
  memset(s, 0, sizeof(*s));
  s->data = d;
  s->len = len;
  s->owned = owned;
  s->division = (d[12] << 8) | d[13];
  if (s->division == 0 || (s->division & 0x8000)) s->division = 140;   // SMPTE: not used by Doom
  uint32_t pos = 8 + Be32(d + 4);
  while (pos + 8 <= (uint32_t)len && s->ntracks < MAX_TRACKS) {
    uint32_t clen = Be32(d + pos + 4);
    uint32_t start = pos + 8, end = start + clen;
    if (end > (uint32_t)len) end = len;
    if (memcmp(d + pos, "MTrk", 4) == 0) {
      s->track_start[s->ntracks] = start;
      s->track_end[s->ntracks] = end;
      s->ntracks++;
    }
    pos = end;
  }
  if (s->ntracks == 0) { if (owned) Z_Free(owned); Z_Free(s); return NULL; }
  return s;
}

static void Mus_StopSong(void) {
  ocd_audio_lock();
  song_playing = false;
  AllNotesOff(-1);
  StopAllVoices();
  ocd_audio_unlock();
}

static void Mus_UnRegisterSong(void *handle) {
  if (!handle) return;
  ocd_audio_lock();
  if (cur_song == handle) {
    song_playing = false;
    cur_song = NULL;
    StopAllVoices();
  }
  ocd_audio_unlock();
  mus_song_t *s = handle;
  if (s->owned) Z_Free(s->owned);
  Z_Free(s);
}

static void Mus_PlaySong(void *handle, boolean looping) {
  if (!handle) return;
  LoadGenmidi();
  ocd_audio_lock();
  StopAllVoices();
  cur_song = handle;
  song_looping = looping;
  song_paused = false;
  RestartSong();
  song_playing = true;
  ocd_audio_unlock();
}

static boolean Mus_MusicIsPlaying(void) { return song_playing; }

static snddevice_t music_devices[] = {
  SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS,
  SNDDEVICE_GENMIDI, SNDDEVICE_AWE32, SNDDEVICE_ADLIB, SNDDEVICE_SB,
};

music_module_t DG_music_module = {
  music_devices, arrlen(music_devices),
  Mus_Init, Mus_Shutdown, Mus_SetMusicVolume, Mus_PauseMusic, Mus_ResumeMusic,
  Mus_RegisterSong, Mus_UnRegisterSong, Mus_PlaySong, Mus_StopSong,
  Mus_MusicIsPlaying, NULL,
};

// ---------------------------------------------------------------------------
// Output, called from the audio interrupt
// ---------------------------------------------------------------------------

#define MAX_BLOCK 256

void ocd_audio_render(int16_t *left, int16_t *right, int n) {
  static float mix_l[MAX_BLOCK], mix_r[MAX_BLOCK];
  while (n > 0) {
    int chunk = n > MAX_BLOCK ? MAX_BLOCK : n;
    memset(mix_l, 0, chunk * sizeof(float));
    memset(mix_r, 0, chunk * sizeof(float));
    MixSfx(mix_l, mix_r, chunk);
    MixMusic(mix_l, mix_r, chunk);
    for (int i = 0; i < chunk; i++) {
      float l = mix_l[i] * 32767.0f, r = mix_r[i] * 32767.0f;
      if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
      if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
      left[i] = (int16_t)l;
      right[i] = (int16_t)r;
    }
    left += chunk; right += chunk; n -= chunk;
  }
}
