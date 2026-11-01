// oc_doom_glue.c - doomgeneric platform layer for Ornament & Crime (Teensy 4.1)
//
// - Doom renders 320x200 8-bit into I_VideoBuffer. Each frame is reduced to
//   the O_C's 128x64 1-bit OLED: box-filtered luminance, a contrast curve,
//   then ordered or error-diffusion dithering. In a level the status bar is
//   cropped and replaced with a 7-pixel HUD (health / armor / ammo).
// - WAD files are read from the Teensy 4.1 SD card through callbacks into
//   the C++ app (oc_wad_file below replaces w_file_stdc.c).
// - Every exit()/I_Error() longjmps back to the app, so a Doom error shows a
//   message instead of hanging the module.

#include <setjmp.h>
#include <string.h>
#include <stdio.h>

#include "oc_doom.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_player.h"
#include "d_items.h"
#include "m_argv.h"
#include "w_file.h"
#include "z_zone.h"
#include "i_video.h"

extern boolean menuactive;
extern boolean messageNeedsInput;
extern byte *I_VideoBuffer;
extern int viewwindowy, viewheight;

void M_FindResponseFile(void);
void D_DoomMain(void);
void doomgeneric_Tick(void);

pixel_t *DG_ScreenBuffer = NULL;   // unused on O_C, Doom draws into I_VideoBuffer

#define OW 128
#define OH 64
#define VIEW_H 168   // 3D view height while the status bar is up
#define HUD_H 7

static jmp_buf ocd_jmp;
static int ocd_jmp_armed = 0;
static int ocd_dead = 0;
static char ocd_errmsg[96] = "";

static uint8_t oled[OW * OH / 8];
static uint8_t pal_luma[256];
static uint8_t curve[256];
static int dither_mode = 0;
static int contrast_level = 2;

static const uint8_t bayer4[4][4] = {
    { 0, 8, 2, 10}, {12, 4, 14, 6}, { 3, 11, 1, 9}, {15, 7, 13, 5}};

static const uint8_t font3x5[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};

// ---------------------------------------------------------------- errors

void ocd_set_error(const char *msg)
{
    strncpy(ocd_errmsg, msg, sizeof(ocd_errmsg) - 1);
    ocd_errmsg[sizeof(ocd_errmsg) - 1] = 0;
}

void ocd_exit(int code)
{
    if (!ocd_errmsg[0])
        snprintf(ocd_errmsg, sizeof(ocd_errmsg), code ? "Doom exited (%d)" : "Doom quit", code);
    ocd_dead = 1;
    if (ocd_jmp_armed)
        longjmp(ocd_jmp, 1);
    for (;;) { }   // unreachable: every Doom entry point is guarded
}

const char *ocd_error(void) { return ocd_errmsg; }

// ---------------------------------------------------------------- display

