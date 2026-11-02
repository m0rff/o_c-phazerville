# DOOM on Ornament & Crime (Teensy 4.1)

Experimental. Doom (doomgeneric) is a regular full-screen app, "DOOM", in
the T41 builds (`T41` and `T41_audio`). Pick it from the app menu like any
other app. Game data is read from the Teensy's microSD card.

**Needs a Teensy 4.1 with a PSRAM chip** (8 MB APS6404L on the underside
pads, fitted on the O.R.N.8). Without PSRAM the app shows a message and
does nothing.

## Memory

- Doom's code and constant tables run from flash (about 340 KB).
- Its variables (`.doom.data` / `.doom.bss`, about 280 KB) are linked into
  PSRAM and set up when the app starts.
- The zone (3 MB) is allocated from PSRAM; the 64 KB frame buffer is the
  only Doom allocation in internal RAM, made when DOOM is first started.
- The app itself adds a few hundred bytes to RAM1.

Built in with `-DENABLE_APP_DOOM` (in `custom_T41_flags`) plus the
`doomgeneric` library from `lib_doom`.

## Getting the WAD onto the card

Either copy it with a card reader, or over USB: reboot into the "X" (MTP)
firmware from the multiboot menu, and the SD card shows up as a drive on
the computer (MTP, on macOS use OpenMTP or Android File Transfer).

## SD card

FAT32 microSD in the Teensy 4.1 slot, with one IWAD in the root:
`freedoom1.wad` (free, freedoom.github.io), `doom1.wad` (shareware),
`doom.wad`/`doomu.wad` (registered/Ultimate), `doom2.wad` or `freedoom2.wad`.

## Controls

| Control | In game | In menus |
|---|---|---|
| Left encoder | turn | up / down |
| Right encoder | throttle: back fast, back, stop, walk, run | left / right |
| Right encoder push | stop; long press opens Doom menu | |
| Left encoder push | next weapon; long press automap | |
| UP (A) | fire | select / yes |
| DOWN (B) | use / open doors | back |
| X / Y (O.R.N.8) | strafe left / right | |
| Hold DOWN + turn left / right encoder | contrast / dithering | |

CV: TR1 fire, TR2 use, TR3 next weapon, TR4 walk forward, CV1 turn, CV2 walk.
Outputs: A firing gate, B hurt trigger, C health 0-5 V, D ammo 0-5 V.
