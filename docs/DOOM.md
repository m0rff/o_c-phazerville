# DOOM on Ornament & Crime (Teensy 4.1)

Experimental. Runs Doom (doomgeneric) as a full-screen app on a Teensy 4.1
O_C. Game data is read from the Teensy's microSD card.

**Needs a Teensy 4.1 with a PSRAM chip** (8 MB APS6404L on the underside
pads). Without PSRAM Doom runs out of memory loading a level.

## Build

    cd software
    pio run -e T41_DOOM_SOLO   # standalone DOOM firmware (any T4.1 O_C)
    pio run -e T41_DOOM        # boot slot 2 image for the multiboot build (O.R.N.8)

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
