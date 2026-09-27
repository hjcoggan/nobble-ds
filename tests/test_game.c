#include <stdio.h>
#include "game.h"

static int failures;

#define FIX_TEST(v) ((v) * 256)

// Tests that need known peg positions use the pyramid layout: slot 0 sits
// straight below the launcher, 3 and 5 are either side of the third row.
#define PYRAMID 5
#define CENTRE 0
#define LOW 3
#define HIGH 5

static void empty_pyramid(Game *g)
{
    game_set_layout(g, PYRAMID);
    for (int i = 0; i < NUM_SLOTS; i++) g->pegs[i] = 0;
}

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
    empty_pyramid(&g);
    g.pegs[CENTRE] = 8;                                   // top centre peg, right below the launcher
    Events ev;
    game_launch(&g, 0);
    while (!game_step(&g, &ev) && !ev.pop) {}
    CHECK(g.score == 8);
    CHECK(g.pegs[CENTRE] == 4);

    g.pegs[CENTRE] = 1;
    game_launch(&g, 0);
    while (!game_step(&g, &ev) && !ev.pop) {}
    CHECK(g.score == 1 && g.pegs[CENTRE] == 0 && ev.gone);
}

static void test_restock_and_merge(void)
{
    Game g;
    game_new_run(&g, 5);
    empty_pyramid(&g);
    g.pegs[0] = 1;
    g.pegs[1] = 1;
    g.pegs[2] = 2;
    g.pegs[3] = 4;
    g.quota = 10;
    g.score = 15;                                     // 1 restock
    int coins = g.coins;
    g.lives = 2;
    Result r = game_resolve(&g);
    CHECK(r == RESULT_CLEARED);
    CHECK(g.restocks == 1);
    CHECK(g.coins == coins + 1);
    CHECK(g.lives == 3);
    CHECK(g.round == 2);
    // the two 1s merge into a 2 and leave a gap; the 2 and the 4 have no
    // partner; then every empty slot gets a new 1
    CHECK(g.pegs[0] == 2 && g.pegs[1] == 1 && g.pegs[2] == 2 && g.pegs[3] == 4);
    for (int i = 4; i < g.nslots; i++) CHECK(g.pegs[i] == 1);
    int expect = game_potential(&g) * 175 / 1000;
    CHECK(g.quota == (expect < 5 ? 5 : expect));

    // many restocks keep merging, so numbers climb quickly
    g.score = g.quota * 6;
    game_resolve(&g);
    int32_t big = 0;
    for (int i = 0; i < g.nslots; i++)
        if (g.pegs[i] > big) big = g.pegs[i];
    CHECK(big >= 16);

    // changing layout keeps the biggest pegs
    game_set_layout(&g, 6);
    int32_t big2 = 0;
    for (int i = 0; i < g.nslots; i++)
        if (g.pegs[i] > big2) big2 = g.pegs[i];
    CHECK(big2 == big && g.nslots == layouts[6].n);
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
    g.items[g.nitems++] = ITEM_ZAPPER;
    for (int t = 0; t < 50; t++) {
        game_roll_shop(&g);
        for (int s = 0; s < SHOP_SLOTS; s++) {
            CHECK(g.shop[s] >= 0 && g.shop[s] != ITEM_ZAPPER);
            for (int o = 0; o < s; o++) CHECK(g.shop[s] != g.shop[o]);
        }
    }
    g.coins = 100;
    int bought = 0;
    for (int t = 0; t < 3; t++) {
        game_roll_shop(&g);
        for (int s = 0; s < SHOP_SLOTS; s++) bought += game_buy(&g, s);
    }
    CHECK(g.nitems == MAX_ITEMS && bought == MAX_ITEMS - 1);
    game_roll_shop(&g);
    CHECK(!game_buy(&g, 0));                  // no room for a sixth item
    int wanted = g.shop[0], coins = g.coins, old = g.items[2];
    CHECK(game_buy_swap(&g, 0, 2));           // ...but it can replace one, for a partial refund
    CHECK(g.nitems == MAX_ITEMS && g.items[2] == wanted && g.shop[0] == -1);
    CHECK(game_refund(old) > 0 && game_refund(old) < item_info[old].price);
    CHECK(g.coins == coins - item_info[wanted].price + game_refund(old));

    game_new_run(&g, 12);                     // swapping a heart away takes its life
    g.coins = 100;
    g.shop[0] = ITEM_HEART;
    game_buy(&g, 0);
    g.shop[0] = ITEM_PUMP;
    CHECK(game_buy_swap(&g, 0, 0));
    CHECK(g.max_lives == START_LIVES && g.lives == START_LIVES);

    game_new_run(&g, 8);
    g.coins = 100;
    g.shop[0] = ITEM_HEART;
    CHECK(game_buy(&g, 0));
    CHECK(g.max_lives == START_LIVES + 1 && g.lives == START_LIVES + 1);

    g.round = 4;
    CHECK(game_shop_due(&g));
    g.round = 5;
    CHECK(!game_shop_due(&g));
    g.round = 6;
    CHECK(game_perk_due(&g));
}

