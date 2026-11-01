# doomgeneric for Ornament & Crime (Teensy 4.1)

Based on [doomgeneric](https://github.com/ozkl/doomgeneric) commit e257a09
(itself based on Chocolate Doom). GPL-2.0-or-later, see LICENSE.

Changes for the O_C, all behind `#ifdef OC_DOOM`:

- `oc_doom_glue.c`: platform layer. Reduces each 320x200 frame to the
  128x64 1-bit OLED (box filter, contrast curve, ordered or error-diffusion
  dithering) with a small health/armor/ammo HUD; reads WADs from the SD card
  via callbacks in `src/apps/Doom.h`; turns `exit()`/`I_Error()` into a
  `longjmp` back to the app.
- `i_system.c`: zone memory comes from `ocd_zone_alloc()` (PSRAM when fitted).
- `i_video.c`: no RGB framebuffer copy; frame buffer kept in internal RAM.
- `st_stuff.c`: status bar graphics are not loaded or drawn.
- `m_menu.c`: full-height view (screenblocks 11).
- `d_main.c`: no screen-melt wipe (it needs ~250 KB of temporary memory).
- `net_defs.h`: BACKUPTICS 32 (single player only).
- `m_misc.c`: file-exists check goes to the SD card.

Doom's code and constant tables are linked into flash (see
`slot0_doom.ld` / `slot2_doom.ld`), leaving ITCM/DTCM for Phazerville.
