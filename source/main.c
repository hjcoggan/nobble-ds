#include "gba.h"
#include "assets.h"
#include "game.h"
#include "sound.h"
#include "save.h"
#include "ui.h"

#define MAP_SBB 31
#define PAUSE_DIM 9               // 0-16 brightness decrease behind menus
#define RESULT_FRAMES 60
#define CLEAR_FRAMES 120

// OAM slots
#define OBJ_NUBBY 0
#define OBJ_ICON  1               // 4 slots: owned items, or shop wares
#define OBJ_PEG   5

typedef enum {
    ST_TITLE, ST_MENU, ST_HOWTO, ST_CREDITS,
    ST_AIM, ST_FALL, ST_RESULT, ST_CLEAR, ST_SHOP, ST_PAUSE, ST_OVER,
} State;

enum { MAIN_PLAY, MAIN_HOWTO, MAIN_CREDITS, MAIN_COUNT };
enum { PAUSE_RESUME, PAUSE_QUIT, PAUSE_COUNT };

static uint16_t oam[128 * 4];
static Game game;
static State state, paused_from;
static int frames, timer, menu_sel, aim_x, best_at_start;
static int credits_scroll, credits_rows;
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
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;
}

static void dim(int on, int level)
{
    REG_BLDCNT = on ? (BLD_DARKEN | BLD_BG0 | BLD_OBJ) : 0;
    REG_BLDY = level;
}

#define ATTR1_SIZE16 0x4000

static void set_obj(int n, int x, int y, uint16_t a1size, uint16_t a2)
{
    oam[n * 4 + 0] = y & 255;
    oam[n * 4 + 1] = (x & 511) | a1size;
    oam[n * 4 + 2] = a2;
}

static void hide_obj(int n) { oam[n * 4] = ATTR0_HIDE; }

static void icon_obj(int n, int item, int x, int y, int prio)
{
    set_obj(n, x, y, ATTR1_SIZE16, TILE_ICON(item) | ATTR2_PRIO(prio) | ATTR2_PAL(PAL_ICON));
}

static void peg_obj(int n, int type, int lit, int x, int y, int prio)
{
    if (type == PEG_BUMPER)
        set_obj(n, x - 8, y - 8, ATTR1_SIZE16, TILE_BUMPER | ATTR2_PRIO(prio) | ATTR2_PAL(PAL_PEG(type, lit)));
    else
        set_obj(n, x - 4, y - 4, ATTR1_SIZE8, TILE_PEG | ATTR2_PRIO(prio) | ATTR2_PAL(PAL_PEG(type, lit)));
}

static int on_board(void)
{
    return state == ST_AIM || state == ST_FALL || state == ST_RESULT || state == ST_CLEAR ||
           state == ST_PAUSE || state == ST_OVER;
}