static Game board_with(uint32_t seed, int item)
{
    Game g;
    game_new_run(&g, seed);
    empty_pyramid(&g);
    g.pegs[LOW] = 1;
    g.pegs[HIGH] = 8;
    g.pegs[CENTRE] = 2;                               // below the launcher
    if (item >= 0) g.items[g.nitems++] = (uint8_t)item;
    return g;
}

static void first_pop(Game *g)
{
    Events ev;
    game_launch(g, 0);
    while (!game_step(g, &ev) && g->hits == 0) {}
}

static void test_items(void)
{
    Game g = board_with(1, ITEM_PUMP);           // launch: double the lowest peg
    game_launch(&g, 0);
    CHECK(g.pegs[LOW] == 2 && g.item_flash[0]);

    g = board_with(2, ITEM_SEEDER);              // launch: add a peg
    int before = 0, after = 0;
    for (int i = 0; i < NUM_SLOTS; i++) before += g.pegs[i] != 0;
    game_launch(&g, 0);
    for (int i = 0; i < NUM_SLOTS; i++) after += g.pegs[i] != 0;
    CHECK(after == before + 1);

    g = board_with(3, ITEM_ZAPPER);              // first pop: pop the highest peg
    first_pop(&g);
    CHECK(g.pegs[HIGH] == 4);
    CHECK(g.score == 2 + 8);

    g = board_with(4, ITEM_ENCORE);              // dies: +25% of the launch score
    Events ev;
    game_launch(&g, 0);
    while (!game_step(&g, &ev)) {}
    CHECK(g.score >= 2 && g.hits >= 1);

    g = board_with(5, ITEM_SPRINGS);             // dies: bounce back up once
    game_launch(&g, 0);
    int springs = 0;
    while (!game_step(&g, &ev)) springs += ev.spring;
    CHECK(springs == 1);
}

static void test_perks(void)
{
    // waffle: first pop triggers 2 random items. Pump normally fires on
    // launch only, so its flash after the first pop comes from the perk.
    Game g = board_with(6, ITEM_PUMP);
    g.perks[g.nperks++] = PERK_WAFFLE;
    game_launch(&g, 0);
    g.item_flash[0] = 0;
    Events ev;
    while (!game_step(&g, &ev) && g.hits == 0) {}
    CHECK(g.item_flash[0] && g.perk_flash[0]);

    // trophy: passing the goal triggers every item. Encore normally waits
    // for Nubby to fall out, so extra score right after the first pop can
    // only have come from the perk.
    g = board_with(7, ITEM_PIGGY);
    g.items[g.nitems++] = ITEM_ENCORE;
    g.perks[g.nperks++] = PERK_TROPHY;
    g.quota = 1;
    first_pop(&g);
    CHECK(g.passed_goal);
    CHECK(g.item_flash[0] && g.item_flash[1]);
    CHECK(g.score > 2);                          // encore added to the 2 points

    game_new_run(&g, 9);
    game_roll_perks(&g);
    CHECK(g.perk_offer[0] != g.perk_offer[1]);
    game_take_perk(&g, 1);
    CHECK(g.nperks == 1 && g.perks[0] == g.perk_offer[1]);
}

