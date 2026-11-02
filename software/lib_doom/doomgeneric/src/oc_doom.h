// oc_doom.h - glue between doomgeneric and the Ornament & Crime (Phazerville)
// DOOM app. This header is force-included into every Doom source file
// (see library.json), and also included by the C++ app.
#ifndef OC_DOOM_H_
#define OC_DOOM_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Implemented by the glue (C), called by the app -----------------------

// Start Doom with the given IWAD path on the SD card. Returns 0 on success,
// nonzero if Doom bailed out (see ocd_error()).
int ocd_start(const char *iwad_path);
// Run Doom for one frame (one or more 35 Hz tics). Returns 0 while running.
int ocd_tick(void);
// Last fatal error text (empty string if none).
const char *ocd_error(void);
// 128x64 1-bit frame in SSD1306/SH1106 page layout (8 pages x 128 bytes).
const uint8_t *ocd_frame(void);
// Dithering: 0 = ordered (Bayer), 1 = error diffusion (Floyd-Steinberg)
void ocd_set_dither(int mode);
// Contrast 0..4 (2 = default)
void ocd_set_contrast(int level);

// Input. Keys are Doom key codes (doomkeys.h).
void ocd_key(int pressed, int key);
void ocd_mouse(int dx, int dy);

// Game state, for the app's UI and CV outputs
int ocd_menu_active(void);
int ocd_in_level(void);       // 1 while playing a level (not menus/intermission)
int ocd_player_health(void);
int ocd_player_ammo(void);    // ammo of the current weapon, -1 for fist/chainsaw
int ocd_player_attacking(void);
int ocd_player_damage(void);  // damage flash counter
int ocd_player_dead(void);
int ocd_next_weapon_key(void); // key ('1'..'7') for the next owned weapon, 0 if none

// Audio (oc_doom_sound.c). Set the output rate before ocd_start(); render is
// called from the audio interrupt and fills n stereo samples.
void ocd_audio_set_rate(int hz);
void ocd_audio_render(int16_t *left, int16_t *right, int n);

// Built-in WAD packed into program flash (oc_doom_builtin.c, pack_wad.py).
// ocd_builtin_path() returns its path (e.g. "/flash/doom1.wad") or NULL.
const char *ocd_builtin_path(void);
int ocd_builtin_is(const char *path);
uint32_t ocd_builtin_size(void);
size_t ocd_builtin_read(uint32_t offset, void *buffer, size_t len);
void ocd_builtin_set(const void *blob);

// ---- Implemented by the app (C++), called by the glue ---------------------
uint32_t ocd_millis(void);
void ocd_delay(uint32_t ms);
void *ocd_zone_alloc(int *size);
// Internal-RAM allocation for hot buffers; may return NULL (then the zone is used)
void *ocd_fast_alloc(size_t size);
// Keep the audio interrupt out while Doom changes sound/music state
void ocd_audio_lock(void);
void ocd_audio_unlock(void);
int ocd_file_exists(const char *path);
void *ocd_file_open(const char *path, uint32_t *length);
size_t ocd_file_read(void *handle, uint32_t offset, void *buffer, size_t len);
void ocd_file_close(void *handle);

// ---- Used inside the Doom sources -----------------------------------------
void ocd_set_error(const char *msg);
void ocd_set_palette(int index, int r, int g, int b);
void ocd_exit(int code) __attribute__((noreturn));

#ifdef OC_DOOM_INTERNAL
// Every exit() inside Doom unwinds back to the app instead of halting the MCU.
#include <stdlib.h>
#define exit(code) ocd_exit(code)
#endif

#ifdef __cplusplus
}
#endif

#endif // OC_DOOM_H_
