// Nubby DS: the board on the touch screen, the dashboard on the top screen.
#include <nds.h>
#include <stdio.h>
#include "assets.h"
#include "game.h"
#include "gfx.h"
#include "sound.h"
#include "save.h"

#include "title_top_bin.h"
#include "title_bottom_bin.h"
#include "dashboard_bin.h"
#include "board0_bin.h"
#include "board1_bin.h"
#include "board2_bin.h"

static const uint8_t *const boards[NUM_BOARDS] = { board0_bin, board1_bin, board2_bin };

#define PAUSE_DIM 9               // 0-16 darkening behind menus
#define SHOP_DIM 12
#define RESULT_FRAMES 120
#define AIM_DOTS 12
#define NUM_SPARKS 40

// bottom screen sprites (lower ids draw on top)
#define OBJ_NUBBY 0
#define OBJ_PAUSE 1
#define OBJ_POPUP 2               // NUM_POPUPS score popups
#define OBJ_SPARK (OBJ_POPUP + NUM_POPUPS)
#define OBJ_DOT   (OBJ_SPARK + NUM_SPARKS)
#define OBJ_PEG   (OBJ_DOT + AIM_DOTS)
#define OBJ_FX    (OBJ_PEG + NUM_SLOTS)      // laser beam or wind streaks (10)
#define OBJ_UI    (OBJ_FX + 10)              // icons on menus and cards
// top screen sprites
#define TOP_FACE 0
#define TOP_HEART 1               // up to 8
#define TOP_COIN 9
#define TOP_ITEM 10               // MAX_ITEMS
#define TOP_PERK (TOP_ITEM + MAX_ITEMS)
#define TOP_DETAIL (TOP_PERK + MAX_PERKS)

// runtime sprite tiles: each peg's number is drawn onto its own sprite
#define PEG_TILE(i) (TILE_FREE + (i) * PEG_TILES)
#define POPUP_TILE(i) (TILE_FREE + NUM_SLOTS * PEG_TILES + (i) * POPUP_TILES)

// Nubby's face in the dashboard window (see tools/gen_assets.py)
#define FACE_X 180
#define FACE_Y 24

typedef enum {
    ST_TITLE, ST_HOWTO, ST_CREDITS,
    ST_AIM, ST_FLY, ST_RESULT, ST_PERK, ST_SHOP, ST_SWAP, ST_PAUSE, ST_INVENTORY, ST_BOSS, ST_OVER,
} State;

enum { FACE_HAPPY, FACE_BLINK, FACE_WOW, FACE_WORRY };

static Game game;
static State state, paused_from;
static Result last_result;
static int frames, timer, menu_sel, aim, best_launch;
static int inv_sel, swap_for = -1, shop_sel;
static int credits_scroll, credits_rows;
static int face, face_timer, shake_timer;
static int32_t shown[NUM_SLOTS];      // value currently drawn on each peg sprite
static int32_t prev_pegs[NUM_SLOTS];  // to spot pops and laser hits
static uint32_t seed = 0x5EED1234;

// input for this frame
static uint32_t pressed, held;
static int touching, tapped, released, tx, ty;
static int aiming_by_touch;

static const char *const trigger_short[NUM_TRIGGERS] = {
    "ALWAYS", "ON LAUNCH", "FIRST POP", "PEG GONE", "WALL HIT", "FALLS OUT", "EVERY 8",
};
static const char *const perk_short[NUM_PERKS] = {
    "EVERY 3 SEC", "EVERY 1 SEC", "FIRST POP", "FALLS OUT", "WALL HIT", "GOAL MET", "TOP PEG HIT", "15 POPS",
};

// ---------------------------------------------------------------- input

static void read_input(void)
{
    scanKeys();
    pressed = keysDown();
    held = keysHeld();
    uint32_t up = keysUp();
    touching = (held & KEY_TOUCH) != 0;
    tapped = (pressed & KEY_TOUCH) != 0;
    released = (up & KEY_TOUCH) != 0;
    if (touching) {
        touchPosition tp;
        touchRead(&tp);
        tx = tp.px;
        ty = tp.py;
    }
    seed = seed * 1664525u + 1013904223u + held + tx * 7 + ty;
}

static int tapped_in(int x, int y, int w, int h)    // pixels
{
    return tapped && tx >= x && tx < x + w && ty >= y && ty < y + h;
}

// ---------------------------------------------------------------- buttons

typedef struct {
    int x, y, w, h;         // tiles
    const char *label;
} Button;

static void draw_buttons(const Button *b, int n, int sel)
{
    for (int i = 0; i < n; i++) {
        panel(BOT, b[i].x, b[i].y, b[i].w, b[i].h);
        text_center_in(BOT, b[i].x, b[i].w, b[i].y + b[i].h / 2, b[i].label, i == sel ? TXT_HILITE : TXT_PANEL);
    }
}

// The button tapped this frame, or chosen with the d-pad and A. -1 if none.
static int buttons_update(const Button *b, int n, int *sel)
{
    for (int i = 0; i < n; i++)
        if (tapped_in(b[i].x * 8, b[i].y * 8, b[i].w * 8, b[i].h * 8)) {
            *sel = i;
            return i;
        }
    if (n > 1 && (pressed & KEY_UP)) {
        *sel = (*sel + n - 1) % n;
        sfx_move();
        draw_buttons(b, n, *sel);
    }
    if (n > 1 && (pressed & KEY_DOWN)) {
        *sel = (*sel + 1) % n;
        sfx_move();
        draw_buttons(b, n, *sel);
    }
    return (pressed & KEY_A) ? *sel : -1;
}

// ---------------------------------------------------------------- numbered pegs

// 3x5 digits (bit 14 = top left), then K for thousands
static const uint16_t digits3x5[11] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7252, 0x7BEF, 0x7BCF, 0x5D35,
};

// set pixel (x, y) of a sprite `wt` tiles wide, in a 4bpp buffer
static void spx(uint32_t *buf, int wt, int ht, int x, int y, int c)
{
    if (x < 0 || y < 0 || x >= wt * 8 || y >= ht * 8) return;
    int word = ((y >> 3) * wt + (x >> 3)) * 8 + (y & 7);
    int shift = (x & 7) * 4;
    buf[word] = (buf[word] & ~(0xFu << shift)) | ((uint32_t)c << shift);
}

static int digits_of(int32_t v, int *out)
{
    int tmp[8], m = 0, n = 0;
    do {
        tmp[m++] = v % 10;
        v /= 10;
    } while (v && m < 8);
    while (m) out[n++] = tmp[--m];
    return n;
}

static void copy_to_vram(int tile, const uint32_t *buf, int words)
{
    volatile uint32_t *dst = spr_tiles(BOT, tile);
    for (int w = 0; w < words; w++) dst[w] = buf[w];
}

