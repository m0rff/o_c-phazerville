# doomgeneric for Ornament & Crime (Teensy 4.1)

Based on [doomgeneric](https://github.com/ozkl/doomgeneric) commit e257a09
(itself based on Chocolate Doom). GPL-2.0-or-later, see LICENSE.

Changes for the O_C, all behind `#ifdef OC_DOOM`:

- `oc_doom_glue.c`: platform layer. Reduces each 320x200 frame to the
  128x64 1-bit OLED (box filter, contrast curve, ordered or error-diffusion
  dithering) with a small health/armor/ammo HUD; reads WADs from the SD card
  via callbacks in `src/apps/Doom.h`; turns `exit()`/`I_Error()` into a
  `longjmp` back to the app.
- `oc_doom_sound.c`: sound module. Mixes the WAD's sound effects and plays
  music (MIDI, or MUS converted with `mus2mid`) on a 2-operator FM synth
  driven by the GENMIDI patches. `ocd_audio_render()` is called from the
  app's audio stream; `FEATURE_SOUND` is on.
- `i_sound.c`: no SDL_mixer include.
- `i_system.c`: zone memory comes from `ocd_zone_alloc()` (PSRAM when fitted).
- `i_video.c`: no RGB framebuffer copy; frame buffer kept in internal RAM.
- `st_stuff.c`: status bar graphics are not loaded or drawn.
- `m_menu.c`: full-height view (screenblocks 11).
- `d_main.c`: no screen-melt wipe (it needs ~250 KB of temporary memory).
- `net_defs.h`: BACKUPTICS 32 (single player only).
- `m_misc.c`: file-exists check goes to the SD card.

Doom's code and constant tables are linked into flash, and its variables
into PSRAM (`.doom.data` / `.doom.bss` in `slot0.ld` / `slot1.ld`; the glue
copies and clears them in `ocd_start()`), leaving ITCM/DTCM for Phazerville.
