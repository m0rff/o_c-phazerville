// Doom.h - DOOM for Ornament & Crime (Teensy 4.1, Phazerville)
//
// Runs doomgeneric (lib_doom/doomgeneric) as a full-screen app. The game data
// (an IWAD) is read from the Teensy 4.1's built-in microSD card, or from a WAD
// packed into flash by the multiboot build (lib_doom/pack_wad.py).
// Sound effects and music take over the audio outputs while DOOM runs.
//
// Controls
//   Left encoder ........ turn (menus: up/down)
//   Right encoder ....... walk throttle: back 2 / back / stop / walk / run
//                         (menus: left/right)
//   Right encoder push .. stop;   long press: Doom menu (Esc)
//   Left encoder push ... next weapon;   long press: automap
//   UP / A button ....... fire (menus: select / "yes")
//   DOWN / B button ..... use / open doors (menus: back)
//   X / Y (if present) .. strafe left / right
//   Hold DOWN + turn L .. display contrast;  hold DOWN + turn R: dithering
//   Hold UP + push R .... leave to the Phazerville app menu (global hotkey)
//
// CV / gates
//   TR1 fire (gate) · TR2 use (gate) · TR3 next weapon (trigger) · TR4 walk forward (gate)
//   CV1 turn (bipolar) · CV2 walk / back (bipolar)
//   Out A: gate while firing · Out B: trigger when hurt
//   Out C: health 0-5 V · Out D: ammo 0-5 V
//
// Doom is GPL-2.0-or-later (id Software, Chocolate Doom, doomgeneric authors).

#include <SD.h>
#include <Audio.h>
#include "HSApplication.h"
#include "../AudioIO.h"
#include "../lib_doom/doomgeneric/src/oc_doom.h"
#include "../lib_doom/doomgeneric/src/doomkeys.h"

#if defined(ARDUINO_TEENSY41)
extern "C" uint8_t external_psram_size;
#endif

namespace ocdoom {

// IWADs looked for in the SD card root, first match wins
static const char *const kWadNames[] = {
  "doom.wad", "doomu.wad", "freedoom1.wad", "doom1.wad", "doom2.wad", "freedoom2.wad",
};

static inline int psram_mb() {
#if defined(ARDUINO_TEENSY41)
  return external_psram_size;
#else
  return 0;
#endif
}

static File wad_files[2];

// Doom's sound effects and music as a stereo audio stream. It feeds the
// audio outputs only while DOOM runs in the foreground (see AudioIO).
class DoomAudioStream : public AudioStream {
public:
  DoomAudioStream() : AudioStream(0, nullptr) {}
  volatile bool enabled = false;
  virtual void update(void) {
    if (!enabled) return;
    audio_block_t *l = allocate();
    audio_block_t *r = allocate();
    if (l && r) {
      ocd_audio_render(l->data, r->data, AUDIO_BLOCK_SAMPLES);
      transmit(l, 0);
      transmit(r, 1);
    }
    if (l) release(l);
    if (r) release(r);
  }
};

static DoomAudioStream audio_stream;
static bool audio_connected = false;

// Takes the audio outputs over while DOOM runs, gives them back otherwise
static void SetAudio(bool on) {
  if (on && !audio_connected) {
    OC::AudioIO::ConnectOverride(audio_stream);
    audio_connected = true;
  }
  audio_stream.enabled = on;
  OC::AudioIO::SetOverride(on);
}

}  // namespace ocdoom