// Redraw slot i's 32x32 sprite: the disc template with the number on top.
static void render_peg(int i, int32_t v)
{
    uint32_t buf[PEG_TILES * 8];
    for (int w = 0; w < PEG_TILES * 8; w++) buf[w] = obj_tiles[TILE_PEG * 8 + w];
    int d[8], n;
    if (v < 1000) {                             // big 5x7 digits
        n = digits_of(v, d);
        int x0 = 16 - (n * 6 - 1) / 2;
        for (int g = 0; g < n; g++)
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (glyphs5x7[d[g]][r] & (0x10 >> c)) spx(buf, 4, 4, x0 + g * 6 + c, 13 + r, 5);
    } else {                                    // small 3x5 digits, K for thousands
        n = digits_of(v >= 10000 ? v / 1000 : v, d);
        if (v >= 10000) d[n++] = 10;
        int x0 = 16 - (n * 4 - 1) / 2;
        for (int g = 0; g < n; g++)
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 3; c++)
                    if (digits3x5[d[g]] & (1 << (14 - r * 3 - c))) spx(buf, 4, 4, x0 + g * 4 + c, 14 + r, 5);
    }
    copy_to_vram(PEG_TILE(i), buf, PEG_TILES * 8);
}

static int tier(int32_t v)
{
    int t = 0;
    while (v > 1 && t < NUM_TIERS - 1) {
        v >>= 1;
        t++;
    }
    return t;
}

// ---------------------------------------------------------------- effects: sparks and score popups

typedef struct {
    int x, y, vx, vy;       // 8.8 fixed point
    int life, pal;
} Spark;

typedef struct {
    int x, y, life;
} Popup;

static Spark sparks[NUM_SPARKS];
static Popup popups[NUM_POPUPS];
static int next_popup;

static void burst(int x, int y, int pal, int n)
{
    for (int k = 0; k < NUM_SPARKS && n; k++) {
        Spark *s = &sparks[k];
        if (s->life) continue;
        uint32_t r = game_rand(&game);
        s->x = x << 8;
        s->y = y << 8;
        s->vx = (int)(r & 1023) - 512;
        s->vy = (int)((r >> 10) & 511) - 700;
        s->life = 18 + (r >> 20) % 14;
        s->pal = pal;
        n--;
    }
}

// Draw "+v" into the next popup sprite (32x16) and float it up from (x, y).
static void popup(int x, int y, int32_t v)
{
    static const uint8_t plus[7] = { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 };
    uint32_t buf[POPUP_TILES * 8] = { 0 };
    int d[8], n = digits_of(v, d);
    if (n > 4) n = 4;
    int x0 = 16 - ((n + 1) * 6 - 1) / 2;
    for (int pass = 0; pass < 2; pass++)
        for (int g = 0; g <= n; g++) {
            const uint8_t *rows = g == 0 ? plus : glyphs5x7[d[g - 1]];
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++) {
                    if (!(rows[r] & (0x10 >> c))) continue;
                    int px = x0 + g * 6 + c, py = 4 + r;
                    if (pass == 0) {                                 // dark outline first
                        for (int oy = -1; oy <= 1; oy++)
                            for (int ox = -1; ox <= 1; ox++) spx(buf, 4, 2, px + ox, py + oy, 6);
                    } else {
                        spx(buf, 4, 2, px, py, r < 3 ? 13 : 11);
                    }
                }
        }
    int k = next_popup;
    next_popup = (next_popup + 1) % NUM_POPUPS;
    copy_to_vram(POPUP_TILE(k), buf, POPUP_TILES * 8);
    popups[k].x = x - 16;
    popups[k].y = y - 20;
    popups[k].life = 40;
}

static void clear_effects(void)
{
    for (int k = 0; k < NUM_SPARKS; k++) sparks[k].life = 0;
    for (int k = 0; k < NUM_POPUPS; k++) popups[k].life = 0;
}

static void set_face(int f, int t)
{
    face = f;
    face_timer = t;
}

static void sync_board(void)
{
    for (int i = 0; i < NUM_SLOTS; i++) prev_pegs[i] = game.pegs[i];
}

// Compare the board with last frame: sparks and popups for pops, a red
// burst where the laser struck. Returns the x of the last popped peg, or -1.
static int board_effects(void)
{
    int px = -1;
    for (int i = 0; i < NUM_SLOTS; i++) {
        int32_t was = prev_pegs[i], now = game.pegs[i];
        if (now < was) {
            if (game.flash[i] == FLASH_FRAMES) {           // popped
                burst(game.slot[i].x, game.slot[i].y, PAL_TIER(tier(was)), now ? 4 : 9);
                popup(game.slot[i].x, game.slot[i].y, was);
                px = game.slot[i].x;
                if (was >= 32) shake_timer = 4;
            } else if (!now) {                                // lasered away
                burst(game.slot[i].x, game.slot[i].y, PAL_FX, 8);
            }
        }
        prev_pegs[i] = now;
    }
    return px;
}

// ---------------------------------------------------------------- the top screen dashboard

static int on_board(void)
{
    return state == ST_AIM || state == ST_FLY || state == ST_RESULT || state == ST_PAUSE ||
           state == ST_BOSS || state == ST_OVER || state == ST_SHOP || state == ST_SWAP ||
           state == ST_PERK || state == ST_INVENTORY;
}

enum { DETAIL_NONE = -1, DETAIL_ITEM, DETAIL_PERK };
static int detail_kind = DETAIL_NONE, detail_id;   // what the big panel on the top screen shows
static const char *detail_hint = "";

static void show_detail(int kind, int id, const char *hint)
{
    detail_kind = kind;
    detail_id = id;
    detail_hint = hint;
}

