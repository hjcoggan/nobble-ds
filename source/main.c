#include "gba.h"
#include "assets.h"
#include "game.h"
#include "sound.h"
#include "save.h"
#include "ui.h"

#define MAP_SBB 31
#define PAUSE_DIM 9               // 0-16 brightness decrease behind menus
#define RESULT_FRAMES 100
#define AIM_DOTS 8

// OAM slots
#define OBJ_NUBBY 0
#define OBJ_ICON  1               // 5 slots: owned items, or shop wares / perk choices
#define OBJ_PERK  (OBJ_ICON + MAX_ITEMS)
#define OBJ_DOT   (OBJ_PERK + MAX_PERKS)   // aim guide
#define OBJ_PEG   (OBJ_DOT + AIM_DOTS)
#define OBJ_FX    (OBJ_PEG + NUM_SLOTS)   // boss laser beam or wind streaks (20)

// each slot's peg has its own 16x16 sprite with its number drawn on
#define PEG_TILE(i) (TILE_FREE + (i) * 4)

#define ATTR1_SIZE16 0x4000

typedef enum {
    ST_TITLE, ST_MENU, ST_HOWTO, ST_CREDITS,
    ST_AIM, ST_FLY, ST_RESULT, ST_PERK, ST_SHOP, ST_PAUSE, ST_INVENTORY, ST_BOSS, ST_OVER,
} State;

enum { MAIN_PLAY, MAIN_HOWTO, MAIN_CREDITS, MAIN_COUNT };
enum { PAUSE_RESUME, PAUSE_INVENTORY, PAUSE_QUIT, PAUSE_COUNT };

static uint16_t oam[128 * 4];
static Game game;
static State state, paused_from;
static Result last_result;
static int frames, timer, menu_sel, aim, best_launch;
static int inv_sel;                // which owned item or perk the pause screen shows
static int swap_for = -1;          // shop slot being bought with full hands, -1 if not swapping
static int credits_scroll, credits_rows;
static int32_t shown[NUM_SLOTS];  // value currently drawn on each peg sprite
static uint32_t seed = 0x5EED1234;
static uint16_t keys, prev_keys;

// ---------------------------------------------------------------- video

static void load_bg(const uint16_t *pal, const uint32_t *tiles)
{
    PAL_BG[0] = pal[0];
    for (int i = BG_FIRST_COLOR; i < 256; i++) PAL_BG[i] = pal[i];
    volatile uint32_t *d = CHARBLOCK(0);
    for (int i = 0; i < BG_IMG_WORDS; i++) d[i] = tiles[i];
}

static void init_video(void)
{
    REG_DISPCNT = 0x0080;         // forced blank while loading
    REG_BLDCNT = BLD_DARKEN | BLD_BG0 | BLD_BG1 | BLD_OBJ | BLD_BD;
    REG_BLDY = 16;                // start black, screens fade in

    for (int i = 0; i < 16; i++) PAL_BG[16 + i] = font_pal[i];
    for (int i = 0; i < (int)(sizeof(obj_pal) / 2); i++) PAL_OBJ[i] = obj_pal[i];

    volatile uint32_t *d = CHARBLOCK(FONT_CBB);
    for (int i = 0; i < (int)(sizeof(font_tiles) / 4); i++) d[i] = font_tiles[i];
    for (int i = 0; i < (int)(sizeof(obj_tiles) / 4); i++) OBJ_TILES[i] = obj_tiles[i];

    volatile uint16_t *map = SCREENBLOCK(MAP_SBB);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            map[y * 32 + x] = (x < 30 && y < 20) ? y * 30 + x : 0;
    text_clear();

    REG_BG0CNT = BG_PRIO(3) | BG_CBB(0) | BG_SBB(MAP_SBB) | BG_8BPP;
    REG_BG1CNT = BG_PRIO(0) | BG_CBB(FONT_CBB) | BG_SBB(TEXT_SBB);

    for (int i = 0; i < 128; i++) oam[i * 4] = ATTR0_HIDE;
    for (int i = 0; i < NUM_SLOTS; i++) shown[i] = -1;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;
}

static void dim(int on, int level)
{
    REG_BLDCNT = on ? (BLD_DARKEN | BLD_BG0 | BLD_OBJ) : 0;
    REG_BLDY = level;
}

static void set_obj(int n, int x, int y, uint16_t a1size, uint16_t a2)
{
    oam[n * 4 + 0] = y & 255;
    oam[n * 4 + 1] = (x & 511) | a1size;
    oam[n * 4 + 2] = a2;
}

static void hide_obj(int n) { oam[n * 4] = ATTR0_HIDE; }

static void icon_obj(int n, int item, int x, int y, int prio, int lit)
{
    set_obj(n, x, y, ATTR1_SIZE16, TILE_ICON(item) | ATTR2_PRIO(prio) | ATTR2_PAL(lit ? PAL_ICON_FLASH : PAL_ICON));
}

