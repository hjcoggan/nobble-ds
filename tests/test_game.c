#include <stdio.h>
#include "game.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

// Drop at x and run until Nubby lands, checking it stays on the board.
static int run_drop(Game *g, int x, int *frames)
{
    Events ev;
    game_drop(g, x);
    int f = 0;
    while (!game_step(g, &ev)) {
        f++;
        int px = g->x >> 8, py = g->y >> 8;
        if (px < BOARD_L || px > BOARD_R || py < 0 || py > 170) {
            printf("escaped at (%d,%d)\n", px, py);
            failures++;
            break;
        }
        if (f > DROP_TIMEOUT + 10) break;
    }
    *frames = f;
    return g->last_score;
}

static void test_drops_land(void)
{
    Game g;
    long total = 0, frames_total = 0;
    int n = 0, stuck = 0, max_frames = 0;
    int buckets[NUM_BUCKETS] = { 0 };
    for (int run = 0; run < 200; run++) {
        game_new_run(&g, 1000 + run);
        game_start_round(&g);
        for (int d = 0; d < BASE_DROPS; d++) {
            int x = BOARD_L + 8 + (int)(game_rand(&g) % (BOARD_R - BOARD_L - 16));
            int f;
            total += run_drop(&g, x, &f);
            frames_total += f;
            if (f > max_frames) max_frames = f;
            if (f >= DROP_TIMEOUT) stuck++;
            buckets[g.last_bucket]++;
            n++;
        }
    }
    printf("  %d drops: avg score %ld, avg %ld frames, max %d, stuck %d\n",
           n, total / n, frames_total / n, max_frames, stuck);
    printf("  buckets: %d %d %d %d %d\n", buckets[0], buckets[1], buckets[2], buckets[3], buckets[4]);
    CHECK(stuck < n / 100);
    CHECK(total / n > 10);
}

static void test_round_one_is_winnable(void)
{
    // with random aim and no items, most first rounds should be cleared
    int cleared = 0, runs = 300;
    for (int run = 0; run < runs; run++) {
        Game g;
        game_new_run(&g, 5000 + run);
        game_start_round(&g);
        while (game_round_state(&g) == ROUND_PLAYING) {
            int f, x = BOARD_L + 8 + (int)(game_rand(&g) % (BOARD_R - BOARD_L - 16));
            run_drop(&g, x, &f);
        }
        cleared += game_round_state(&g) == ROUND_CLEARED;
    }
    printf("  round 1 cleared by random play: %d/%d\n", cleared, runs);
    CHECK(cleared > runs * 6 / 10);
    CHECK(cleared < runs);
}

static void test_scoring_and_items(void)
{
    Game g;
    game_new_run(&g, 42);
    CHECK(game_bucket_mult(&g, 2) == 3);
    g.items[g.nitems++] = ITEM_BUCKET;
    CHECK(game_bucket_mult(&g, 2) == 5);
    CHECK(game_bucket_mult(&g, 0) == 2);

    game_start_round(&g);
    CHECK(g.drops_left == BASE_DROPS);
    g.items[g.nitems++] = ITEM_EXTRA;
    g.round = 0;
    game_start_round(&g);
    CHECK(g.drops_left == BASE_DROPS + 1);
    CHECK(g.round == 1);

    int bumpers = 0, mults = 0, coins = 0;
    for (int i = 0; i < g.npegs; i++) {
        bumpers += g.pegs[i].type == PEG_BUMPER;
        mults += g.pegs[i].type == PEG_MULT;
        coins += g.pegs[i].type == PEG_COIN;
    }
    CHECK(bumpers == 2 && mults == 3 && coins == 4);
    CHECK(g.npegs > 40 && g.npegs <= MAX_PEGS);
}

static void test_targets_rise(void)
{
    CHECK(game_target(1) == 90);
    for (int r = 1; r < 12; r++) CHECK(game_target(r + 1) > game_target(r));
}

static void test_shop(void)
{
    Game g;
    game_new_run(&g, 7);
    g.items[g.nitems++] = ITEM_LUCKY;
    for (int t = 0; t < 50; t++) {
        game_roll_shop(&g);
        for (int s = 0; s < SHOP_SLOTS; s++) {
            CHECK(g.shop[s] >= 0 && g.shop[s] != ITEM_LUCKY);
            for (int o = 0; o < s; o++) CHECK(g.shop[s] != g.shop[o]);
        }
    }
    g.coins = 100;
    game_roll_shop(&g);
    int bought = 0;
    for (int s = 0; s < SHOP_SLOTS; s++) bought += game_buy(&g, s);
    CHECK(bought == 3 && g.nitems == 4);
    game_roll_shop(&g);
    CHECK(!game_buy(&g, 0));                  // no room for a fifth item
    g.coins = 0;
    g.nitems = 0;
    game_roll_shop(&g);
    CHECK(!game_buy(&g, 0));                  // can't afford it
}

int main(void)
{
    test_drops_land();
    test_round_one_is_winnable();
    test_scoring_and_items();
    test_targets_rise();
    test_shop();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all game tests passed\n");
    return 0;
}