static void hud_text(void)
{
    char buf[40], *p;
    text_clear(TOP);
    // header: round and coins (hearts and the coin are sprites)
    p = put_str(buf, "ROUND ");
    put_num(p, game.round);
    text_style(TOP, 1, 1, buf, game.boss ? TXT_GOLD : TXT_PLAIN);
    text_num(TOP, 27, 1, game.coins, 4, TXT_GOLD);

    if (detail_kind != DETAIL_NONE) {
        panel(TOP, 1, 3, 30, 8);
        if (detail_kind == DETAIL_ITEM) {
            const ItemInfo *it = &item_info[detail_id];
            text_style(TOP, 7, 4, it->name, TXT_HILITE);
            p = put_num(buf, it->price);
            put_str(p, " COINS");
            text_style(TOP, 20, 4, buf, game.coins >= it->price ? TXT_PANEL : TXT_DIM);
            text_style(TOP, 7, 6, trigger_text[it->trigger], TXT_PANEL);
            text_style(TOP, 7, 7, it->effect, TXT_HILITE);
        } else {
            const PerkInfo *pk = &perk_info[detail_id];
            text_style(TOP, 7, 4, pk->name, TXT_HILITE);
            text_style(TOP, 7, 6, pk->line1, TXT_PANEL);
            text_style(TOP, 7, 7, pk->line2, TXT_HILITE);
        }
        text_center_in(TOP, 1, 30, 9, detail_hint, TXT_PANEL);
    } else {
        text_style(TOP, 2, 3, "SCORE", TXT_LCD);
        big_number(TOP, 2, 4, game.score, 8);
        p = put_str(buf, "GOAL ");
        put_num(p, game.quota);
        text_style(TOP, 2, 7, buf, TXT_LCD);
        int met = game.score >= game.quota;
        progress_bar(TOP, 2, 8, 18, game.score, game.quota, met);
        if (met) {
            int r = game.score / game.quota;
            p = put_str(buf, "RESTOCKS ");
            put_num(p, r > MAX_RESTOCKS ? MAX_RESTOCKS : r);
            text_style(TOP, 2, 9, buf, TXT_GOLD);
        } else if (state == ST_FLY) {
            p = put_str(buf, "NEED ");
            put_num(p, game.quota - game.score);
            p = put_str(p, " MORE");
            text_style(TOP, 2, 9, buf, TXT_LCD);
        }
    }

    // status line
    if (game.boss) {
        p = put_str(buf, "BOSS: ");
        put_str(p, boss_info[game.boss].name);
        text_center(TOP, 11, buf, TXT_GOLD);
    } else {
        int until = SHOP_EVERY - (game.round - 1) % SHOP_EVERY;
        p = put_str(buf, "NEXT SHOP IN ");
        p = put_num(p, until);
        put_str(p, until == 1 ? " ROUND" : " ROUNDS");
        text_center(TOP, 11, buf, TXT_PLAIN);
    }

    // items and perks windows
    p = put_str(buf, "ITEMS ");
    p = put_num(p, game.nitems);
    p = put_str(p, "/");
    put_num(p, MAX_ITEMS);
    text_style(TOP, 1, 13, game.nitems >= MAX_ITEMS ? "ITEMS FULL" : buf, TXT_LCD);
    for (int i = 0; i < game.nitems; i++) {
        const ItemInfo *it = &item_info[game.items[i]];
        text_style(TOP, 4, 14 + i * 2, it->name, game.item_flash[i] ? TXT_GOLD : TXT_PLAIN);
        text_style(TOP, 4, 15 + i * 2, trigger_short[it->trigger], TXT_LCD);
    }
    text_style(TOP, 16, 13, "PERKS", TXT_LCD);
    if (!game.nperks) text_style(TOP, 16, 15, "ONE EVERY", TXT_LCD);
    if (!game.nperks) text_style(TOP, 16, 16, "5 ROUNDS", TXT_LCD);
    for (int i = 0; i < game.nperks; i++) {
        text_style(TOP, 19, 14 + i * 2, perk_info[game.perks[i]].name, game.perk_flash[i] ? TXT_GOLD : TXT_PLAIN);
        text_style(TOP, 19, 15 + i * 2, perk_short[game.perks[i]], TXT_LCD);
    }
}

static void hud_sprites(void)
{
    // Nubby's face reacts to what's happening
    int f = face;
    if (face_timer) face_timer--;
    else f = FACE_HAPPY;
    if (f == FACE_HAPPY && frames % 200 < 8) f = FACE_BLINK;
    int bob = (frames / 12) % 4 == 3 ? 1 : 0;
    spr(TOP, TOP_FACE, FACE_X, FACE_Y + bob, SpriteSize_64x64, TILE_FACE(f), PAL_UI, 1);

    for (int h = 0; h < game.max_lives && h < 8; h++)
        spr(TOP, TOP_HEART + h, 80 + h * 14, 4, SpriteSize_16x16, h < game.lives ? TILE_HEART : TILE_HEART_EMPTY,
            PAL_UI, 1);
    spr(TOP, TOP_COIN, 200, 4, SpriteSize_16x16, TILE_COIN, PAL_UI, 1);
    for (int i = 0; i < game.nitems; i++)
        spr(TOP, TOP_ITEM + i, 10, 112 + i * 16, SpriteSize_16x16, TILE_ICON(game.items[i]),
            (game.item_flash[i] & 4) ? PAL_ICON_FLASH : PAL_ICON, 1);
    for (int i = 0; i < game.nperks; i++)
        spr(TOP, TOP_PERK + i, 130, 112 + i * 16, SpriteSize_16x16, TILE_PERK(game.perks[i]),
            (game.perk_flash[i] & 4) ? PAL_ICON_FLASH : PAL_ICON, 1);
    if (detail_kind != DETAIL_NONE)
        spr(TOP, TOP_DETAIL, 16, 36, SpriteSize_32x32,
            detail_kind == DETAIL_ITEM ? TILE_BIGICON(detail_id) : TILE_BIGPERK(detail_id), PAL_ICON, 0);
}

// ---------------------------------------------------------------- the bottom screen