static void draw_objects(void)
{
    for (int i = 0; i < 128; i++) hide_obj(i);

    if (state == ST_HOWTO) {
        // example pegs next to the rules
        peg_obj(OBJ_PEG + 0, PEG_PLUS, 0, 36, 60, 0);
        peg_obj(OBJ_PEG + 1, PEG_MULT, 0, 36, 68, 0);
        peg_obj(OBJ_PEG + 2, PEG_COIN, 0, 36, 76, 0);
        peg_obj(OBJ_PEG + 3, PEG_BUMPER, 0, 36, 84, 0);
        return;
    }
    if (state == ST_SHOP) {
        for (int s = 0; s < SHOP_SLOTS; s++)
            if (game.shop[s] >= 0) icon_obj(OBJ_ICON + s, game.shop[s], 24, 36 + s * 24, 0);
        return;
    }
    if (!on_board()) return;

    // Nubby, hanging at the top while aiming
    int nx = state == ST_AIM ? aim_x : game.x >> 8;
    int ny = state == ST_AIM ? DROP_Y : game.y >> 8;
    int blink = (frames % 180) < 8;
    if (state != ST_OVER || game.dropping)
        set_obj(OBJ_NUBBY, nx - 4, ny - 4, ATTR1_SIZE8,
                (blink ? TILE_NUBBY_BLINK : TILE_NUBBY) | ATTR2_PRIO(1) | ATTR2_PAL(PAL_NUBBY));

    for (int i = 0; i < game.nitems; i++) icon_obj(OBJ_ICON + i, game.items[i], 212, 44 + i * 20, 1);

    for (int i = 0; i < game.npegs && OBJ_PEG + i < 128; i++) {
        const Peg *p = &game.pegs[i];
        int lit = p->type == PEG_BUMPER ? p->flash > 0 : p->lit;
        peg_obj(OBJ_PEG + i, p->type, lit, p->x, p->y, 1);
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
    text_at(0, 1, "ROUND");
    num_right(0, 2, game.round, 5);
    text_at(0, 4, "GOAL");
    num_right(0, 5, game.target, 5);
    text_at(0, 7, "SCORE");
    num_right(0, 8, game.round_score, 5);
    text_at(0, 10, "DROPS");
    num_right(0, 11, game.drops_left, 5);
    text_at(0, 13, "PTS");
    num_right(0, 14, game.dropping ? game.points : 0, 5);
    text_at(0, 15, "MULT");
    text_at(0, 16, "X");
    num_right(1, 16, game.dropping ? game.mult : 1, 4);

    text_at(25, 1, "COINS");
    num_right(25, 2, game.coins, 5);
    text_at(25, 4, "ITEMS");

    for (int b = 0; b < NUM_BUCKETS; b++) {
        char buf[4] = "X ";
        buf[1] = '0' + game_bucket_mult(&game, b);
        text_style(6 + b * 4, 18, buf, TXT_GOLD);
    }
}

// ---------------------------------------------------------------- saving

static void record_run(void)
{
    int changed = 0;
    if (game.round > save.best_round) {
        save.best_round = game.round;
        changed = 1;
    }
    if (game.total_score > save.best_score) {
        save.best_score = game.total_score;
        changed = 1;
    }
    if (changed) save_write();
}

// ---------------------------------------------------------------- title, menus, credits

static const char *const main_items[MAIN_COUNT] = { "PLAY", "HOW TO PLAY", "CREDITS" };
static const char *const pause_items[PAUSE_COUNT] = { "RESUME", "QUIT" };

static void draw_title_text(void)
{
    text_clear();
    if (save.best_round > 0) {
        char buf[32], *p = put_str(buf, "BEST ROUND ");
        p = put_num(p, save.best_round);
        p = put_str(p, "  SCORE ");
        put_num(p, save.best_score);
        text_center(17, buf, TXT_GOLD);
    }
}

static void draw_main_menu(void) { menu_draw(9, 16, 0, main_items, MAIN_COUNT, menu_sel); }
static void draw_pause_menu(void) { menu_draw(6, 14, "PAUSED", pause_items, PAUSE_COUNT, menu_sel); }

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
    game.npegs = 0;
    game.nitems = 0;
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
    REG_BLDCNT = BLD_DARKEN | BLD_BG0;    // keep the example pegs bright
    REG_BLDY = 12;
    panel(1, 28, 18);
    text_center(2, "HOW TO PLAY", TXT_HILITE);
    text_style(3, 4, "LEFT RIGHT TO AIM NUBBY", TXT_PANEL);
    text_style(3, 5, "A TO DROP", TXT_PANEL);
    text_style(6, 7, "WHITE PEG   +1 POINT", TXT_PANEL);
    text_style(6, 8, "PURPLE PEG  +1 MULT", TXT_PANEL);
    text_style(6, 9, "GOLD PEG    +1 COIN", TXT_PANEL);
    text_style(6, 10, "BUMPER      BOUNCE +1", TXT_PANEL);
    text_style(3, 12, "DROP SCORE =", TXT_PANEL);
    text_style(3, 13, "POINTS X MULT X BUCKET", TXT_HILITE);
    text_style(3, 15, "REACH THE GOAL TO WIN", TXT_PANEL);
    text_style(3, 16, "COINS BUY ITEMS", TXT_PANEL);
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

static void start_round(void)
{
    fade(1);
    music_stop();
    game_start_round(&game);
    int board = (game.round - 1) % NUM_BOARDS;
    load_bg(board_pal[board], board_tiles[board]);
    dim(0, 0);
    text_clear();
    draw_hud();
    aim_x = (BOARD_L + BOARD_R) / 2;
    state = ST_AIM;
    fade(0);
    music_play(game.round >= 5 ? SONG_OVERTIME : SONG_FACTORY);
}

static void new_run(void)
{
    best_at_start = save.best_score;
    game_new_run(&game, seed ^ (uint32_t)frames * 2654435761u);
    start_round();
}

static void show_result(void)
{
    char buf[32], *p = buf;
    p = put_num(p, game.points);
    p = put_str(p, " X ");
    p = put_num(p, game.mult);
    p = put_str(p, " X ");
    put_num(p, game_bucket_mult(&game, game.last_bucket));
    panel(7, 16, 5);
    text_center(8, buf, TXT_PANEL);
    p = put_str(buf, "+");
    put_num(p, game.last_score);
    text_center(10, buf, TXT_HILITE);
}

static void game_over(void)
{
    char buf[32], *p;
    state = ST_OVER;
    sfx_over();
    record_run();
    text_clear();
    draw_hud();
    panel(5, 18, 10);
    text_center(6, "GAME OVER", TXT_HILITE);
    p = put_str(buf, "ROUND ");
    put_num(p, game.round);
    text_center(8, buf, TXT_PANEL);
    p = put_str(buf, "SCORE ");
    put_num(p, game.total_score);
    text_center(9, buf, TXT_PANEL);
    if (game.total_score > best_at_start) text_center(11, "NEW BEST!", TXT_HILITE);
    text_center(13, "PRESS START", TXT_PANEL);
}

// ---------------------------------------------------------------- shop

static void draw_shop(void)
{
    char buf[32], *p;
    panel(1, 28, 18);
    text_center(2, "SHOP", TXT_HILITE);
    p = put_str(buf, "COINS ");
    put_num(p, game.coins);
    text_center(3, buf, TXT_GOLD);
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
        text_style(6, row, "              ", TXT_PANEL);
        if (item < 0) {
            text_style(6, row, "SOLD", TXT_PANEL);
            continue;
        }
        text_style(6, row, item_info[item].name, style);
        p = put_num(buf, item_info[item].price);
        put_str(p, " COINS");
        text_style(17, row, "         ", TXT_PANEL);
        text_style(17, row, buf, game.coins >= item_info[item].price ? TXT_PANEL : TXT_PLAIN);
    }
    // description of the highlighted item
    text_style(2, 16, "                        ", TXT_PANEL);
    text_style(2, 17, "                        ", TXT_PANEL);
    if (menu_sel < SHOP_SLOTS && game.shop[menu_sel] >= 0) {
        text_center(16, item_info[game.shop[menu_sel]].desc, TXT_HILITE);
        p = put_str(buf, "ITEMS ");
        p = put_num(p, game.nitems);
        p = put_str(p, " OF ");
        put_num(p, MAX_ITEMS);
        text_center(17, buf, TXT_PANEL);
    }
}

