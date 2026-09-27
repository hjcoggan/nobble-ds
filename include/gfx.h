// Both DS screens: a full-colour 256x192 picture (BG3), an 8x8 text layer on
// top of it (BG0) and 128 sprites each. The bottom screen is the touch screen.
#ifndef GFX_H
#define GFX_H

#include <nds.h>
#include <stdint.h>

enum { TOP, BOT };

// text styles, matching FONT_STYLES in tools/gen_assets.py
#define TXT_PLAIN 0
#define TXT_PANEL 1
#define TXT_HILITE 2
#define TXT_GOLD 3
#define TXT_DIM 4
#define TXT_LCD 5          // teal, for labels on the dashboard screens

#define COLS 32
#define ROWS 24

void gfx_init(void);
void gfx_picture(int scr, const void *pixels);      // 256x192 RGB15 bitmap

void text_clear(int scr);
void text_clear_rows(int scr, int y0, int y1);      // rows y0..y1-1
void text_fill(int scr, int x, int y, int w, int h, int tile);
void text_style(int scr, int x, int y, const char *s, int style);
void text_center(int scr, int y, const char *s, int style);
void text_center_in(int scr, int x, int w, int y, const char *s, int style);
void text_num(int scr, int x, int y, int v, int width, int style);   // right-aligned
void big_number(int scr, int x, int y, int v, int digits);           // 16px digits, right-aligned
void progress_bar(int scr, int x, int y, int tiles, int num, int den, int gold);
void panel(int scr, int x, int y, int w, int h);
void text_scroll(int scr, int y);                   // vertical scroll of the text layer

char *put_str(char *p, const char *s);
char *put_num(char *p, int v);

void fade(int to_black);        // both screens, over a few frames
void dim(int scr, int level);   // darken the picture and sprites (not the text), 0-16
void dim_picture(int scr, int level);   // darken only the picture, so sprites stay bright
void shake(int dx, int dy);     // nudge the bottom picture

// sprites: tile is a 32-byte tile index into the shared sprite tile set
void spr(int scr, int id, int x, int y, SpriteSize size, int tile, int pal, int prio);
void spr_hide_all(void);
volatile uint32_t *spr_tiles(int scr, int tile);    // for drawing into sprite VRAM

void gfx_frame(void);           // wait for vblank and show this frame's sprites

#endif