static void board_sprites(void)
{
    if (state == ST_SHOP || state == ST_SWAP || state == ST_PERK || state == ST_INVENTORY) return;   // menus cover it
    int blink = (frames % 180) < 8;
    int flying = state == ST_FLY || (state == ST_PAUSE && paused_from == ST_FLY);
    int aiming = state == ST_AIM || state == ST_BOSS || (state == ST_PAUSE && paused_from == ST_AIM);
    int nx = flying ? game.x >> 8 : LAUNCH_X;
    int ny = flying ? game.y >> 8 : LAUNCH_Y;
    if (flying || aiming) {
        int big = game_has(&game, ITEM_BIG);
        int t = big ? (blink ? TILE_NUBBY_BIG_BLINK : TILE_NUBBY_BIG) : (blink ? TILE_NUBBY_BLINK : TILE_NUBBY);
        spr(BOT, OBJ_NUBBY, nx - 8, ny - 8, SpriteSize_16x16, t, PAL_NUBBY, 1);
    }
    if (state == ST_AIM || state == ST_FLY)
        spr(BOT, OBJ_PAUSE, 236, 1, SpriteSize_16x16, TILE_PAUSE, PAL_ICON, 1);

    if (state == ST_AIM) {
        int16_t xs[AIM_DOTS], ys[AIM_DOTS];
        int n = game_predict(&game, aim, xs, ys, AIM_DOTS);
        for (int d = 0; d < n; d++)
            spr(BOT, OBJ_DOT + d, xs[d] - 4, ys[d] - 4, SpriteSize_8x8, TILE_DOT, PAL_NUBBY, 1);
    }

    for (int i = 0; i < game.nslots; i++) {
        int32_t v = game.pegs[i];
        if (!v) {                                  // an empty slot in this layout
            spr(BOT, OBJ_PEG + i, game.slot[i].x - 16, game.slot[i].y - 16, SpriteSize_32x32, TILE_SOCKET,
                PAL_ARMOR, 2);
            continue;
        }
        if (v != shown[i]) {
            render_peg(i, v);
            shown[i] = v;
        }
        int pal = game.flash[i] ? PAL_FLASH : game.armor[i] ? PAL_ARMOR : PAL_TIER(tier(v));
        spr(BOT, OBJ_PEG + i, game.slot[i].x - 16, game.slot[i].y - 16, SpriteSize_32x32, PEG_TILE(i), pal, 1);
    }

    for (int k = 0; k < NUM_SPARKS; k++) {
        Spark *s = &sparks[k];
        if (!s->life) continue;
        s->life--;
        s->x += s->vx;
        s->y += s->vy;
        s->vy += 40;
        spr(BOT, OBJ_SPARK + k, (s->x >> 8) - 4, (s->y >> 8) - 4, SpriteSize_8x8, TILE_SPARK, s->pal, 1);
    }
    for (int k = 0; k < NUM_POPUPS; k++) {
        Popup *p = &popups[k];
        if (!p->life) continue;
        p->life--;
        if (p->life > 12 || (frames & 1))
            spr(BOT, OBJ_POPUP + k, p->x, p->y - (40 - p->life) / 2, SpriteSize_32x16, POPUP_TILE(k), PAL_UI, 1);
    }

    // boss hazards: the laser locks onto a row, blinks a warning, then fires
    if (game.boss == BOSS_LASER && game.laser_y >= 0 && state == ST_FLY) {
        int warn = game.laser_timer < LASER_WARN;
        if (!warn || (frames & 4))
            for (int k = 0; k < 8; k++)
                spr(BOT, OBJ_FX + k, k * 32, game.laser_y - 4, SpriteSize_32x8,
                    warn ? TILE_LASER_WARN : TILE_LASER, PAL_FX, warn ? 1 : 0);
    }
    if (game.boss == BOSS_WIND && (state == ST_AIM || state == ST_FLY || state == ST_BOSS)) {
        for (int k = 0; k < 10; k++) {
            int x = (k * 67 + frames * 3 * game.wind) % 236;
            if (x < 0) x += 236;
            spr(BOT, OBJ_FX + k, BOARD_L + x - 8, 30 + k * 15, SpriteSize_16x8, TILE_WIND, PAL_FX, 2);
        }
    }
}

static void ui_sprites(void);

static void frame(void)
{
    spr_hide_all();
    if (on_board()) {
        hud_sprites();
        board_sprites();
        ui_sprites();
    }
    if (shake_timer) {
        shake_timer--;
        shake(shake_timer ? (int)(game_rand(&game) % 5) - 2 : 0, shake_timer ? (int)(game_rand(&game) % 3) - 1 : 0);
    }
    gfx_frame();
    sound_update();
    frames++;
}

// ---------------------------------------------------------------- saving

static void record_run(void)
{
    int changed = 0;
    if (game.round > save.best_round) {
        save.best_round = game.round;
        changed = 1;
    }
    if (best_launch > save.best_score) {
        save.best_score = best_launch;
        changed = 1;
    }
    if (changed) save_write();
}

// ---------------------------------------------------------------- title, how to play, credits

static const Button title_buttons[] = {
    { 7, 4, 18, 4, "PLAY" }, { 7, 9, 18, 3, "HOW TO PLAY" }, { 7, 13, 18, 3, "CREDITS" },
};
static const Button back_button[] = { { 9, 19, 14, 3, "BACK" } };

static void draw_title(void)
{
    char buf[40], *p;
    text_clear(TOP);
    text_clear(BOT);
    draw_buttons(title_buttons, 3, menu_sel);
    if (save.best_round > 0) {
        panel(BOT, 2, 18, 28, 3);
        p = put_str(buf, "BEST ROUND ");
        p = put_num(p, save.best_round);
        p = put_str(p, "  LAUNCH ");
        put_num(p, save.best_score);
        text_center(BOT, 19, buf, TXT_HILITE);
    } else if (!save_available) {
        text_center(BOT, 19, "NO SD CARD: SCORES WON'T SAVE", TXT_PLAIN);
    }
}

static void go_title(void)
{
    fade(1);
    music_stop();
    dim(TOP, 0);
    dim(BOT, 0);
    shake(0, 0);
    text_scroll(TOP, 0);
    gfx_picture(TOP, title_top_bin);
    gfx_picture(BOT, title_bottom_bin);
    state = ST_TITLE;
    menu_sel = 0;
    draw_title();
    frame();
    fade(0);
    music_play(SONG_FACTORY);
}

static void back_to_title(int sel)
{
    text_scroll(TOP, 0);
    dim(TOP, 0);
    state = ST_TITLE;
    menu_sel = sel;
    draw_title();
}

static void show_howto(void)
{
    static const char *const lines[] = {
        "TOUCH THE BOARD AND DRAG TO",
        "AIM, THEN LET GO TO LAUNCH.",
        "LEFT, RIGHT AND A WORK TOO.",
        "",
        "A HIT SCORES THE PEG'S NUMBER",
        "AND HALVES IT. A 1 VANISHES.",
        "",
        "REACH THE GOAL IN ONE LAUNCH",
        "OR LOSE A LIFE. BEAT IT BY",
        "MORE TO RESTOCK THE BOARD.",
        "",
        "BUY ITEMS, PICK PERKS AND",
        "BEAT A BOSS EVERY 5 ROUNDS!",
    };
    state = ST_HOWTO;
    text_clear(TOP);
    text_clear(BOT);
    dim(TOP, 10);
    panel(TOP, 0, 1, 32, 20);
    text_center(TOP, 2, "HOW TO PLAY", TXT_HILITE);
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        text_style(TOP, 2, 4 + i, lines[i], i >= 7 && i <= 9 ? TXT_HILITE : TXT_PANEL);
    menu_sel = 0;
    draw_buttons(back_button, 1, 0);
}