static void open_shop(void)
{
    fade(1);
    game_roll_shop(&game);
    state = ST_SHOP;
    menu_sel = 0;
    text_clear();
    dim(0, 0);
    draw_shop();
    // the shop sits over a darkened board
    REG_BLDCNT = BLD_DARKEN | BLD_BG0;
    REG_BLDY = 12;
    for (int i = 0; i <= 16; i += 2) frame();
    music_play(SONG_SHOP);
}

static void update_shop(uint16_t pressed)
{
    if (menu_move(pressed, SHOP_SLOTS + 1)) draw_shop();
    if (!(pressed & KEY_A)) return;
    if (menu_sel == SHOP_SLOTS) {
        start_round();
        return;
    }
    if (game_buy(&game, menu_sel)) {
        sfx_buy();
    } else {
        sfx_deny();
    }
    draw_shop();
}

// ---------------------------------------------------------------- play

static void update_aim(uint16_t pressed)
{
    if (pressed & KEY_START) {
        paused_from = state;
        state = ST_PAUSE;
        menu_sel = PAUSE_RESUME;
        music_stop();
        dim(1, PAUSE_DIM);
        draw_pause_menu();
        return;
    }
    if (keys & KEY_LEFT) aim_x -= 2;
    if (keys & KEY_RIGHT) aim_x += 2;
    if (keys & KEY_L) aim_x -= 1;         // shoulders for fine adjustment
    if (keys & KEY_R) aim_x += 1;
    if (aim_x < BOARD_L + NUBBY_R + 1) aim_x = BOARD_L + NUBBY_R + 1;
    if (aim_x > BOARD_R - NUBBY_R - 1) aim_x = BOARD_R - NUBBY_R - 1;
    if (pressed & KEY_A) {
        game_drop(&game, aim_x);
        state = ST_FALL;
    }
}

