#include "gfx.h"
#include "assets.h"

// Per screen VRAM (bank A bottom, bank C top), both laid out the same:
//   0KB   font tiles
//   28KB  text map (32x32)
//   32KB  the 256x192 picture, 16 bits per pixel
#define MAP_BASE 14         // 2KB units
#define BMP_BASE 2          // 16KB units
#define FONT_PAL_BANK 1

static int bg_text[2], bg_pic[2];
static uint16_t *text_map[2];
static uint16_t *pic[2];

void gfx_init(void)
{
    powerOn(POWER_ALL_2D);
    lcdMainOnBottom();                          // main engine drives the touch screen
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankB(VRAM_B_MAIN_SPRITE);
    vramSetBankC(VRAM_C_SUB_BG);
    vramSetBankD(VRAM_D_SUB_SPRITE);
    setBrightness(3, -16);

    bg_pic[BOT] = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, BMP_BASE, 0);
    bg_text[BOT] = bgInit(0, BgType_Text4bpp, BgSize_T_256x256, MAP_BASE, 0);
    bg_pic[TOP] = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, BMP_BASE, 0);
    bg_text[TOP] = bgInitSub(0, BgType_Text4bpp, BgSize_T_256x256, MAP_BASE, 0);
    for (int s = 0; s < 2; s++) {
        bgSetPriority(bg_pic[s], 3);
        bgSetPriority(bg_text[s], 0);
        text_map[s] = bgGetMapPtr(bg_text[s]);
        pic[s] = bgGetGfxPtr(bg_pic[s]);
        dmaCopy(font_tiles, bgGetGfxPtr(bg_text[s]), sizeof(font_tiles));
        text_clear(s);
    }
    dmaCopy(font_pal, BG_PALETTE + FONT_PAL_BANK * 16, sizeof(font_pal));
    dmaCopy(font_pal, BG_PALETTE_SUB + FONT_PAL_BANK * 16, sizeof(font_pal));

    oamInit(&oamMain, SpriteMapping_1D_128, false);
    oamInit(&oamSub, SpriteMapping_1D_128, false);
    dmaCopy(obj_pal, SPRITE_PALETTE, sizeof(obj_pal));
    dmaCopy(obj_pal, SPRITE_PALETTE_SUB, sizeof(obj_pal));
    dmaCopy(obj_tiles, SPRITE_GFX, sizeof(obj_tiles));
    dmaCopy(obj_tiles, SPRITE_GFX_SUB, sizeof(obj_tiles));
    bgUpdate();
}

void gfx_picture(int scr, const void *pixels)
{
    dmaCopy(pixels, pic[scr], 256 * 192 * 2);
}

// ---------------------------------------------------------------- text

static uint16_t cell(int tile) { return tile | (FONT_PAL_BANK << 12); }

void text_fill(int scr, int x, int y, int w, int h, int tile)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < 32) text_map[scr][(j & 31) * 32 + i] = cell(tile);
}

void text_clear(int scr) { text_fill(scr, 0, 0, 32, 32, 0); }

void text_clear_rows(int scr, int y0, int y1) { text_fill(scr, 0, y0, 32, y1 - y0, 0); }

static int glyph(char ch)
{
    for (const char *f = FONT_CHARS; *f; f++)
        if (*f == ch) return f - FONT_CHARS;
    return 0;
}

void text_style(int scr, int x, int y, const char *s, int style)
{
    for (; *s; s++, x++)
        if (x >= 0 && x < 32) text_map[scr][(y & 31) * 32 + x] = cell(style * FONT_NCHARS + glyph(*s));
}

static int len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

void text_center_in(int scr, int x, int w, int y, const char *s, int style)
{
    text_style(scr, x + (w - len(s)) / 2, y, s, style);
}

void text_center(int scr, int y, const char *s, int style) { text_center_in(scr, 0, 32, y, s, style); }

char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