static const struct { const char *role, *name; } credits[] = {
    { "GAME DIRECTOR", "HEATH" }, { "CREATIVE DIRECTOR", "CLAUDE" },
    { "TECHNICAL DIRECTOR", "CLAUDE" }, { "PRODUCER", "CLAUDE" },
    { "LEAD GAME DESIGNER", "CLAUDE" }, { "SYSTEMS DESIGNER", "CLAUDE" },
    { "PHYSICS PROGRAMMER", "CLAUDE" }, { "GAMEPLAY PROGRAMMER", "CLAUDE" },
    { "ENGINE PROGRAMMER", "CLAUDE" }, { "GRAPHICS PROGRAMMER", "CLAUDE" },
    { "TOUCH CONTROLS", "CLAUDE" }, { "AUDIO PROGRAMMER", "CLAUDE" },
    { "TOOLS PROGRAMMER", "CLAUDE" }, { "UI PROGRAMMER", "CLAUDE" },
    { "BUILD ENGINEER", "CLAUDE" }, { "ART DIRECTOR", "CLAUDE" },
    { "PIXEL ARTIST", "CLAUDE" }, { "CHARACTER ARTIST", "CLAUDE" },
    { "UI ARTIST", "CLAUDE" }, { "COMPOSER", "CLAUDE" },
    { "SOUND DESIGNER", "CLAUDE" }, { "QA LEAD", "CLAUDE" },
    { "QA TESTER", "HEATH" }, { "BALANCE TESTER", "HEATH" },
    { "ECONOMY DESIGNER", "CLAUDE" }, { "PEG ENGINEER", "CLAUDE" },
    { "NUBBY WRANGLER", "CLAUDE" },
};
#define NUM_ROLES ((int)(sizeof(credits) / sizeof(credits[0])))
#define CREDITS_LEAD 24           // blank rows so the list starts below the screen
#define CREDITS_HEAD 4            // "NUBBY DS", blank, "CREDITS", blank
#define CREDITS_TAIL (CREDITS_LEAD + CREDITS_HEAD + NUM_ROLES * 3 + 1)
#define CREDITS_END (CREDITS_TAIL + 5)

// Write virtual credits row r into the (32-row, wrapping) text map.
static void credits_write_row(int r)
{
    text_clear_rows(TOP, r & 31, (r & 31) + 1);
    int i = r - CREDITS_LEAD;
    if (i == 0) text_center(TOP, r, "NUBBY DS", TXT_GOLD);
    if (i == 2) text_center(TOP, r, "CREDITS", TXT_PLAIN);
    i -= CREDITS_HEAD;
    if (i < 0) return;
    int role = i / 3;
    if (role < NUM_ROLES) {
        if (i % 3 == 0) text_center(TOP, r, credits[role].role, TXT_PLAIN);
        if (i % 3 == 1) text_center(TOP, r, credits[role].name, TXT_GOLD);
    } else if (r == CREDITS_TAIL) {
        text_center(TOP, r, "INSPIRED BY", TXT_PLAIN);
    } else if (r == CREDITS_TAIL + 1) {
        text_center(TOP, r, "NUBBY'S NUMBER FACTORY", TXT_GOLD);
    } else if (r == CREDITS_END) {
        text_center(TOP, r, "THANKS FOR PLAYING!", TXT_GOLD);
    }
}

static void start_credits(void)
{
    state = ST_CREDITS;
    text_clear(TOP);
    text_clear(BOT);
    dim(TOP, 11);
    credits_scroll = 0;
    for (credits_rows = 0; credits_rows < 25; credits_rows++) credits_write_row(credits_rows);
    menu_sel = 0;
    draw_buttons(back_button, 1, 0);
}

static void update_credits(void)
{
    int sel = 0;
    if (buttons_update(back_button, 1, &sel) == 0 || (pressed & (KEY_B | KEY_START))) {
        back_to_title(2);
        return;
    }
    // scroll until the last line sits in the middle of the screen, then hold
    int top = credits_scroll >> 3;
    if ((frames & 1) && top < CREDITS_END - 11) credits_scroll++;
    top = credits_scroll >> 3;
    while (credits_rows <= top + 24) credits_write_row(credits_rows++);
    text_scroll(TOP, credits_scroll & 255);
}

// ---------------------------------------------------------------- the run

// A new song every milestone, and the boss theme on boss rounds.
static int song_for_round(int round)
{
    static const int tiers[] = { SONG_FACTORY, SONG_ASSEMBLY, SONG_OVERTIME, SONG_MELTDOWN };
    if (game_boss_for(round)) return SONG_BOSS;
    return tiers[(round - 1) / BOSS_EVERY % 4];
}

static void draw_boss_intro(void)
{
    const BossInfo *b = &boss_info[game.boss];
    char buf[40], *p;
    panel(BOT, 3, 5, 26, 13);
    text_center(BOT, 6, "BOSS ROUND!", TXT_HILITE);
    text_center(BOT, 8, b->name, TXT_HILITE);
    text_center(BOT, 10, b->line1, TXT_PANEL);
    text_center(BOT, 11, b->line2, TXT_PANEL);
    p = put_str(buf, "WIN FOR +");
    p = put_num(p, BOSS_BONUS);
    put_str(p, " COINS");
    text_center(BOT, 13, buf, TXT_PANEL);
    text_center(BOT, 15, "TOUCH TO START", TXT_HILITE);
}

// Fade to this round's board (the pegs carry over from the last round).
static void show_board(void)
{
    fade(1);
    music_stop();
    gfx_picture(TOP, dashboard_bin);
    gfx_picture(BOT, boards[(game.round - 1) % NUM_BOARDS]);
    dim(TOP, 0);
    dim(BOT, 0);
    text_clear(BOT);
    show_detail(DETAIL_NONE, 0, "");
    clear_effects();
    sync_board();
    state = ST_AIM;
    aiming_by_touch = 0;
    hud_text();
    frame();
    fade(0);
    if (game.boss) {                        // explain the hazard before the first launch
        state = ST_BOSS;
        dim(BOT, PAUSE_DIM);
        draw_boss_intro();
    }
    music_play(song_for_round(game.round));
}

static void new_run(void)
{
    game_new_run(&game, seed ^ (uint32_t)frames * 2654435761u);
    best_launch = 0;
    aim = 0;
    set_face(FACE_HAPPY, 0);
    for (int i = 0; i < NUM_SLOTS; i++) shown[i] = -1;
    show_board();
}

static void game_over(void)
{
    char buf[40], *p;
    int record = game.round > save.best_round && save.best_round > 0;
    state = ST_OVER;
    sfx_over();
    record_run();
    set_face(FACE_WORRY, 1 << 30);
    text_clear(BOT);
    dim(BOT, PAUSE_DIM);
    panel(BOT, 4, 5, 24, 12);
    text_center(BOT, 6, "GAME OVER", TXT_HILITE);
    p = put_str(buf, "REACHED ROUND ");
    put_num(p, game.round);
    text_center(BOT, 8, buf, TXT_PANEL);
    p = put_str(buf, "BEST LAUNCH ");
    put_num(p, best_launch);
    text_center(BOT, 10, buf, TXT_PANEL);
    if (record) text_center(BOT, 12, "NEW RECORD!", TXT_HILITE);
    text_center(BOT, 14, "TOUCH TO CONTINUE", TXT_PANEL);
    hud_text();
}

