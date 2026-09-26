#include <stdio.h>
#include "game.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static int random_angle(Game *g) { return (int)(game_rand(g) % (2 * AIM_MAX + 1)) - AIM_MAX; }

// Launch and run until Nubby falls out, checking it stays inside the board.
static int run_launch(Game *g, int angle)
{
    Events ev;
    game_launch(g, angle);
    int f = 0;
    while (!game_step(g, &ev)) {
        f++;
        int px = g->x >> 8, py = g->y >> 8;
        if (px < BOARD_L || px > BOARD_R || py < CEILING_Y) {
            printf("escaped at (%d,%d)\n", px, py);
            failures++;
            break;
        }
    }
    return f;
}

static void test_launches(void)
{
    Game g;
    long frames = 0, hits = 0, share = 0;
    int n = 0, timeouts = 0;
    for (int run = 0; run < 400; run++) {
        game_new_run(&g, 1000 + run);
        int potential = game_potential(&g);
        int f = run_launch(&g, random_angle(&g));
        frames += f;
        hits += g.hits;
        share += potential ? g.score * 100 / potential : 0;
        timeouts += f >= LAUNCH_TIMEOUT;
        n++;
    }
    printf("  %d launches: avg %ld frames, %ld.%ld hits, %ld%% of the board scored, %d timeouts\n",
           n, frames / n, hits / n, hits * 10 / n % 10, share / n, timeouts);
    CHECK(timeouts == 0);
    CHECK(hits / n >= 3);
}

static void test_popping(void)
{
    Game g;
    game_new_run(&g, 3);
    for (int i = 0; i < NUM_SLOTS; i++) g.pegs[i] = 0;
    g.pegs[5] = 8;                                   // top centre peg, right below the launcher
    Events ev;
    game_launch(&g, 0);
    while (!game_step(&g, &ev) && !ev.pop) {}
    CHECK(g.score == 8);
    CHECK(g.pegs[5] == 4);

    g.pegs[5] = 1;
    game_launch(&g, 0);
    while (!game_step(&g, &ev) && !ev.pop) {}
    CHECK(g.score == 1 && g.pegs[5] == 0 && ev.gone);
}

static void test_restock_and_merge(void)
{
    Game g;
    game_new_run(&g, 5);
    for (int i = 0; i < NUM_SLOTS; i++) g.pegs[i] = 0;
    g.pegs[0] = 1;
    g.pegs[1] = 2;
    g.quota = 10;
    g.score = 25;                                     // 2 restocks
    g.flying = 0;
    int coins = g.coins;
    g.lives = 2;
    Result r = game_resolve(&g);
    CHECK(r == RESULT_CLEARED);
    CHECK(g.restocks == 2);
    CHECK(g.coins == coins + 2);
    CHECK(g.lives == 3);
    CHECK(g.round == 2);
    // restock 1 fills the board with 1s and merges into the existing 1;
    // restock 2 then turns every new 1 into a 2
    int filled = 0;
    for (int i = 0; i < NUM_SLOTS; i++) filled += g.pegs[i] != 0;
    CHECK(filled == NUM_SLOTS);
    for (int i = 0; i < NUM_SLOTS; i++) CHECK(g.pegs[i] == 2);
    CHECK(g.quota == game_potential(&g) * 18 / 100);
}

static void test_failed_launch_resets(void)
{
    Game g;
    game_new_run(&g, 9);
    int32_t before[NUM_SLOTS];
    for (int i = 0; i < NUM_SLOTS; i++) before[i] = g.pegs[i];
    g.pegs[3] = g.pegs[3] ? 0 : 1;
    g.score = 0;
    Result r = game_resolve(&g);
    CHECK(r == RESULT_RETRY);
    CHECK(g.lives == START_LIVES - 1);
    for (int i = 0; i < NUM_SLOTS; i++) CHECK(g.pegs[i] == before[i]);
    g.lives = 1;
    g.score = 0;
    CHECK(game_resolve(&g) == RESULT_GAME_OVER);
}

static void test_perfect_pop(void)
{
    Game g;
    game_new_run(&g, 11);
    for (int i = 0; i < NUM_SLOTS; i++) g.pegs[i] = 0;
    g.score = 6;
    g.quota = 10;
    CHECK(game_resolve(&g) == RESULT_CLEARED);      // 6 doubled to 12
    CHECK(g.perfect);
}

static void test_runs(void)
{
    // random aim, no shopping: how far do runs get?
    int total_rounds = 0, max_round = 0, runs = 300, first_cleared = 0, big = 0;
    for (int run = 0; run < runs; run++) {
        Game g;
        game_new_run(&g, 777 + run);
        for (;;) {
            run_launch(&g, random_angle(&g));
            int round = g.round;
            Result r = game_resolve(&g);
            if (r == RESULT_CLEARED && round == 1) first_cleared++;
            if (r == RESULT_GAME_OVER || g.round > 60) break;
        }
        for (int i = 0; i < NUM_SLOTS; i++)
            if (g.pegs[i] > big) big = g.pegs[i];
        total_rounds += g.round;
        if (g.round > max_round) max_round = g.round;
    }
    printf("  random-aim runs: round 1 cleared %d/%d, average end round %d, best %d, biggest peg %d\n",
           first_cleared, runs, total_rounds / runs, max_round, big);
    CHECK(first_cleared > runs / 2);
}

static void test_shop(void)
{
    Game g;
    game_new_run(&g, 7);
    g.items[g.nitems++] = ITEM_FIRST;
    for (int t = 0; t < 50; t++) {
        game_roll_shop(&g);
        for (int s = 0; s < SHOP_SLOTS; s++) {
            CHECK(g.shop[s] >= 0 && g.shop[s] != ITEM_FIRST);
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

    game_new_run(&g, 8);
    g.coins = 100;
    g.shop[0] = ITEM_HEART;
    CHECK(game_buy(&g, 0));
    CHECK(g.max_lives == START_LIVES + 1 && g.lives == START_LIVES + 1);

    g.round = 4;
    CHECK(game_shop_due(&g));
    g.round = 5;
    CHECK(!game_shop_due(&g));
}

static void test_predict(void)
{
    Game g;
    game_new_run(&g, 1);
    int16_t xs[8], ys[8];
    int n = game_predict(&g, AIM_MAX, xs, ys, 8);
    CHECK(n > 0);
    for (int i = 1; i < n; i++) CHECK(ys[i] >= ys[i - 1] - 1);
}

int main(void)
{
    test_launches();
    test_popping();
    test_restock_and_merge();
    test_failed_launch_resets();
    test_perfect_pop();
    test_runs();
    test_shop();
    test_predict();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all game tests passed\n");
    return 0;
}