static void test_everything_terminates(void)
{
    // every perk and a full set of items: launches still end, scores stay sane
    long launches = 0, total = 0;
    for (int run = 0; run < 200; run++) {
        Game g;
        game_new_run(&g, 4242 + run);
        for (int k = 0; k < MAX_ITEMS; k++) g.items[g.nitems++] = (uint8_t)((run + k * 3) % NUM_ITEMS);
        for (int p = 0; p < MAX_PERKS; p++) g.perks[g.nperks++] = (uint8_t)((run + p * 2) % NUM_PERKS);
        for (int l = 0; l < 5; l++) {
            int f = run_launch(&g, random_angle(&g));
            CHECK(f <= LAUNCH_TIMEOUT + 1);
            CHECK(g.score >= 0);
            total += g.score;
            launches++;
            if (game_resolve(&g) == RESULT_GAME_OVER) break;
        }
    }
    printf("  loaded-up launches: %ld, average score %ld\n", launches, total / launches);
}

static void test_bosses(void)
{
    CHECK(game_boss_for(4) == BOSS_NONE && game_boss_for(5) == BOSS_LASER);
    CHECK(game_boss_for(10) == BOSS_WIND && game_boss_for(15) == BOSS_ARMOR);
    CHECK(game_boss_for(20) == BOSS_LASER);

    // laser: wipes out a band of pegs, scoring nothing, and the board comes back after a miss
    Game g;
    game_new_run(&g, 21);
    game_set_layout(&g, PYRAMID);
    g.round = 5;
    for (int i = 0; i < g.nslots; i++) g.pegs[i] = 1;
    g.boss = BOSS_LASER;
    g.lives = 3;
    g.score = 0;
    for (int i = 0; i < NUM_SLOTS; i++) g.round_start[i] = g.pegs[i];
    Events ev;
    game_launch(&g, AIM_MAX);
    int fired = 0;
    while (!game_step(&g, &ev)) fired |= ev.laser;
    int empty = 0;
    for (int i = 0; i < NUM_SLOTS; i++) empty += g.pegs[i] == 0;
    CHECK(fired && empty >= 3);
    g.quota = 1000;
    game_resolve(&g);
    for (int i = 0; i < g.nslots; i++) CHECK(g.pegs[i] == 1);

    // armour: the first hit cracks it and scores nothing
    game_new_run(&g, 22);
    empty_pyramid(&g);
    g.pegs[CENTRE] = 4;
    g.armor[CENTRE] = 1;
    game_launch(&g, 0);
    while (!game_step(&g, &ev) && !ev.armor) {}
    CHECK(g.armor[CENTRE] == 0 && g.pegs[CENTRE] == 4 && g.score == 0);

    // wind drifts a straight-down launch sideways
    game_new_run(&g, 23);
    for (int i = 0; i < NUM_SLOTS; i++) g.pegs[i] = 0;
    g.boss = BOSS_WIND;
    g.wind = 1;
    game_launch(&g, 0);
    for (int f = 0; f < 40; f++) game_step(&g, &ev);
    CHECK(g.x > FIX_TEST(LAUNCH_X + 4));

    // beating a boss round pays a bonus; how often do random-aim runs beat them?
    int tried = 0, beaten = 0;
    for (int run = 0; run < 300; run++) {
        game_new_run(&g, 900 + run);
        while (g.round <= 20) {
            int round = g.round, boss = g.boss;
            run_launch(&g, random_angle(&g));
            Result r = game_resolve(&g);
            if (boss) {
                tried++;
                beaten += r == RESULT_CLEARED;
            }
            (void)round;
            if (r == RESULT_GAME_OVER) break;
        }
    }
    printf("  boss launches: %d, beaten %d%%\n", tried, tried ? beaten * 100 / tried : 0);
    CHECK(tried > 0 && beaten > 0);
}

static void test_aim_at(void)
{
    CHECK(game_aim_at(LAUNCH_X, 150) == 0);                     // straight down
    CHECK(game_aim_at(LAUNCH_X - 60, 150) < 0);                 // left is negative
    CHECK(game_aim_at(LAUNCH_X + 60, 150) > 0);
    CHECK(game_aim_at(LAUNCH_X + 100, LAUNCH_Y + 1) == AIM_MAX); // clamped
    CHECK(game_aim_at(0, 0) == -AIM_MAX);
    int a = game_aim_at(LAUNCH_X + 50, LAUNCH_Y + 50);          // 45 degrees = 32 units
    CHECK(a >= 31 && a <= 33);
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
    test_items();
    test_perks();
    test_everything_terminates();
    test_bosses();
    test_aim_at();
    test_predict();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all game tests passed\n");
    return 0;
}