static void finish_launch(void)
{
    char buf[40], *p;
    int score = game.score;
    int quota = game.quota;
    int boss = game.boss;
    last_result = game_resolve(&game);
    score = game.perfect ? score * 2 : score;
    if (score > best_launch) best_launch = score;
    sync_board();

    if (last_result == RESULT_GAME_OVER) {
        game_over();
        return;
    }
    text_clear(BOT);
    dim(BOT, PAUSE_DIM);
    panel(BOT, 5, 6, 22, 10);
    p = put_num(buf, score);
    p = put_str(p, " OF ");
    put_num(p, quota);
    text_center(BOT, 9, buf, TXT_PANEL);
    if (last_result == RESULT_CLEARED) {
        sfx_clear();
        set_face(FACE_WOW, RESULT_FRAMES);
        text_center(BOT, 7, game.perfect ? "PERFECT! X2" : boss ? "BOSS BEATEN!" : "QUOTA MET!", TXT_HILITE);
        p = put_num(buf, game.restocks);
        put_str(p, game.restocks == 1 ? " RESTOCK" : " RESTOCKS");
        text_center(BOT, 11, buf, TXT_PANEL);
        int coins = game.restocks + (boss ? BOSS_BONUS : 0);
        p = put_str(buf, "+");
        p = put_num(p, coins);
        put_str(p, coins == 1 ? " COIN" : " COINS");
        text_center(BOT, 13, buf, TXT_HILITE);
    } else {
        sfx_deny();
        set_face(FACE_WORRY, RESULT_FRAMES);
        text_center(BOT, 7, "MISSED!", TXT_HILITE);
        p = put_num(buf, game.lives);
        put_str(p, game.lives == 1 ? " LIFE LEFT" : " LIVES LEFT");
        text_center(BOT, 11, buf, TXT_PANEL);
        text_center(BOT, 13, "THE BOARD RESETS", TXT_PANEL);
    }
    hud_text();
    state = ST_RESULT;
    timer = RESULT_FRAMES;
}

// ---------------------------------------------------------------- shop

static const Button next_button[] = { { 7, 19, 18, 3, "NEXT ROUND" } };

static void shop_detail(void)
{
    int item = shop_sel < SHOP_SLOTS ? game.shop[shop_sel] : -1;
    if (item < 0) {
        show_detail(DETAIL_NONE, 0, "");
        return;
    }
    const char *hint = game.coins < item_info[item].price ? "NOT ENOUGH COINS"
                       : game.nitems >= MAX_ITEMS         ? "TAP AGAIN TO SWAP IT IN"
                                                          : "TAP AGAIN TO BUY";
    show_detail(DETAIL_ITEM, item, hint);
}

static void draw_shop(void)
{
    char buf[40], *p;
    text_clear(BOT);
    text_center(BOT, 1, "SHOP", TXT_GOLD);
    p = put_str(buf, "COINS ");
    p = put_num(p, game.coins);
    if (game.nitems >= MAX_ITEMS) {
        put_str(p, "   ITEMS FULL");
    } else {
        p = put_str(p, "   ITEMS ");
        p = put_num(p, game.nitems);
        p = put_str(p, "/");
        put_num(p, MAX_ITEMS);
    }
    text_center(BOT, 2, buf, TXT_GOLD);
    for (int s = 0; s < SHOP_SLOTS; s++) {
        int row = 4 + s * 5, item = game.shop[s], on = s == shop_sel;
        panel(BOT, 1, row, 30, 5);
        if (item < 0) {
            text_style(BOT, 7, row + 2, "SOLD", TXT_DIM);
            continue;
        }
        text_style(BOT, 7, row + 1, item_info[item].name, on ? TXT_HILITE : TXT_PANEL);
        p = put_num(buf, item_info[item].price);
        put_str(p, " COINS");
        text_style(BOT, 21, row + 1, buf, game.coins >= item_info[item].price ? TXT_PANEL : TXT_DIM);
        text_style(BOT, 7, row + 3, trigger_short[item_info[item].trigger], TXT_DIM);
        if (on) text_style(BOT, 2, row + 2, ">", TXT_HILITE);
    }
    draw_buttons(next_button, 1, shop_sel == SHOP_SLOTS ? 0 : -1);
    shop_detail();
    hud_text();
}

static void open_shop(void)
{
    fade(1);
    game_roll_shop(&game);
    state = ST_SHOP;
    shop_sel = 0;
    while (shop_sel < SHOP_SLOTS && game.shop[shop_sel] < 0) shop_sel++;
    swap_for = -1;
    dim_picture(BOT, SHOP_DIM);
    draw_shop();
    frame();
    fade(0);
    music_play(SONG_SHOP);
}

static void draw_swap(void)
{
    char buf[40], *p;
    text_clear(BOT);
    text_center(BOT, 0, "YOUR ITEMS ARE FULL", TXT_GOLD);
    p = put_str(buf, "SWAP ONE FOR ");
    put_str(p, item_info[game.shop[swap_for]].name);
    text_center(BOT, 1, buf, TXT_GOLD);
    for (int i = 0; i < game.nitems; i++) {
        int row = 3 + i * 3, on = i == menu_sel;
        panel(BOT, 1, row, 30, 3);
        text_style(BOT, 5, row + 1, item_info[game.items[i]].name, on ? TXT_HILITE : TXT_PANEL);
        p = put_str(buf, "+");
        p = put_num(p, game_refund(game.items[i]));
        put_str(p, " COINS BACK");
        text_style(BOT, 16, row + 1, buf, on ? TXT_HILITE : TXT_PANEL);
    }
    draw_buttons(back_button, 1, -1);
    show_detail(DETAIL_ITEM, game.items[menu_sel], "TAP AGAIN TO SWAP THIS OUT");
    hud_text();
}

static void leave_swap(int bought)
{
    shop_sel = swap_for;
    swap_for = -1;
    state = ST_SHOP;
    if (bought) {
        sfx_buy();
        set_face(FACE_WOW, 40);
        shop_sel = SHOP_SLOTS;
    }
    draw_shop();
}

static void update_swap(void)
{
    int choose = -1, n = game.nitems;
    for (int i = 0; i < n; i++)
        if (tapped_in(8, (3 + i * 3) * 8, 240, 24)) {
            if (menu_sel == i) {
                choose = i;
            } else {
                menu_sel = i;
                sfx_move();
                draw_swap();
            }
        }
    if (pressed & (KEY_UP | KEY_DOWN)) {
        menu_sel = (menu_sel + ((pressed & KEY_UP) ? n - 1 : 1)) % n;
        sfx_move();
        draw_swap();
    }
    if (pressed & KEY_A) choose = menu_sel;
    if (choose >= 0) {
        game_buy_swap(&game, swap_for, choose);
        leave_swap(1);
        return;
    }
    int sel = 0;
    if (buttons_update(back_button, 1, &sel) == 0 || (pressed & KEY_B)) leave_swap(0);
}