static void update_fall(uint16_t pressed)
{
    if (pressed & KEY_START) {
        paused_from = state;
        state = ST_PAUSE;
        menu_sel = PAUSE_RESUME;
        music_stop();
        dim(1, PAUSE_DIM);
        draw_pause_menu();
        return;
    }
    Events ev;
    int landed = game_step(&game, &ev);
    if (ev.bumper) sfx_bumper();
    else if (ev.coin) sfx_peg(ev.hits, SFX_PEG_COIN);
    else if (ev.mult) sfx_peg(ev.hits, SFX_PEG_MULT);
    else if (ev.plus) sfx_peg(ev.hits, SFX_PEG_PLUS);
    draw_hud();
    if (landed) {
        sfx_land(game_bucket_mult(&game, game.last_bucket));
        draw_hud();
        show_result();
        state = ST_RESULT;
        timer = RESULT_FRAMES;
    }
}

static void update_result(void)
{
    if (--timer > 0) return;
    text_clear();
    draw_hud();
    RoundState rs = game_round_state(&game);
    if (rs == ROUND_CLEARED) {
        char buf[32], *p;
        int earned = game_finish_round(&game);
        record_run();
        sfx_clear();
        draw_hud();
        panel(6, 18, 6);
        text_center(7, "ROUND CLEAR!", TXT_HILITE);
        p = put_str(buf, "+");
        p = put_num(p, earned);
        put_str(p, " COINS");
        text_center(9, buf, TXT_PANEL);
        state = ST_CLEAR;
        timer = CLEAR_FRAMES;
    } else if (rs == ROUND_FAILED) {
        game_over();
    } else {
        state = ST_AIM;
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
    } else if ((pressed & (KEY_A | KEY_START)) && menu_sel == PAUSE_QUIT) {
        record_run();
        go_title();
    }
}

static void update_title(uint16_t pressed)
{
    if (state == ST_TITLE) {
        text_center(16, (frames & 32) ? "           " : "PRESS START", TXT_PLAIN);
        if (pressed & (KEY_START | KEY_A)) {
            state = ST_MENU;
            menu_sel = MAIN_PLAY;
            text_center(16, "           ", TXT_PLAIN);
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
        case ST_FALL:
            update_fall(pressed);
            break;
        case ST_RESULT:
            update_result();
            break;
        case ST_CLEAR:
            if (--timer <= 0) open_shop();
            break;
        case ST_SHOP:
            update_shop(pressed);
            break;
        case ST_PAUSE:
            update_pause(pressed);
            break;
        case ST_OVER:
            if (pressed & KEY_START) go_title();
            break;
        }

        frame();
    }
}