char *put_num(char *p, int v)
{
    char tmp[12];
    int n = 0;
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

void text_num(int scr, int x, int y, int v, int width, int style)
{
    char buf[12];
    int n = put_num(buf, v) - buf;
    for (int i = 0; i < width - n; i++) text_style(scr, x + i, y, " ", style);
    text_style(scr, x + width - n, y, buf, style);
}

void big_number(int scr, int x, int y, int v, int digits)
{
    int d[8], n = 0;
    do {
        d[n++] = v % 10;
        v /= 10;
    } while (v && n < 8);
    for (int i = 0; i < digits; i++) {
        int cx = x + (digits - 1 - i) * 2;
        if (i < n) {
            int t = BIGDIGIT_TILE(d[i]);
            text_map[scr][y * 32 + cx] = cell(t);
            text_map[scr][y * 32 + cx + 1] = cell(t + 1);
            text_map[scr][(y + 1) * 32 + cx] = cell(t + 2);
            text_map[scr][(y + 1) * 32 + cx + 1] = cell(t + 3);
        } else {
            text_fill(scr, cx, y, 2, 2, 0);
        }
    }
}

void progress_bar(int scr, int x, int y, int tiles, int num, int den, int gold)
{
    int px = den > 0 ? num * tiles * 8 / den : 0;
    if (px > tiles * 8) px = tiles * 8;
    for (int i = 0; i < tiles; i++) {
        int f = px - i * 8;
        f = f < 0 ? 0 : f > 8 ? 8 : f;
        text_map[scr][y * 32 + x + i] = cell(BAR_TILE(f, gold));
    }
}

void panel(int scr, int x, int y, int w, int h)
{
    int fill = TXT_PANEL * FONT_NCHARS;          // blank char on a panel
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int t = fill;
            int top = j == 0, bot = j == h - 1, left = i == 0, right = i == w - 1;
            if (top) t = FRAME_TILE + (left ? 0 : right ? 2 : 1);
            else if (bot) t = FRAME_TILE + (left ? 5 : right ? 7 : 6);
            else if (left) t = FRAME_TILE + 3;
            else if (right) t = FRAME_TILE + 4;
            text_map[scr][((y + j) & 31) * 32 + x + i] = cell(t);
        }
    }
}

void text_scroll(int scr, int y)
{
    bgSetScroll(bg_text[scr], 0, y);
    bgUpdate();
}

// ---------------------------------------------------------------- effects

void fade(int to_black)
{
    for (int i = 0; i <= 16; i += 2) {
        setBrightness(3, -(to_black ? i : 16 - i));
        gfx_frame();
    }
}

static void darken(int scr, int level, uint16_t layers)
{
    uint16_t cnt = level ? (BLEND_FADE_BLACK | layers) : 0;
    if (scr == BOT) {
        REG_BLDCNT = cnt;
        REG_BLDY = level;
    } else {
        REG_BLDCNT_SUB = cnt;
        REG_BLDY_SUB = level;
    }
}

void dim(int scr, int level) { darken(scr, level, BLEND_SRC_BG3 | BLEND_SRC_SPRITE); }

void dim_picture(int scr, int level) { darken(scr, level, BLEND_SRC_BG3); }

void shake(int dx, int dy)
{
    bgSetScroll(bg_pic[BOT], dx, dy);
    bgUpdate();
}

// ---------------------------------------------------------------- sprites

void spr(int scr, int id, int x, int y, SpriteSize size, int tile, int pal, int prio)
{
    OamState *oam = scr == BOT ? &oamMain : &oamSub;
    u16 *base = scr == BOT ? SPRITE_GFX : SPRITE_GFX_SUB;
    if (x < -64 || x > 256 || y < -64 || y > 192) return;
    oamSet(oam, id, x, y, prio, pal, size, SpriteColorFormat_16Color, base + tile * 16,
           -1, false, false, false, false, false);
}

void spr_hide_all(void)
{
    oamClear(&oamMain, 0, 128);
    oamClear(&oamSub, 0, 128);
}

volatile uint32_t *spr_tiles(int scr, int tile)
{
    return (volatile uint32_t *)((scr == BOT ? SPRITE_GFX : SPRITE_GFX_SUB) + tile * 16);
}

void gfx_frame(void)
{
    swiWaitForVBlank();
    oamUpdate(&oamMain);
    oamUpdate(&oamSub);
}