static void try_buy(void)
{
    int item = game.shop[shop_sel];
    if (item < 0) return;
    if (game.nitems >= MAX_ITEMS && game.coins >= item_info[item].price) {
        swap_for = shop_sel;                   // hands full: choose what to let go of
        menu_sel = 0;
        state = ST_SWAP;
        sfx_move();
        draw_swap();
        return;
    }
    if (game_buy(&game, shop_sel)) {
        sfx_buy();
        set_face(FACE_WOW, 40);
    } else {
        sfx_deny();
    }
    draw_shop();
}

static void update_shop(void)
{
    for (int s = 0; s < SHOP_SLOTS; s++)
        if (tapped_in(8, (4 + s * 5) * 8, 240, 40) && game.shop[s] >= 0) {
            if (shop_sel == s) {
                try_buy();
            } else {
                shop_sel = s;
                sfx_move();
                draw_shop();
            }
            return;
        }
    if (tapped_in(next_button[0].x * 8, next_button[0].y * 8, next_button[0].w * 8, next_button[0].h * 8)) {
        show_board();
        return;
    }
    if (pressed & (KEY_UP | KEY_DOWN)) {
        shop_sel = (shop_sel + ((pressed & KEY_UP) ? SHOP_SLOTS : 1)) % (SHOP_SLOTS + 1);
        sfx_move();
        draw_shop();
    }
    if (pressed & KEY_A) {
        if (shop_sel == SHOP_SLOTS) show_board();
        else try_buy();
    }
}

// ---------------------------------------------------------------- perks

static void next_after_perk(void)
{
    if (game_shop_due(&game)) open_shop();
    else show_board();
}

static void draw_perks(void)
{
    text_clear(BOT);
    text_center(BOT, 1, "CHOOSE A PERK", TXT_GOLD);
    text_center(BOT, 2, "PERKS SET OFF YOUR ITEMS", TXT_GOLD);
    for (int c = 0; c < PERK_CHOICES; c++) {
        const PerkInfo *pk = &perk_info[game.perk_offer[c]];
        int row = 4 + c * 8, on = c == menu_sel;
        panel(BOT, 1, row, 30, 7);
        text_style(BOT, 7, row + 1, pk->name, on ? TXT_HILITE : TXT_PANEL);
        text_style(BOT, 7, row + 3, pk->line1, TXT_PANEL);
        text_style(BOT, 7, row + 4, pk->line2, TXT_PANEL);
        if (on) text_style(BOT, 2, row + 5, ">", TXT_HILITE);
    }
    show_detail(DETAIL_PERK, game.perk_offer[menu_sel], "TAP AGAIN TO TAKE IT");
    hud_text();
}

static void open_perks(void)
{
    fade(1);
    game_roll_perks(&game);
    state = ST_PERK;
    menu_sel = 0;
    dim_picture(BOT, SHOP_DIM);
    draw_perks();
    frame();
    fade(0);
    music_play(SONG_SHOP);
}

static void update_perks(void)
{
    int take = -1;
    for (int c = 0; c < PERK_CHOICES; c++)
        if (tapped_in(8, (4 + c * 8) * 8, 240, 56)) {
            if (menu_sel == c) {
                take = c;
            } else {
                menu_sel = c;
                sfx_move();
                draw_perks();
            }
        }
    if (pressed & (KEY_UP | KEY_DOWN)) {
        menu_sel ^= 1;
        sfx_move();
        draw_perks();
    }
    if (pressed & KEY_A) take = menu_sel;
    if (take < 0) return;
    game_take_perk(&game, take);
    sfx_buy();
    set_face(FACE_WOW, 40);
    next_after_perk();
}

// ---------------------------------------------------------------- pause and the items and perks screen

static const Button pause_buttons[] = {
    { 6, 5, 20, 3, "RESUME" }, { 6, 9, 20, 3, "ITEMS AND PERKS" }, { 6, 13, 20, 3, "QUIT" },
};

static void draw_pause(void)
{
    dim(BOT, PAUSE_DIM);
    text_clear(BOT);
    text_center(BOT, 2, "PAUSED", TXT_GOLD);
    draw_buttons(pause_buttons, 3, menu_sel);
}

static void pause_game(void)
{
    paused_from = state;
    state = ST_PAUSE;
    menu_sel = 0;
    music_stop();
    draw_pause();
}

static void resume(void)
{
    dim(BOT, 0);
    text_clear(BOT);
    show_detail(DETAIL_NONE, 0, "");
    hud_text();
    state = paused_from;
    aiming_by_touch = 0;
    music_resume();
}

static void inventory_detail(void)
{
    if (inv_sel < game.nitems) show_detail(DETAIL_ITEM, game.items[inv_sel], "");
    else if (inv_sel < game.nitems + game.nperks) show_detail(DETAIL_PERK, game.perks[inv_sel - game.nitems], "");
    else show_detail(DETAIL_NONE, 0, "");
    hud_text();
}

static void draw_inventory(void)
{
    text_clear(BOT);
    dim_picture(BOT, SHOP_DIM);
    text_center(BOT, 1, "ITEMS AND PERKS", TXT_GOLD);
    text_center(BOT, 2, "TOUCH ONE TO READ ABOUT IT", TXT_GOLD);
    panel(BOT, 1, 4, 30, 7);
    text_style(BOT, 3, 4, "ITEMS", TXT_HILITE);
    panel(BOT, 1, 11, 30, 7);
    text_style(BOT, 3, 11, "PERKS", TXT_HILITE);
    if (!game.nitems) text_center(BOT, 7, "BUY THEM IN THE SHOP", TXT_DIM);
    if (!game.nperks) text_center(BOT, 14, "ONE EVERY 5 ROUNDS", TXT_DIM);
    draw_buttons(back_button, 1, -1);
    inventory_detail();
}

static void inventory_pos(int k, int *x, int *y)
{
    int row = k < game.nitems ? 0 : 1;
    int col = row ? k - game.nitems : k;
    *x = 26 + col * 44;
    *y = row ? 108 : 52;
}

static void open_inventory(void)
{
    state = ST_INVENTORY;
    inv_sel = 0;
    draw_inventory();
}

static void update_inventory(void)
{
    int n = game.nitems + game.nperks;
    for (int k = 0; k < n; k++) {
        int x, y;
        inventory_pos(k, &x, &y);
        if (tapped_in(x - 4, y - 4, 40, 40)) {
            inv_sel = k;
            sfx_move();
            inventory_detail();
        }
    }
    if (n > 1 && (pressed & (KEY_LEFT | KEY_UP | KEY_L))) {
        inv_sel = (inv_sel + n - 1) % n;
        sfx_move();
        inventory_detail();
    }
    if (n > 1 && (pressed & (KEY_RIGHT | KEY_DOWN | KEY_R))) {
        inv_sel = (inv_sel + 1) % n;
        sfx_move();
        inventory_detail();
    }
    int sel = 0;
    if (buttons_update(back_button, 1, &sel) == 0 || (pressed & KEY_B)) {
        show_detail(DETAIL_NONE, 0, "");
        state = ST_PAUSE;
        menu_sel = 1;
        draw_pause();
        hud_text();
    }
}