extern "C" {

void ocd_audio_lock(void) { AudioNoInterrupts(); }
void ocd_audio_unlock(void) { AudioInterrupts(); }

uint32_t ocd_millis(void) { return millis(); }
void ocd_delay(uint32_t ms) { delay(ms); }

static void *ocd_video_buffer = nullptr;

FLASHMEM void *ocd_zone_alloc(int *size) {
  // Doom's 64 KB frame buffer goes in internal RAM, outside the zone: it is
  // touched every frame, and keeping it out of the zone avoids fragmenting it.
  if (!ocd_video_buffer) ocd_video_buffer = malloc(320 * 200);
  // Doom's zone lives in PSRAM (shared with audio applets, so take 3 MB,
  // or 2 MB if that is all that's left; Freedoom peaks around 1 MB).
  for (int mb = 3; mb >= 2; --mb) {
    *size = mb * 1024 * 1024;
    void *p = extmem_malloc(*size);
    if (p) return p;
  }
  return nullptr;
}

void *ocd_fast_alloc(size_t size) {
  void *p = (size <= 320 * 200) ? ocd_video_buffer : nullptr;
  ocd_video_buffer = nullptr;
  return p;
}

static int ocd_builtin_handle;   // its address marks the built-in WAD

int ocd_file_exists(const char *path) {
  if (ocd_builtin_is(path)) return 1;
  return (SDcard_Ready && SD.exists(path)) ? 1 : 0;
}

FLASHMEM void *ocd_file_open(const char *path, uint32_t *length) {
  if (ocd_builtin_is(path)) {
    *length = ocd_builtin_size();
    return &ocd_builtin_handle;
  }
  if (!SDcard_Ready) return nullptr;
  for (auto &f : ocdoom::wad_files) {
    if (!f) {
      f = SD.open(path, FILE_READ);
      if (!f) return nullptr;
      *length = f.size();
      return &f;
    }
  }
  return nullptr;
}

size_t ocd_file_read(void *handle, uint32_t offset, void *buffer, size_t len) {
  if (handle == &ocd_builtin_handle) return ocd_builtin_read(offset, buffer, len);
  File *f = static_cast<File *>(handle);
  if (!f->seek(offset)) return 0;
  int n = f->read(buffer, len);
  return n < 0 ? 0 : (size_t)n;
}

void ocd_file_close(void *handle) {
  if (handle == &ocd_builtin_handle) return;
  static_cast<File *>(handle)->close();
}

}  // extern "C"

