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

## 1. Check for PSRAM

On an O.R.N.8: hold **Z** while powering on, press **Y** (HW Debug) and look
for the PSRAM line. Otherwise look at the underside of the Teensy 4.1 for a
chip on one of the two 8-pin pads (APS6404L, 8 MB).

## 2. Prepare the SD card

FAT32 microSD in the Teensy 4.1 slot, with one IWAD in the root folder:
`freedoom1.wad` (free, freedoom.github.io), `doom1.wad` (shareware),
`doom.wad`/`doomu.wad` (registered/Ultimate), `doom2.wad` or `freedoom2.wad`.
If several are present, `doom.wad`/`doomu.wad` win, then `freedoom1.wad`,
then `doom1.wad`.

The card can stay in the module: hold **Z** while powering on and press
**X** (MTP). The SD card then shows up on the computer as an MTP device
(Windows Explorer; on macOS use OpenMTP or Android File Transfer). Copy the
WAD, then power-cycle back into Phazerville.

### Or: build the WAD into the firmware

Put the WAD file in `software/lib_doom/` (for example
`software/lib_doom/DOOM1.WAD`) before building. The multiboot build then packs
it (compressed, in 32 KB chunks) into the free flash above the boot slots,
at 0x60400000, and DOOM starts without an SD card. About 3.25 MB is free
there: `doom1.wad` (4.0 MB) packs to 1.8 MB; the full Freedoom WADs are too
big. A WAD on the SD card still wins over the built-in one. `*.wad` files in
`lib_doom/` are ignored by git.

The shareware `doom1.wad` may only be passed on unmodified, so keep a
firmware .hex with it built in to yourself.

## 3. Build and flash

    git clone -b doom-teensy41 --recurse-submodules https://github.com/m0rff/o_c-phazerville
    cd o_c-phazerville/software
    pio run -e T41 -e T41_audio -e T41_MTP

This produces the multiboot image `software/o_C-phazerville-<version>-<rev>.hex`
(all boot slots; DOOM is in A and B). Flash it like any Phazerville update:
Teensy Loader, **File > Open HEX File**, press the Teensy's button, wait for
"Reboot OK". Most O_C builds cut the Teensy's USB power trace, so keep the
rack powered while flashing.

To update later: `git pull`, then build and flash again. To go back, flash an
official Phazerville `_T41` release.

## 4. First start

1. Open the app list and pick **DOOM**.
2. "Loading WAD..." and the PSRAM size show for a few seconds.
3. The title screen and demos play. **UP** opens the menu; pick New Game,
   episode and skill with the left encoder and **UP**. **DOWN** goes back.
4. To leave, hold **UP** and push the right encoder for Phazerville's menu.

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

## Sound

While DOOM runs in the foreground it takes over the audio outputs (and USB
audio in the A slot): sound effects and music replace the audio applets, which
come back when you leave the app. Volume is set in Doom's own Options menu.

Music is played by a small built-in FM synth using the WAD's GENMIDI
instrument data (the patches the original OPL sound cards used). It is
close to the original but not an exact OPL emulation.

## Troubleshooting

| Screen says | Meaning |
|---|---|
| DOOM: no SD card | Card missing or not FAT32. Insert it and power-cycle. |
| DOOM: no WAD found | No supported file name in the card's root folder. |
| DOOM needs PSRAM | No PSRAM chip was found. |
| DOOM stopped: (text) | Doom hit an error. Power-cycle, and report the text. |
