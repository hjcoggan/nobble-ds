// Nubby's run: pegboard physics, scoring, rounds, items and the shop.
// Hardware independent so it can be tested on the host.
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

// board geometry, in pixels
#define BOARD_L 40
#define BOARD_R 200
#define DROP_Y 16            // where Nubby hangs before a drop
#define CEILING_Y 6
#define BUCKET_Y 140         // top of the bucket dividers
#define FLOOR_Y 152          // Nubby has landed once its centre passes this
#define NUM_BUCKETS 5
#define BUCKET_W ((BOARD_R - BOARD_L) / NUM_BUCKETS)
#define NUBBY_R 4             // smaller than the 10px gap between pegs in a row
#define PEG_R 3
#define BUMPER_R 6
#define MAX_PEGS 80

#define BASE_DROPS 5
#define MAX_ITEMS 4
#define SHOP_SLOTS 3
#define DROP_TIMEOUT (60 * 12)   // frames before a stuck Nubby is dropped in a bucket

enum { PEG_PLUS, PEG_MULT, PEG_COIN, PEG_BUMPER };

enum {
    ITEM_HEAVY,     // white pegs worth 2
    ITEM_MULTI,     // 2 more purple pegs
    ITEM_EXTRA,     // 1 more drop per round
    ITEM_SPRING,    // 2 more bumpers, bumpers worth 3
    ITEM_LUCKY,     // every 7th peg in a drop gives +7
    ITEM_PIGGY,     // 1 coin per 4 saved at round end
    ITEM_BOOST,     // each drop starts with 5 points
    ITEM_BUCKET,    // middle bucket x5
    NUM_ITEMS
};

typedef struct {
    const char *name;
    const char *desc;
    int price;
} ItemInfo;

extern const ItemInfo item_info[NUM_ITEMS];

typedef struct {
    int16_t x, y;
    uint8_t type;
    uint8_t lit;             // already scored this drop
    uint8_t flash;           // frames of hit flash left (bumpers: hit cooldown)
} Peg;

// What happened during a step, for sound and effects.
typedef struct {
    uint8_t plus, mult, coin, bumper;   // pegs hit this step, by type
    uint8_t lucky;                      // a lucky 7th peg paid out
    uint8_t landed;
    int hits;                           // pegs hit so far this drop
} Events;

typedef enum { ROUND_PLAYING, ROUND_CLEARED, ROUND_FAILED } RoundState;

typedef struct {
    // run
    int round, target, round_score, total_score, coins, drops_left;
    uint8_t items[MAX_ITEMS];
    int nitems;
    uint32_t rng;

    // board
    Peg pegs[MAX_PEGS];
    int npegs;

    // current drop (positions and velocities are 8.8 fixed point)
    int dropping;
    int32_t x, y, vx, vy;
    int points, mult, hits, frames, slot, still, bumps;
    int last_score, last_bucket;      // result of the most recent drop

    // shop
    int shop[SHOP_SLOTS];             // item ids, -1 once bought
} Game;

uint32_t game_rand(Game *g);
int game_has(const Game *g, int item);
int game_bucket_mult(const Game *g, int slot);
int game_target(int round);

void game_new_run(Game *g, uint32_t seed);
void game_start_round(Game *g);       // new board, goal and drops
void game_drop(Game *g, int x);       // release Nubby at pixel x
int game_step(Game *g, Events *ev);   // one frame; returns 1 when Nubby lands
RoundState game_round_state(const Game *g);
int game_finish_round(Game *g);       // pays out coins, returns the amount

void game_roll_shop(Game *g);
int game_buy(Game *g, int slot);      // 1 if bought

#endif