static void update_pause(void)
{
    if (pressed & (KEY_B | KEY_START)) {
        resume();
        return;
    }
    switch (buttons_update(pause_buttons, 3, &menu_sel)) {
    case 0:
        resume();
        break;
    case 1:
        open_inventory();
        break;
    case 2:
        record_run();
        go_title();
        break;
    }
}

// Icons on the menu cards, drawn every frame.
static void ui_sprites(void)
{
    if (state == ST_SHOP) {
        for (int s = 0; s < SHOP_SLOTS; s++)
            if (game.shop[s] >= 0)
                spr(BOT, OBJ_UI + s, 16, (4 + s * 5) * 8 + 4, SpriteSize_32x32, TILE_BIGICON(game.shop[s]),
                    PAL_ICON, 0);
    } else if (state == ST_SWAP) {
        for (int i = 0; i < game.nitems; i++)
            spr(BOT, OBJ_UI + i, 16, (3 + i * 3) * 8 + 4, SpriteSize_16x16, TILE_ICON(game.items[i]), PAL_ICON, 0);
    } else if (state == ST_PERK) {
        for (int c = 0; c < PERK_CHOICES; c++)
            spr(BOT, OBJ_UI + c, 16, (4 + c * 8) * 8 + 12, SpriteSize_32x32, TILE_BIGPERK(game.perk_offer[c]),
                PAL_ICON, 0);
    } else if (state == ST_INVENTORY) {
        for (int k = 0; k < game.nitems + game.nperks; k++) {
            int x, y;
            inventory_pos(k, &x, &y);
            int t = k < game.nitems ? TILE_BIGICON(game.items[k]) : TILE_BIGPERK(game.perks[k - game.nitems]);
            spr(BOT, OBJ_UI + k, x, y - (k == inv_sel ? 3 : 0), SpriteSize_32x32, t,
                k == inv_sel ? PAL_ICON_FLASH : PAL_ICON, 0);
        }
    }
}

// ---------------------------------------------------------------- play

static int pause_tapped(void) { return tapped_in(224, 0, 32, 20); }

static void update_aim(void)
{
    if ((pressed & KEY_START) || pause_tapped()) {
        pause_game();
        return;
    }
    // touch: drag to aim, let go to launch; letting go up by the launcher cancels
    if (tapped) aiming_by_touch = 1;
    if (touching && aiming_by_touch) aim = game_aim_at(tx, ty);
    int launch = 0;
    if (released && aiming_by_touch) {
        aiming_by_touch = 0;
        launch = ty > LAUNCH_Y + 10;
    }
    if (held & KEY_LEFT) aim--;               // negative angles aim left
    if (held & KEY_RIGHT) aim++;
    if (pressed & KEY_L) aim--;               // shoulders nudge one step for fine aim
    if (pressed & KEY_R) aim++;
    if (aim > AIM_MAX) aim = AIM_MAX;
    if (aim < -AIM_MAX) aim = -AIM_MAX;
    if (launch || (pressed & KEY_A)) {
        game_launch(&game, aim);
        board_effects();                      // launch items (Pump, Seeder) change the board
        sfx_launch();
        state = ST_FLY;
    }
    hud_text();
}

static void update_fly(void)
{
    if ((pressed & KEY_START) || pause_tapped()) {
        pause_game();
        return;
    }
    Events ev;
    int out = game_step(&game, &ev);
    int px = board_effects();
    if (ev.laser) {
        sfx_laser();
        shake_timer = 14;
        set_face(FACE_WORRY, 40);
    } else if (ev.armor) {
        sfx_armor();
    } else if (ev.spring) {
        sfx_spring();
    } else if (ev.pop) {
        sfx_peg(ev.hits, ev.gone ? SFX_POP_GONE : SFX_POP, px < 0 ? game.x >> 8 : px);
    } else if (ev.item) {
        sfx_item();
    } else if (ev.wall) {
        sfx_wall(game.x >> 8);
    }
    if (ev.pop && (ev.hits % 8 == 0 || (game.score >= game.quota && face != FACE_WOW))) set_face(FACE_WOW, 40);
    else if ((game.y >> 8) > FLOOR_Y - 30 && game.score < game.quota && !face_timer) set_face(FACE_WORRY, 20);
    hud_text();
    if (out) finish_launch();
}

static void update_result(void)
{
    if (--timer > 0 && !tapped && !(pressed & (KEY_A | KEY_START))) return;
    if (last_result == RESULT_CLEARED) {
        if (game_perk_due(&game)) open_perks();
        else next_after_perk();
    } else {
        text_clear(BOT);
        dim(BOT, 0);
        sync_board();
        state = ST_AIM;
        aiming_by_touch = 0;
        hud_text();
    }
}

static void update_title(void)
{
    if (pressed & KEY_START) {
        new_run();
        return;
    }
    switch (buttons_update(title_buttons, 3, &menu_sel)) {
    case 0:
        new_run();
        break;
    case 1:
        show_howto();
        break;
    case 2:
        start_credits();
        break;
    }
}

int main(void)
{
    gfx_init();
    sound_init();
    save_load();
    go_title();

    for (;;) {
        read_input();
        switch (state) {
        case ST_TITLE:
            update_title();
            break;
        case ST_HOWTO: {
            int sel = 0;
            if (buttons_update(back_button, 1, &sel) == 0 || (pressed & (KEY_B | KEY_START))) back_to_title(1);
            break;
        }
        case ST_CREDITS:
            update_credits();
            break;
        case ST_AIM:
            update_aim();
            break;
        case ST_FLY:
            update_fly();
            break;
        case ST_RESULT:
            update_result();
            break;
        case ST_PERK:
            update_perks();
            break;
        case ST_SHOP:
            update_shop();
            break;
        case ST_SWAP:
            update_swap();
            break;
        case ST_PAUSE:
            update_pause();
            break;
        case ST_INVENTORY:
            update_inventory();
            break;
        case ST_BOSS:
            if (tapped || (pressed & (KEY_A | KEY_START))) {
                dim(BOT, 0);
                text_clear(BOT);
                state = ST_AIM;
                aiming_by_touch = 0;
            }
            break;
        case ST_OVER:
            if (tapped || (pressed & (KEY_A | KEY_START))) go_title();
            break;
        }
        frame();
    }
}