static void perk_obj(int n, int perk, int x, int y, int prio, int lit)
{
    set_obj(n, x, y, ATTR1_SIZE16, TILE_PERK(perk) | ATTR2_PRIO(prio) | ATTR2_PAL(lit ? PAL_ICON_FLASH : PAL_ICON));
}

// ---------------------------------------------------------------- numbered pegs

// 3x5 digits (bit 14 = top left), then K for thousands
static const uint16_t digits3x5[11] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7252, 0x7BEF, 0x7BCF, 0x5D35,
};

static void peg_pixel(uint32_t *buf, int x, int y, int c)
{
    int word = ((y >> 3) * 2 + (x >> 3)) * 8 + (y & 7);
    int shift = (x & 7) * 4;
    buf[word] = (buf[word] & ~(0xFu << shift)) | ((uint32_t)c << shift);
}

// Redraw slot i's sprite: the disc template with the number on top.
static void render_peg(int i, int32_t v)
{
    uint32_t buf[32];
    for (int w = 0; w < 32; w++) buf[w] = obj_tiles[TILE_PEG * 8 + w];

    int glyphs[4], n = 0;
    if (v >= 10000) {                     // 12K, 999K
        int k = v / 1000;
        if (k > 999) k = 999;
        int tmp[3], m = 0;
        do {
            tmp[m++] = k % 10;
            k /= 10;
        } while (k);
        while (m) glyphs[n++] = tmp[--m];
        glyphs[n++] = 10;
    } else {
        int tmp[4], m = 0;
        do {
            tmp[m++] = v % 10;
            v /= 10;
        } while (v);
        while (m) glyphs[n++] = tmp[--m];
    }
    int x0 = (16 - (n * 4 - 1)) / 2;
    for (int g = 0; g < n; g++) {
        uint16_t bits = digits3x5[glyphs[g]];
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (bits & (1 << (14 - r * 3 - c))) peg_pixel(buf, x0 + g * 4 + c, 5 + r, 5);
    }
    volatile uint32_t *dst = OBJ_TILES + PEG_TILE(i) * 8;
    for (int w = 0; w < 32; w++) dst[w] = buf[w];
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

static int on_board(void)
{
    return state == ST_AIM || state == ST_FLY || state == ST_RESULT || state == ST_PAUSE ||
           state == ST_BOSS || state == ST_OVER;
}

static void draw_objects(void)
{
    for (int i = 0; i < 128; i++) hide_obj(i);

    if (state == ST_SHOP && swap_for >= 0) {
        for (int i = 0; i < game.nitems; i++) icon_obj(OBJ_ICON + i, game.items[i], 24, 36 + i * 16, 0, 0);
        return;
    }
    if (state == ST_SHOP) {
        for (int s = 0; s < SHOP_SLOTS; s++)
            if (game.shop[s] >= 0) icon_obj(OBJ_ICON + s, game.shop[s], 24, 36 + s * 24, 0, 0);
        return;
    }
    if (state == ST_PERK) {
        for (int c = 0; c < PERK_CHOICES; c++) perk_obj(OBJ_ICON + c, game.perk_offer[c], 24, 44 + c * 40, 0, 0);
        return;
    }
    if (state == ST_INVENTORY) {
        // everything owned in a row; the one being read is lit and raised
        int n = game.nitems + game.nperks, x = 120 - n * 9;
        for (int k = 0; k < n; k++, x += 18) {
            int y = k == inv_sel ? 44 : 48;
            if (k < game.nitems) icon_obj(OBJ_ICON + k, game.items[k], x, y, 0, k == inv_sel);
            else perk_obj(OBJ_ICON + k, game.perks[k - game.nitems], x, y, 0, k == inv_sel);
        }
        return;
    }
    if (!on_board()) return;

    int blink = (frames % 180) < 8;
    int nx = state == ST_AIM ? LAUNCH_X : game.x >> 8;
    int ny = state == ST_AIM ? LAUNCH_Y : game.y >> 8;
    if ((state == ST_AIM || game.flying) && game_has(&game, ITEM_BIG))
        set_obj(OBJ_NUBBY, nx - 8, ny - 8, ATTR1_SIZE16,
                (blink ? TILE_NUBBY_BIG_BLINK : TILE_NUBBY_BIG) | ATTR2_PRIO(1) | ATTR2_PAL(PAL_NUBBY));
    else if (state == ST_AIM || game.flying)
        set_obj(OBJ_NUBBY, nx - 4, ny - 4, ATTR1_SIZE8,
                (blink ? TILE_NUBBY_BLINK : TILE_NUBBY) | ATTR2_PRIO(1) | ATTR2_PAL(PAL_NUBBY));

    if (state == ST_AIM) {
        int16_t xs[AIM_DOTS], ys[AIM_DOTS];
        int n = game_predict(&game, aim, xs, ys, AIM_DOTS);
        for (int d = 0; d < n; d++)
            set_obj(OBJ_DOT + d, xs[d] - 4, ys[d] - 4, ATTR1_SIZE8,
                    TILE_DOT | ATTR2_PRIO(1) | ATTR2_PAL(PAL_NUBBY));
    }

    // boss hazards: the laser locks onto a row, blinks a warning, then fires
    if (game.boss == BOSS_LASER && game.laser_row >= 0 && state == ST_FLY) {
        int warn = game.laser_timer < LASER_WARN;
        if (!warn || (frames & 4))
            for (int k = 0; k < 20; k++)
                set_obj(OBJ_FX + k, BOARD_L + k * 8, game_row_y(game.laser_row) - 4, 0,
                        (warn ? TILE_LASER_WARN : TILE_LASER) | ATTR2_PRIO(warn ? 1 : 0) | ATTR2_PAL(PAL_FX));
    }
    // wind streaks drift the way the wind blows
    if (game.boss == BOSS_WIND && (state == ST_AIM || state == ST_FLY || state == ST_BOSS)) {
        for (int k = 0; k < 10; k++) {
            int x = (k * 53 + frames * 3 * game.wind) % 160;
            if (x < 0) x += 160;
            set_obj(OBJ_FX + k, BOARD_L + x - 4, 24 + k * 13, 0, TILE_WIND | ATTR2_PRIO(2) | ATTR2_PAL(PAL_FX));
        }
    }

    // items stacked on the right, perks on the left; both flash when they fire
    for (int i = 0; i < game.nitems; i++)
        icon_obj(OBJ_ICON + i, game.items[i], 214, 41 + i * 15, 1, game.item_flash[i] & 4);
    for (int p = 0; p < game.nperks; p++)
        perk_obj(OBJ_PERK + p, game.perks[p], 2 + (p % 2) * 18, 112 + (p / 2) * 18, 1, game.perk_flash[p] & 4);

    for (int i = 0; i < NUM_SLOTS; i++) {
        int32_t v = game.pegs[i];
        if (!v) continue;
        if (v != shown[i]) {
            render_peg(i, v);
            shown[i] = v;
        }
        int pal = game.flash[i] ? PAL_FLASH : game.armor[i] ? PAL_ARMOR : PAL_TIER(tier(v));
        set_obj(OBJ_PEG + i, slots[i].x - 8, slots[i].y - 8, ATTR1_SIZE16,
                PEG_TILE(i) | ATTR2_PRIO(1) | ATTR2_PAL(pal));
    }
}

static void frame(void)
{
    draw_objects();
    vsync();
    for (int i = 0; i < 128 * 4; i++) OAM[i] = oam[i];
    sound_update();
    frames++;
}

static void fade(int to_black)
{
    REG_BLDCNT = BLD_DARKEN | BLD_BG0 | BLD_BG1 | BLD_OBJ | BLD_BD;
    for (int i = 0; i <= 16; i += 2) {
        REG_BLDY = to_black ? i : 16 - i;
        frame();
    }
    if (!to_black) REG_BLDCNT = 0;
}

// ---------------------------------------------------------------- text helpers

static void num_right(int x, int y, int v, int width)
{
    char buf[12];
    int max = 1;
    for (int i = 0; i < width; i++) max *= 10;
    if (v >= max) v = max - 1;
    format_num(buf, v, width);
    for (int i = 0; i < width - 1 && buf[i] == '0'; i++) buf[i] = ' ';
    text_at(x, y, buf);
}

// Append a number to a string without leading zeros; returns the new end.
static char *put_num(char *p, int v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v /= 10;
    } while (v && n < 11);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static void draw_hud(void)
{
    if (game.boss) text_style(0, 1, "BOSS!", TXT_GOLD);
    else text_at(0, 1, "ROUND");
    num_right(0, 2, game.round, 5);
    text_at(0, 4, "GOAL");
    num_right(0, 5, game.quota, 5);
    text_at(0, 7, "SCORE");
    num_right(0, 8, game.score, 5);
    text_at(0, 10, "LIVES");
    num_right(0, 11, game.lives, 5);

    if (game.nperks) text_at(0, 13, "PERKS");

    text_at(25, 1, "COINS");
    num_right(25, 2, game.coins, 5);
    text_at(25, 3, "ITEMS");
    if (game.nitems >= MAX_ITEMS) {
        text_style(25, 4, " FULL", TXT_GOLD);
    } else {
        char n[6] = { ' ', ' ', (char)('0' + game.nitems), '/', (char)('0' + MAX_ITEMS), 0 };
        text_at(25, 4, n);
    }
    int until = SHOP_EVERY - (game.round - 1) % SHOP_EVERY;
    text_at(25, 15, "SHOP");
    text_at(25, 16, "IN");
    num_right(27, 16, until, 3);
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

// ---------------------------------------------------------------- title, menus, credits

static const char *const main_items[MAIN_COUNT] = { "PLAY", "HOW TO PLAY", "CREDITS" };
static const char *const pause_items[PAUSE_COUNT] = { "RESUME", "ITEMS AND PERKS", "QUIT" };

static void draw_title_text(void)
{
    text_clear();
    if (save.best_round > 0) {
        char buf[32], *p = put_str(buf, "BEST ROUND ");
        p = put_num(p, save.best_round);
        p = put_str(p, "  LAUNCH ");
        put_num(p, save.best_score);
        panel(17, 28, 3);                   // a solid strip so it reads over the sky
        text_center(18, buf, TXT_HILITE);
    }
}

static void draw_main_menu(void) { menu_draw(9, 16, 0, main_items, MAIN_COUNT, menu_sel); }
static void draw_pause_menu(void) { menu_draw(6, 20, "PAUSED", pause_items, PAUSE_COUNT, menu_sel); }

static int menu_move(uint16_t pressed, int n)
{
    if (pressed & KEY_UP) {
        menu_sel = (menu_sel + n - 1) % n;
        sfx_move();
        return 1;
    }
    if (pressed & KEY_DOWN) {
        menu_sel = (menu_sel + 1) % n;
        sfx_move();
        return 1;
    }
    return 0;
}

static void go_title(void)
{
    fade(1);
    music_stop();
    REG_BG1VOFS = 0;
    load_bg(title_pal, title_tiles);
    draw_title_text();
    state = ST_TITLE;
    fade(0);
    music_play(SONG_FACTORY);
}

static void back_to_menu(int sel)
{
    REG_BG1VOFS = 0;
    dim(0, 0);
    state = ST_MENU;
    menu_sel = sel;
    draw_title_text();
    draw_main_menu();
}

static void show_howto(void)
{
    state = ST_HOWTO;
    text_clear();
    dim(1, 12);
    panel(1, 28, 18);
    text_center(2, "HOW TO PLAY", TXT_HILITE);
    text_style(2, 4, "AIM WITH LEFT AND RIGHT,", TXT_PANEL);
    text_style(2, 5, "A LAUNCHES NUBBY.", TXT_PANEL);
    text_style(2, 7, "A HIT SCORES THE PEG'S", TXT_PANEL);
    text_style(2, 8, "NUMBER AND HALVES IT.", TXT_PANEL);
    text_style(2, 9, "A 1 POPS AND VANISHES.", TXT_PANEL);
    text_style(2, 11, "REACH THE GOAL IN ONE", TXT_HILITE);
    text_style(2, 12, "LAUNCH OR LOSE A LIFE.", TXT_HILITE);
    text_style(2, 14, "BEAT IT BY MORE TO RESTOCK", TXT_PANEL);
    text_style(2, 15, "AND GROW THE PEGS.", TXT_PANEL);
    text_style(2, 16, "BOUNCE OFF THE WALLS!", TXT_PANEL);
}

static const struct { const char *role, *name; } credits[] = {
    { "GAME DIRECTOR", "HEATH" }, { "CREATIVE DIRECTOR", "CLAUDE" },
    { "TECHNICAL DIRECTOR", "CLAUDE" }, { "PRODUCER", "CLAUDE" },
    { "LEAD GAME DESIGNER", "CLAUDE" }, { "SYSTEMS DESIGNER", "CLAUDE" },
    { "PHYSICS PROGRAMMER", "CLAUDE" }, { "GAMEPLAY PROGRAMMER", "CLAUDE" },
    { "ENGINE PROGRAMMER", "CLAUDE" }, { "GRAPHICS PROGRAMMER", "CLAUDE" },
    { "AUDIO PROGRAMMER", "CLAUDE" }, { "TOOLS PROGRAMMER", "CLAUDE" },
    { "UI PROGRAMMER", "CLAUDE" }, { "BUILD ENGINEER", "CLAUDE" },
    { "ART DIRECTOR", "CLAUDE" }, { "PIXEL ARTIST", "CLAUDE" },
    { "CHARACTER ARTIST", "CLAUDE" }, { "UI ARTIST", "CLAUDE" },
    { "COMPOSER", "CLAUDE" }, { "SOUND DESIGNER", "CLAUDE" },
    { "QA LEAD", "CLAUDE" }, { "QA TESTER", "HEATH" },
    { "BALANCE TESTER", "HEATH" }, { "ECONOMY DESIGNER", "CLAUDE" },
    { "PEG ENGINEER", "CLAUDE" }, { "NUBBY WRANGLER", "CLAUDE" },
};
#define NUM_ROLES ((int)(sizeof(credits) / sizeof(credits[0])))
#define CREDITS_LEAD 20           // blank rows so the list starts below the screen
#define CREDITS_HEAD 4            // "NUBBY GBA", blank, "CREDITS", blank
#define CREDITS_TAIL (CREDITS_LEAD + CREDITS_HEAD + NUM_ROLES * 3 + 1)
#define CREDITS_END (CREDITS_TAIL + 5)

// Write virtual credits row r into the (32-row, wrapping) text map.
static void credits_write_row(int r)
{
    text_clear_row(r);
    int i = r - CREDITS_LEAD;
    if (i == 0) text_center(r, "NUBBY GBA", TXT_GOLD);
    if (i == 2) text_center(r, "CREDITS", TXT_PLAIN);
    i -= CREDITS_HEAD;
    if (i < 0) return;
    int role = i / 3;
    if (role < NUM_ROLES) {
        if (i % 3 == 0) text_center(r, credits[role].role, TXT_PLAIN);
        if (i % 3 == 1) text_center(r, credits[role].name, TXT_GOLD);
    } else if (r == CREDITS_TAIL) {
        text_center(r, "INSPIRED BY", TXT_PLAIN);
    } else if (r == CREDITS_TAIL + 1) {
        text_center(r, "NUBBY'S NUMBER FACTORY", TXT_GOLD);
    } else if (r == CREDITS_END) {
        text_center(r, "THANKS FOR PLAYING!", TXT_GOLD);
    }
}

static void start_credits(void)
{
    state = ST_CREDITS;
    text_clear();
    dim(1, 12);
    credits_scroll = 0;
    for (credits_rows = 0; credits_rows < 21; credits_rows++) credits_write_row(credits_rows);
}

static void update_credits(uint16_t pressed)
{
    if (pressed & (KEY_A | KEY_B | KEY_START)) {
        back_to_menu(MAIN_CREDITS);
        return;
    }
    // scroll until the last line sits in the middle of the screen, then hold
    int top = credits_scroll >> 3;
    if ((frames & 1) && top < CREDITS_END - 9) credits_scroll++;
    top = credits_scroll >> 3;
    while (credits_rows <= top + 20) credits_write_row(credits_rows++);
    REG_BG1VOFS = credits_scroll & 255;
}

// ---------------------------------------------------------------- the run

// Fade to this round's board (the pegs carry over from the last round).
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
    char buf[32], *p;
    panel(4, 24, 12);
    text_center(5, "BOSS ROUND!", TXT_HILITE);
    text_center(7, b->name, TXT_HILITE);
    text_center(9, b->line1, TXT_PANEL);
    text_center(10, b->line2, TXT_PANEL);
    p = put_str(buf, "WIN FOR +");
    p = put_num(p, BOSS_BONUS);
    put_str(p, " COINS");
    text_center(12, buf, TXT_PANEL);
    text_center(14, "PRESS A", TXT_PANEL);
}

static void update_boss_intro(uint16_t pressed)
{
    if (!(pressed & (KEY_A | KEY_START))) return;
    dim(0, 0);
    text_clear();
    draw_hud();
    state = ST_AIM;
}

static void show_board(void)
{
    fade(1);
    music_stop();
    int board = (game.round - 1) % NUM_BOARDS;
    load_bg(board_pal[board], board_tiles[board]);
    dim(0, 0);
    text_clear();
    draw_hud();
    state = ST_AIM;
    fade(0);
    if (game.boss) {                        // explain the hazard before the first launch
        state = ST_BOSS;
        dim(1, PAUSE_DIM);
        draw_boss_intro();
    }
    music_play(song_for_round(game.round));
}

static void new_run(void)
{
    game_new_run(&game, seed ^ (uint32_t)frames * 2654435761u);
    best_launch = 0;
    aim = 0;
    show_board();
}

static void game_over(void)
{
    char buf[32], *p;
    state = ST_OVER;
    sfx_over();
    record_run();
    text_clear();
    draw_hud();
    panel(5, 18, 9);
    text_center(6, "GAME OVER", TXT_HILITE);
    p = put_str(buf, "REACHED ROUND ");
    put_num(p, game.round);
    text_center(8, buf, TXT_PANEL);
    p = put_str(buf, "BEST LAUNCH ");
    put_num(p, best_launch);
    text_center(9, buf, TXT_PANEL);
    text_center(11, "PRESS START", TXT_PANEL);
}

// Called when Nubby falls out: score the launch and show how it went.
static void finish_launch(void)
{
    char buf[32], *p;
    int score = game.score;
    int quota = game.quota;
    int boss = game.boss;
    last_result = game_resolve(&game);
    score = game.perfect ? score * 2 : score;
    if (score > best_launch) best_launch = score;

    if (last_result == RESULT_GAME_OVER) {
        game_over();
        return;
    }
    text_clear();
    draw_hud();
    panel(5, 20, 9);
    p = put_num(buf, score);
    p = put_str(p, " OF ");
    put_num(p, quota);
    text_center(8, buf, TXT_PANEL);
    if (last_result == RESULT_CLEARED) {
        sfx_clear();
        text_center(6, game.perfect ? "PERFECT! X2" : boss ? "BOSS BEATEN!" : "QUOTA MET!", TXT_HILITE);
        p = put_num(buf, game.restocks);
        put_str(p, game.restocks == 1 ? " RESTOCK" : " RESTOCKS");
        text_center(10, buf, TXT_PANEL);
        int coins = game.restocks + (boss ? BOSS_BONUS : 0);
        p = put_str(buf, "+");
        p = put_num(p, coins);
        put_str(p, coins == 1 ? " COIN" : " COINS");
        text_center(12, buf, TXT_HILITE);
    } else {
        sfx_deny();
        text_center(6, "MISSED!", TXT_HILITE);
        p = put_num(buf, game.lives);
        put_str(p, game.lives == 1 ? " LIFE LEFT" : " LIVES LEFT");
        text_center(10, buf, TXT_PANEL);
        text_center(12, "THE BOARD RESETS", TXT_PANEL);
    }
    state = ST_RESULT;
    timer = RESULT_FRAMES;
}

// ---------------------------------------------------------------- shop

// Hands full: pick which owned item the new one replaces.
static void draw_swap(void)
{
    char buf[32];
    panel(1, 28, 18);
    text_center(2, "YOUR ITEMS ARE FULL", TXT_HILITE);
    put_str(put_str(buf, "SWAP ONE FOR "), item_info[game.shop[swap_for]].name);
    text_center(3, buf, TXT_PANEL);
    for (int i = 0; i < game.nitems; i++) {
        int on = i == menu_sel, row = 5 + i * 2;
        text_style(2, row, on ? ">" : " ", on ? TXT_HILITE : TXT_PANEL);
        text_style(6, row, item_info[game.items[i]].name, on ? TXT_HILITE : TXT_PANEL);
        char *q = put_str(buf, "+");
        q = put_num(q, game_refund(game.items[i]));
        put_str(q, " BACK");
        text_style(19, row, buf, on ? TXT_HILITE : TXT_PANEL);
    }
    const ItemInfo *it = &item_info[game.items[menu_sel]];
    text_center(15, trigger_text[it->trigger], TXT_PANEL);
    text_center(16, it->effect, TXT_PANEL);
    text_center(17, "A SWAP   B KEEP THEM", TXT_PANEL);
}

static void draw_shop(void)
{
    char buf[32], *p;
    panel(1, 28, 18);
    text_center(2, "SHOP", TXT_HILITE);
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
    text_center(3, buf, TXT_HILITE);
    for (int s = 0; s <= SHOP_SLOTS; s++) {
        int row = 5 + s * 3;
        int on = s == menu_sel;
        int style = on ? TXT_HILITE : TXT_PANEL;
        text_style(2, row, on ? ">" : " ", style);
        if (s == SHOP_SLOTS) {
            text_style(6, row, "NEXT ROUND", style);
            continue;
        }
        int item = game.shop[s];
        text_style(6, row, "                      ", TXT_PANEL);
        if (item < 0) {
            text_style(6, row, "SOLD", TXT_PANEL);
            continue;
        }
        text_style(6, row, item_info[item].name, style);
        p = put_num(buf, item_info[item].price);
        put_str(p, " COINS");
        text_style(18, row, buf, game.coins >= item_info[item].price ? TXT_PANEL : TXT_DIM);
    }
    text_style(2, 16, "                        ", TXT_PANEL);
    text_style(2, 17, "                        ", TXT_PANEL);
    if (menu_sel < SHOP_SLOTS && game.shop[menu_sel] >= 0) {
        const ItemInfo *it = &item_info[game.shop[menu_sel]];
        text_center(16, trigger_text[it->trigger], TXT_PANEL);
        text_center(17, it->effect, TXT_HILITE);
    }
}

static void open_shop(void)
{
    fade(1);
    game_roll_shop(&game);
    state = ST_SHOP;
    menu_sel = 0;
    text_clear();
    draw_shop();
    REG_BLDCNT = BLD_DARKEN | BLD_BG0;    // the shop sits over a darkened board
    REG_BLDY = 12;
    music_play(SONG_SHOP);
}

static void update_shop(uint16_t pressed)
{
    if (swap_for >= 0) {
        if (menu_move(pressed, game.nitems)) draw_swap();
        if (pressed & (KEY_A | KEY_B)) {
            if (pressed & KEY_A) {
                game_buy_swap(&game, swap_for, menu_sel);
                sfx_buy();
            }
            menu_sel = swap_for;
            swap_for = -1;
            draw_shop();
        }
        return;
    }
    if (menu_move(pressed, SHOP_SLOTS + 1)) draw_shop();
    if (!(pressed & KEY_A)) return;
    if (menu_sel == SHOP_SLOTS) {
        show_board();
        return;
    }
    int item = game.shop[menu_sel];
    if (item >= 0 && game.nitems >= MAX_ITEMS && game.coins >= item_info[item].price) {
        swap_for = menu_sel;                  // hands full: choose what to let go of
        menu_sel = 0;
        sfx_move();
        draw_swap();
        return;
    }
    if (game_buy(&game, menu_sel)) sfx_buy();
    else sfx_deny();
    draw_shop();
}

// ---------------------------------------------------------------- play

static void pause_game(void)
{
    paused_from = state;
    state = ST_PAUSE;
    menu_sel = PAUSE_RESUME;
    music_stop();
    dim(1, PAUSE_DIM);
    draw_pause_menu();
}

static void update_aim(uint16_t pressed)
{
    if (pressed & KEY_START) {
        pause_game();
        return;
    }
    if (keys & KEY_LEFT) aim--;           // negative angles aim left
    if (keys & KEY_RIGHT) aim++;
    if (pressed & KEY_L) aim--;           // shoulders nudge one step for fine aim
    if (pressed & KEY_R) aim++;
    if (aim > AIM_MAX) aim = AIM_MAX;
    if (aim < -AIM_MAX) aim = -AIM_MAX;
    if (pressed & KEY_A) {
        game_launch(&game, aim);
        sfx_launch();
        state = ST_FLY;
    }
}

static void update_fly(uint16_t pressed)
{
    if (pressed & KEY_START) {
        pause_game();
        return;
    }
    Events ev;
    int out = game_step(&game, &ev);
    if (ev.laser) sfx_laser();
    else if (ev.armor) sfx_armor();
    else if (ev.spring) sfx_spring();
    else if (ev.pop) sfx_peg(ev.hits, ev.gone ? SFX_POP_GONE : SFX_POP);
    else if (ev.item) sfx_item();
    else if (ev.wall) sfx_wall();
    draw_hud();
    if (out) finish_launch();
}

static void open_shop(void);

// After a cleared round: a perk choice, then the shop, then the next board.
static void next_after_perk(void)
{
    if (game_shop_due(&game)) open_shop();
    else show_board();
}

static void draw_perks(void)
{
    panel(2, 28, 16);
    text_center(3, "CHOOSE A PERK", TXT_HILITE);
    for (int c = 0; c < PERK_CHOICES; c++) {
        const PerkInfo *pk = &perk_info[game.perk_offer[c]];
        int row = 6 + c * 5, on = c == menu_sel;
        text_style(2, row, on ? ">" : " ", on ? TXT_HILITE : TXT_PANEL);
        text_style(6, row, pk->name, on ? TXT_HILITE : TXT_PANEL);
        text_style(6, row + 1, pk->line1, TXT_PANEL);
        text_style(6, row + 2, pk->line2, TXT_PANEL);
    }
    text_center(16, "PERKS TRIGGER YOUR ITEMS", TXT_PANEL);
}

static void open_perks(void)
{
    fade(1);
    game_roll_perks(&game);
    state = ST_PERK;
    menu_sel = 0;
    text_clear();
    draw_perks();
    REG_BLDCNT = BLD_DARKEN | BLD_BG0;
    REG_BLDY = 12;
    music_play(SONG_SHOP);
}

static void update_perks(uint16_t pressed)
{
    if (menu_move(pressed, PERK_CHOICES)) draw_perks();
    if (!(pressed & KEY_A)) return;
    game_take_perk(&game, menu_sel);
    sfx_buy();
    next_after_perk();
}

static void update_result(void)
{
    if (--timer > 0) return;
    if (last_result == RESULT_CLEARED) {
        if (game_perk_due(&game)) open_perks();
        else next_after_perk();
    } else {
        text_clear();
        draw_hud();
        state = ST_AIM;
    }
}

// ---------------------------------------------------------------- items and perks (from pause)

static void draw_inventory(void)
{
    char buf[32], *p;
    text_clear();
    panel(2, 28, 16);
    text_center(3, "ITEMS AND PERKS", TXT_HILITE);
    int n = game.nitems + game.nperks;
    if (!n) {
        text_center(8, "NOTHING YET!", TXT_PANEL);
        text_center(10, "BUY ITEMS IN THE SHOP", TXT_PANEL);
        text_center(11, "AND PICK A PERK EVERY", TXT_PANEL);
        text_center(12, "5 ROUNDS", TXT_PANEL);
    } else if (inv_sel < game.nitems) {
        const ItemInfo *it = &item_info[game.items[inv_sel]];
        p = put_str(buf, "ITEM ");
        p = put_num(p, inv_sel + 1);
        p = put_str(p, " OF ");
        put_num(p, game.nitems);
        text_center(4, buf, TXT_PANEL);
        text_center(9, it->name, TXT_HILITE);
        text_center(11, trigger_text[it->trigger], TXT_PANEL);
        text_center(12, it->effect, TXT_PANEL);
    } else {
        const PerkInfo *pk = &perk_info[game.perks[inv_sel - game.nitems]];
        p = put_str(buf, "PERK ");
        p = put_num(p, inv_sel - game.nitems + 1);
        p = put_str(p, " OF ");
        put_num(p, game.nperks);
        text_center(4, buf, TXT_PANEL);
        text_center(9, pk->name, TXT_HILITE);
        text_center(11, pk->line1, TXT_PANEL);
        text_center(12, pk->line2, TXT_PANEL);
    }
    if (n > 1) text_center(15, "LEFT AND RIGHT TO BROWSE", TXT_PANEL);
    text_center(16, "B TO GO BACK", TXT_PANEL);
}

static void open_inventory(void)
{
    state = ST_INVENTORY;
    inv_sel = 0;
    REG_BLDCNT = BLD_DARKEN | BLD_BG0;      // icons stay bright over the darkened board
    REG_BLDY = 12;
    draw_inventory();
}

static void update_inventory(uint16_t pressed)
{
    int n = game.nitems + game.nperks;
    if (n > 1 && (pressed & (KEY_LEFT | KEY_UP | KEY_L))) {
        inv_sel = (inv_sel + n - 1) % n;
        sfx_move();
        draw_inventory();
    }
    if (n > 1 && (pressed & (KEY_RIGHT | KEY_DOWN | KEY_R))) {
        inv_sel = (inv_sel + 1) % n;
        sfx_move();
        draw_inventory();
    }
    if (pressed & (KEY_B | KEY_START | KEY_A)) {
        state = ST_PAUSE;
        dim(1, PAUSE_DIM);
        text_clear();
        draw_hud();
        draw_pause_menu();
    }
}

static void update_pause(uint16_t pressed)
{
    if (menu_move(pressed, PAUSE_COUNT)) draw_pause_menu();
    int resume = (pressed & KEY_B) || ((pressed & (KEY_A | KEY_START)) && menu_sel == PAUSE_RESUME);
    if (resume) {
        dim(0, 0);
        text_clear();
        draw_hud();
        state = paused_from;
        music_resume();
    } else if ((pressed & (KEY_A | KEY_START)) && menu_sel == PAUSE_INVENTORY) {
        open_inventory();
    } else if ((pressed & (KEY_A | KEY_START)) && menu_sel == PAUSE_QUIT) {
        record_run();
        go_title();
    }
}

static void update_title(uint16_t pressed)
{
    if (state == ST_TITLE) {
        text_center(15, (frames & 32) ? "           " : "PRESS START", TXT_PLAIN);
        if (pressed & (KEY_START | KEY_A)) {
            state = ST_MENU;
            menu_sel = MAIN_PLAY;
            text_center(15, "           ", TXT_PLAIN);
            draw_main_menu();
        }
        return;
    }
    if (menu_move(pressed, MAIN_COUNT)) draw_main_menu();
    if (pressed & KEY_B) {
        state = ST_TITLE;
        draw_title_text();
        return;
    }
    if (!(pressed & (KEY_A | KEY_START))) return;
    switch (menu_sel) {
    case MAIN_PLAY:
        new_run();
        break;
    case MAIN_HOWTO:
        show_howto();
        break;
    case MAIN_CREDITS:
        start_credits();
        break;
    }
}

int main(void)
{
    init_video();
    sound_init();
    save_load();
    go_title();

    for (;;) {
        prev_keys = keys;
        keys = ~REG_KEYINPUT & 0x03FF;
        uint16_t pressed = keys & ~prev_keys;
        seed = seed * 1664525u + 1013904223u + keys;

        switch (state) {
        case ST_TITLE:
        case ST_MENU:
            update_title(pressed);
            break;
        case ST_HOWTO:
            if (pressed & (KEY_A | KEY_B | KEY_START)) back_to_menu(MAIN_HOWTO);
            break;
        case ST_CREDITS:
            update_credits(pressed);
            break;
        case ST_AIM:
            update_aim(pressed);
            break;
        case ST_FLY:
            update_fly(pressed);
            break;
        case ST_RESULT:
            update_result();
            break;
        case ST_PERK:
            update_perks(pressed);
            break;
        case ST_SHOP:
            update_shop(pressed);
            break;
        case ST_PAUSE:
            update_pause(pressed);
            break;
        case ST_INVENTORY:
            update_inventory(pressed);
            break;
        case ST_BOSS:
            update_boss_intro(pressed);
            break;
        case ST_OVER:
            if (pressed & KEY_START) go_title();
            break;
        }

        frame();
    }
}