static void build_curve(void)
{
    // contrast stretch around a dark mid-grey; Doom's palette is very dark
    static const int gain_x10[5] = {15, 20, 25, 30, 38};
    const int mid = 70;
    const int g = gain_x10[contrast_level];
    for (int i = 0; i < 256; i++) {
        int v = (i - mid) * g / 10 + 128;
        curve[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

void ocd_set_dither(int mode) { dither_mode = mode ? 1 : 0; }
void ocd_set_contrast(int level)
{
    contrast_level = level < 0 ? 0 : level > 4 ? 4 : level;
    build_curve();
}

void ocd_set_palette(int index, int r, int g, int b)
{
    pal_luma[index & 255] = (r * 77 + g * 150 + b * 29) >> 8;
}

static inline void px(int x, int y, int on)
{
    uint8_t *p = &oled[(y >> 3) * OW + x];
    if (on) *p |= 1 << (y & 7); else *p &= ~(1 << (y & 7));
}

static void draw_num(int x, int y, int v)
{
    char s[8];
    snprintf(s, sizeof s, "%d", v < 0 ? 0 : v);
    for (char *c = s; *c; c++, x += 4)
        for (int r = 0; r < 5; r++)
            for (int k = 0; k < 3; k++)
                if (x + k < OW) px(x + k, y + r, font3x5[*c - '0'][r] & (4 >> k));
}

static void draw_icon(int x, int y, int which)
{
    static const uint8_t plus[5] = {2,2,7,2,2}, ammo[5] = {2,7,7,7,7}, armor[5] = {7,7,7,7,2};
    const uint8_t *g = which == 0 ? plus : which == 1 ? ammo : armor;
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 3; k++) px(x + k, y + r, g[r] & (4 >> k));
}

void DG_DrawFrame(void)
{
    const int in_level = (gamestate == GS_LEVEL);
    // screenblocks 11: the 3D view is full height; the automap stops at the
    // (undrawn) status bar
    const int src_h = !in_level ? SCREENHEIGHT : automapactive ? VIEW_H : viewwindowy + viewheight;
    const int dst_h = in_level ? OH - HUD_H : OH;
    const int use_max = in_level && automapactive;  // keep thin automap lines visible
    // Two rows of error-diffusion state instead of a full frame buffer (saves DTCM)
    static int16_t err_a[OW + 2], err_b[OW + 2];
    int16_t *err_cur = err_a, *err_next = err_b;
    memset(err_a, 0, sizeof err_a);
    memset(err_b, 0, sizeof err_b);

    memset(oled, 0, sizeof oled);
    for (int y = 0; y < dst_h; y++) {
        const int y0 = y * src_h / dst_h, y1 = (y + 1) * src_h / dst_h;
        for (int x = 0; x < OW; x++) {
            const int x0 = x * SCREENWIDTH / OW, x1 = (x + 1) * SCREENWIDTH / OW;
            unsigned sum = 0, n = 0, mx = 0;
            for (int sy = y0; sy < y1; sy++) {
                const byte *row = I_VideoBuffer + sy * SCREENWIDTH;
                for (int sx = x0; sx < x1; sx++) {
                    const unsigned l = pal_luma[row[sx]];
                    sum += l; n++;
                    if (l > mx) mx = l;
                }
            }
            int on;
            if (use_max) {
                on = mx > 40;
            } else if (dither_mode == 0) {
                on = curve[sum / n] > bayer4[y & 3][x & 3] * 16 + 8;
            } else {
                const int v = curve[sum / n] + err_cur[x + 1];
                on = v > 127;
                const int e = v - (on ? 255 : 0);
                err_cur[x + 2] += e * 7 / 16;
                err_next[x] += e * 3 / 16;
                err_next[x + 1] += e * 5 / 16;
                err_next[x + 2] += e / 16;
            }
            if (on) px(x, y, 1);
        }
        int16_t *t = err_cur; err_cur = err_next; err_next = t;
        memset(err_next, 0, sizeof err_a);
    }

    if (in_level) {
        const player_t *pl = &players[consoleplayer];
        for (int x = 0; x < OW; x++) px(x, OH - HUD_H, 1);
        draw_icon(1, OH - 5, 0);
        draw_num(6, OH - 5, pl->health);
        if (pl->armorpoints) {
            draw_icon(46, OH - 5, 2);
            draw_num(51, OH - 5, pl->armorpoints);
        }
        const int ammo = ocd_player_ammo();
        if (ammo >= 0) {
            draw_icon(OW - 22, OH - 5, 1);
            draw_num(OW - 17, OH - 5, ammo);
        }
    }
}

const uint8_t *ocd_frame(void) { return oled; }

// ---------------------------------------------------------------- timing / input

void DG_Init(void) { build_curve(); }
void DG_SleepMs(uint32_t ms) { ocd_delay(ms); }
uint32_t DG_GetTicksMs(void) { return ocd_millis(); }
int DG_GetKey(int *pressed, unsigned char *key) { return 0; }  // events are posted directly
void DG_SetWindowTitle(const char *title) { (void)title; }

void ocd_key(int pressed, int key)
{
    event_t ev;
    ev.type = pressed ? ev_keydown : ev_keyup;
    ev.data1 = key;
    ev.data2 = key;   // "localized" key for menus / chat
    ev.data3 = 0;
    D_PostEvent(&ev);
}

void ocd_mouse(int dx, int dy)
{
    event_t ev;
    ev.type = ev_mouse;
    ev.data1 = 0;
    ev.data2 = dx;
    ev.data3 = dy;
    D_PostEvent(&ev);
}

int ocd_menu_active(void) { return menuactive || messageNeedsInput; }
int ocd_in_level(void) { return gamestate == GS_LEVEL && !demoplayback; }
int ocd_player_health(void) { return players[consoleplayer].health; }
int ocd_player_attacking(void) { return players[consoleplayer].attackdown; }
int ocd_player_damage(void) { return players[consoleplayer].damagecount; }
int ocd_player_dead(void) { return players[consoleplayer].playerstate == PST_DEAD; }

int ocd_player_ammo(void)
{
    const player_t *pl = &players[consoleplayer];
    const ammotype_t at = weaponinfo[pl->readyweapon].ammo;
    return at == am_noammo ? -1 : pl->ammo[at];
}

int ocd_next_weapon_key(void)
{
    // weapon slots 1..7: fist/chainsaw, pistol, shotgun, chaingun, rocket, plasma, BFG
    static const weapontype_t slot_weapon[7] = {
        wp_fist, wp_pistol, wp_shotgun, wp_chaingun, wp_missile, wp_plasma, wp_bfg};
    const player_t *pl = &players[consoleplayer];
    int cur = 0;
    for (int i = 0; i < 7; i++)
        if (slot_weapon[i] == pl->readyweapon || (i == 0 && pl->readyweapon == wp_chainsaw)) cur = i;
    for (int step = 1; step < 7; step++) {
        const int s = (cur + step) % 7;
        if (s != 0 && pl->weaponowned[slot_weapon[s]])
            return '1' + s;
    }
    return cur == 0 ? 0 : '1';
}

// ---------------------------------------------------------------- WAD on SD

typedef struct {
    wad_file_t wad;
    void *handle;
} oc_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *OC_OpenFile(char *path)
{
    uint32_t len = 0;
    void *h = ocd_file_open(path, &len);
    if (!h) return NULL;
    oc_wad_file_t *f = Z_Malloc(sizeof(oc_wad_file_t), PU_STATIC, 0);
    f->wad.file_class = &stdc_wad_file;
    f->wad.mapped = NULL;
    f->wad.length = len;
    f->handle = h;
    return &f->wad;
}

static void OC_CloseFile(wad_file_t *wad)
{
    oc_wad_file_t *f = (oc_wad_file_t *)wad;
    ocd_file_close(f->handle);
    Z_Free(f);
}

static size_t OC_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t len)
{
    oc_wad_file_t *f = (oc_wad_file_t *)wad;
    return ocd_file_read(f->handle, offset, buffer, len);
}

// Named stdc_wad_file so w_file.c picks it up unchanged.
wad_file_class_t stdc_wad_file = { OC_OpenFile, OC_CloseFile, OC_Read };

// ---------------------------------------------------------------- lifecycle

static char *argv_store[8];

int ocd_start(const char *iwad_path)
{
    static char iwad[64];
    strncpy(iwad, iwad_path, sizeof(iwad) - 1);
    argv_store[0] = "doom";
    argv_store[1] = "-iwad";
    argv_store[2] = iwad;
    argv_store[3] = NULL;

    if (setjmp(ocd_jmp)) {
        ocd_jmp_armed = 0;
        return 1;
    }
    ocd_jmp_armed = 1;
    myargc = 3;
    myargv = argv_store;
    DG_Init();
    D_DoomMain();      // runs startup and the first tick, then returns
    ocd_jmp_armed = 0;
    return ocd_dead;
}

int ocd_tick(void)
{
    if (ocd_dead) return 1;
    if (setjmp(ocd_jmp)) {
        ocd_jmp_armed = 0;
        return 1;
    }
    ocd_jmp_armed = 1;
    doomgeneric_Tick();
    ocd_jmp_armed = 0;
    return 0;
}

// ---------------------------------------------------------------- libc stubs
// Doom's config/savegame code references POSIX file calls; there is no
// filesystem behind newlib on the O_C, so these simply fail.
#include <errno.h>
int mkdir(const char *path, int mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int _open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; errno = ENOSYS; return -1; }
int _unlink(const char *path) { (void)path; errno = ENOSYS; return -1; }
int _link(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