OC_APP_CLASS(AppDoom, TWOCCS("DM"), "DOOM", "Doom"),
  public HSApplication {
public:
  OC_APP_INTERFACE_DECLARE(AppDoom, 0);

  enum State { IDLE, LOADING, RUNNING, NO_PSRAM, NO_CARD, NO_WAD, FAILED };

  void Start() {}
  void Resume() {}

  // ---- ISR side (Controller runs at 16.7 kHz): read gates/CV, write outputs
  void Controller() {
    for (int ch = 0; ch < 4; ++ch) gate_in[ch] = Gate(ch);
    if (Clock(2)) weapon_trig = true;
    cv_turn = DetentedIn(0);
    cv_move = DetentedIn(1);

    GateOut(0, out_firing);
    if (out_hurt_trig) { ClockOut(1); out_hurt_trig = false; }
    Out(2, Proportion(constrain(out_health, 0, 100), 100, HSAPPLICATION_5V));
    Out(3, Proportion(constrain(out_ammo, 0, 200), 200, HSAPPLICATION_5V));
  }

  // ---- Main loop side
  FLASHMEM void DoLoop() {
    switch (state) {
      case LOADING:
        if (!loading_drawn) return;   // let "Loading" reach the screen first
        StartDoom();
        break;
      case RUNNING:
        RunTics();
        break;
      default:
        break;
    }
  }

  FLASHMEM void View() const {
    switch (state) {
      case RUNNING: {
        const uint8_t *fb = ocd_frame();
        for (int page = 0; page < 8; ++page)
          for (int x = 0; x < 128; ++x)
            graphics.drawAlignedByte(x, page * 8, fb[page * 128 + x]);
        if (adjust_msg_until > millis()) {
          graphics.clearRect(0, 0, 128, 9);
          gfxPrint(1, 1, adjust_msg);
          gfxInvert(0, 0, 128, 9);
        }
        break;
      }
      case LOADING:
        gfxPrint(46, 20, "DOOM");
        gfxPrint(16, 36, "Loading WAD...");
        gfxPrint(16, 50, "PSRAM: ");
        gfxPrint(ocdoom::psram_mb());
        gfxPrint(" MB");
        loading_drawn = true;
        break;
      case NO_PSRAM:
        gfxPrint(0, 10, "DOOM needs PSRAM");
        gfxPrint(0, 26, "Solder an 8 MB PSRAM");
        gfxPrint(0, 36, "chip (APS6404L) to");
        gfxPrint(0, 46, "the Teensy 4.1.");
        break;
      case NO_CARD:
        gfxPrint(0, 10, "DOOM: no SD card");
        gfxPrint(0, 26, "Put a FAT32 microSD");
        gfxPrint(0, 36, "with doom1.wad in");
        gfxPrint(0, 46, "the Teensy, reboot.");
        break;
      case NO_WAD:
        gfxPrint(0, 10, "DOOM: no WAD found");
        gfxPrint(0, 26, "Copy doom1.wad (or");
        gfxPrint(0, 36, "freedoom1.wad) to SD");
        gfxPrint(0, 46, "card root, reboot.");
        break;
      case FAILED:
        gfxPrint(0, 0, "DOOM stopped:");
        PrintWrapped(ocd_error(), 12);
        gfxPrint(0, 55, "Reboot to retry");
        break;
      case IDLE:
      default:
        gfxPrint(46, 28, "DOOM");
        break;
    }
  }

  void OnResume() {
    OC::ui.preempt_screensaver(true);
    if (state == IDLE) Launch();
    if (state == RUNNING) ocdoom::SetAudio(true);
  }
  void OnSuspend() {
    OC::ui.preempt_screensaver(false);
    ReleaseAll();
    ocdoom::SetAudio(false);
  }

  void OnButton(const UI::Event &event) {
    if (state != RUNNING) return;
    const bool menu = ocd_menu_active();
    const bool down = event.type == UI::EVENT_BUTTON_DOWN;
    const bool release = event.type == UI::EVENT_BUTTON_RELEASE ||
                         event.type == UI::EVENT_BUTTON_LONG_RELEASE;
    const bool long_press = event.type == UI::EVENT_BUTTON_LONG_PRESS;

    switch (event.control) {
      case OC::CONTROL_BUTTON_UP:   // fire / select
        if (down) {
          if (menu) Tap(KEY_ENTER), Tap('y');
          else SetKey(K_FIRE, true);
        }
        if (release) SetKey(K_FIRE, false);
        break;
      case OC::CONTROL_BUTTON_DOWN: // use / back
        if (down) {
          b_held = true;
          b_adjusted = false;
          if (menu) Tap(KEY_BACKSPACE);
        }
        if (release) {
          b_held = false;
          // tap "use" on release so B can also act as a modifier for adjustments
          if (!menu && !b_adjusted) { SetKey(K_USE, true); use_release_at = millis() + 120; }
        }
        break;
      case OC::CONTROL_BUTTON_L:
        if (event.type == UI::EVENT_BUTTON_RELEASE && !menu) {
          const int k = ocd_next_weapon_key();
          if (k) Tap(k);
        }
        if (long_press) Tap(KEY_TAB);   // automap
        break;
      case OC::CONTROL_BUTTON_R:
        if (event.type == UI::EVENT_BUTTON_RELEASE) SetThrottle(0);
        if (long_press) { SetThrottle(0); Tap(KEY_ESCAPE); }
        break;
#if defined(ARDUINO_TEENSY41)
      case OC::CONTROL_BUTTON_X:
        if (down) SetKey(K_STRAFE_L, true);
        if (release) SetKey(K_STRAFE_L, false);
        break;
      case OC::CONTROL_BUTTON_Y:
        if (down) SetKey(K_STRAFE_R, true);
        if (release) SetKey(K_STRAFE_R, false);
        break;
#endif
      default:
        break;
    }
  }

  void OnEncoder(const UI::Event &event) {
    if (state != RUNNING) return;
    const int v = event.value;
    if (b_held) {   // DOWN held: display adjustments
      b_adjusted = true;
      if (event.control == OC::CONTROL_ENCODER_L) {
        contrast = constrain(contrast + v, 0, 4);
        ocd_set_contrast(contrast);
        snprintf(adjust_msg, sizeof(adjust_msg), "Contrast %d", contrast);
      } else {
        dither = !dither;
        ocd_set_dither(dither);
        snprintf(adjust_msg, sizeof(adjust_msg), dither ? "Dither: diffusion" : "Dither: ordered");
      }
      adjust_msg_until = millis() + 1200;
      return;
    }
    if (ocd_menu_active()) {
      const int key = event.control == OC::CONTROL_ENCODER_L
                        ? (v > 0 ? KEY_DOWNARROW : KEY_UPARROW)
                        : (v > 0 ? KEY_RIGHTARROW : KEY_LEFTARROW);
      for (int i = 0; i < abs(v); ++i) Tap(key);
      return;
    }
    if (event.control == OC::CONTROL_ENCODER_L) {
      turn_accum += v * kTurnPerDetent;
    } else {
      SetThrottle(throttle + v);
    }
  }

private:
  static constexpr int kTurnPerDetent = 320;  // mouse units; ~14 degrees per detent
  static constexpr uint32_t kTicMs = 1000 / 35;

  enum HeldKey { K_FIRE, K_USE, K_FWD, K_BACK, K_RUN, K_STRAFE_L, K_STRAFE_R, K_COUNT };
  static constexpr int kKeyCodes[K_COUNT] = {
    KEY_FIRE, KEY_USE, KEY_UPARROW, KEY_DOWNARROW, KEY_RSHIFT, KEY_STRAFE_L, KEY_STRAFE_R};

  State state = IDLE;
  mutable bool loading_drawn = false;
  uint32_t next_tic_ms = 0;
  uint32_t use_release_at = 0;
  bool key_down[K_COUNT] = {};
  bool gate_key_down[K_COUNT] = {};
  int throttle = 0;          // -2..2
  int turn_accum = 0;
  bool b_held = false, b_adjusted = false;
  int contrast = 2;
  bool dither = false;
  char adjust_msg[24] = "";
  uint32_t adjust_msg_until = 0;
  bool was_menu = false;
  int last_damage = 0;

  // ISR <-> loop shared state
  volatile bool gate_in[4] = {};
  volatile bool weapon_trig = false;
  volatile int cv_turn = 0, cv_move = 0;
  volatile bool out_firing = false, out_hurt_trig = false;
  volatile int out_health = 0, out_ammo = 0;

  void Launch() {
    // Doom's working memory is linked into PSRAM: never touch it without one
    if (ocdoom::psram_mb() == 0) { state = NO_PSRAM; return; }
    if (!SDcard_Ready && !ocd_builtin_path()) { state = NO_CARD; return; }
    state = LOADING;
    loading_drawn = false;
  }

  void StartDoom() {
    const char *wad = nullptr;
    // A WAD on the SD card wins over one built into flash
    if (SDcard_Ready)
      for (const char *name : ocdoom::kWadNames)
        if (SD.exists(name)) { wad = name; break; }
    if (!wad) wad = ocd_builtin_path();
    if (!wad) { state = NO_WAD; return; }
    ocd_audio_set_rate((int)(AUDIO_SAMPLE_RATE_EXACT + 0.5f));
    if (ocd_start(wad) != 0) { state = FAILED; return; }
    ocd_set_contrast(contrast);
    ocd_set_dither(dither);
    state = RUNNING;
    ocdoom::SetAudio(true);
    next_tic_ms = millis();
  }

  void RunTics() {
    const uint32_t now = millis();
    if ((int32_t)(now - next_tic_ms) < 0) return;
    next_tic_ms += kTicMs;
    if ((int32_t)(now - next_tic_ms) > 200) next_tic_ms = now;   // don't spiral after a stall

    ApplyInputs();
    if (ocd_tick() != 0) {
      state = FAILED;
      ocdoom::SetAudio(false);
      return;
    }
    UpdateOutputs();
  }

  void ApplyInputs() {
    const bool menu = ocd_menu_active();
    if (menu && !was_menu) ReleaseAll();   // never hold movement keys into a menu
    was_menu = menu;

    if (use_release_at && (int32_t)(millis() - use_release_at) >= 0) {
      SetKey(K_USE, false);
      use_release_at = 0;
    }

    // gate inputs behave like held keys
    SetGateKey(K_FIRE, gate_in[0] && !menu);
    SetGateKey(K_USE, gate_in[1] && !menu);
    SetGateKey(K_FWD, gate_in[3] && !menu);
    if (weapon_trig) {
      weapon_trig = false;
      if (!menu) { const int k = ocd_next_weapon_key(); if (k) Tap(k); }
    }

    if (!menu) {
      // CV1 turns, CV2 walks (bipolar, +-5 V full scale)
      const int turn = turn_accum + Proportion(cv_turn, HSAPPLICATION_5V, 200);
      const int move = Proportion(cv_move, HSAPPLICATION_5V, 60);
      if (turn || move) ocd_mouse(turn, move);
    }
    turn_accum = 0;
  }

  void UpdateOutputs() {
    const bool playing = ocd_in_level();
    out_firing = playing && ocd_player_attacking();
    const int dmg = ocd_player_damage();
    if (playing && dmg > last_damage) out_hurt_trig = true;
    last_damage = dmg;
    out_health = playing ? ocd_player_health() : 0;
    out_ammo = playing ? ocd_player_ammo() : 0;
  }

  void SetThrottle(int t) {
    throttle = constrain(t, -2, 2);
    SetKey(K_FWD, throttle > 0);
    SetKey(K_BACK, throttle < 0);
    SetKey(K_RUN, abs(throttle) == 2);
    snprintf(adjust_msg, sizeof(adjust_msg), "%s",
             throttle == 2 ? "Run >>" : throttle == 1 ? "Walk >" : throttle == 0 ? "Stop" :
             throttle == -1 ? "< Back" : "<< Back fast");
    adjust_msg_until = millis() + 600;
  }

  void SetKey(HeldKey k, bool down) {
    if (key_down[k] == down) return;
    key_down[k] = down;
    if (!gate_key_down[k]) ocd_key(down, kKeyCodes[k]);
  }
  void SetGateKey(HeldKey k, bool down) {
    if (gate_key_down[k] == down) return;
    gate_key_down[k] = down;
    if (!key_down[k]) ocd_key(down, kKeyCodes[k]);
  }
  void Tap(int key) {
    ocd_key(1, key);
    ocd_key(0, key);
  }
  void ReleaseAll() {
    throttle = 0;
    for (int k = 0; k < K_COUNT; ++k) {
      if (key_down[k] || gate_key_down[k]) ocd_key(0, kKeyCodes[k]);
      key_down[k] = gate_key_down[k] = false;
    }
  }

  void PrintWrapped(const char *s, int y) const {
    char line[22];
    while (*s && y < 52) {
      int n = 0;
      while (s[n] && n < 21) n++;
      memcpy(line, s, n);
      line[n] = 0;
      gfxPrint(0, y, line);
      s += n;
      y += 10;
    }
  }
};

constexpr int AppDoom::kKeyCodes[AppDoom::K_COUNT];

FLASHMEM void AppDoom::Init() { BaseStart(); }

size_t AppDoom::SaveAppData(util::StreamBufferWriter &) const { return 0; }
size_t AppDoom::RestoreAppData(util::StreamBufferReader &) { return 0; }

void AppDoom::Process(OC::IOFrame *ioframe) { BaseController(ioframe); }

FLASHMEM void AppDoom::HandleAppEvent(OC::AppEvent event) {
  switch (event) {
    case OC::APP_EVENT_RESUME: OnResume(); break;
    case OC::APP_EVENT_SUSPEND: OnSuspend(); break;
    default: break;
  }
}

FLASHMEM void AppDoom::Loop() { DoLoop(); }

FLASHMEM void AppDoom::GetIOConfig(OC::IOConfig &ioconfig) const {
  ioconfig.digital_inputs[0].set("Fire");
  ioconfig.digital_inputs[1].set("Use");
  ioconfig.digital_inputs[2].set("Next weapon");
  ioconfig.digital_inputs[3].set("Walk");
  ioconfig.cv[0].set("Turn");
  ioconfig.cv[1].set("Walk/back");
  ioconfig.outputs[0].set("Firing gate", OC::OUTPUT_MODE_GATE);
  ioconfig.outputs[1].set("Hurt trig", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[2].set("Health", OC::OUTPUT_MODE_UNI);
  ioconfig.outputs[3].set("Ammo", OC::OUTPUT_MODE_UNI);
}

void AppDoom::DrawDebugInfo() const {}

FLASHMEM void AppDoom::DrawMenu() const { BaseView(); }
FLASHMEM void AppDoom::DrawScreensaver() const { BaseView(); }

FLASHMEM void AppDoom::HandleButtonEvent(const UI::Event &event) { OnButton(event); }
FLASHMEM void AppDoom::HandleEncoderEvent(const UI::Event &event) { OnEncoder(event); }
