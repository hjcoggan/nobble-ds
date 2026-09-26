// Nubby's run: launch physics, popping pegs, quotas, restocks, items and
// the shop. Hardware independent so it can be tested on the host.
//
// Pegs hold powers of two. A hit scores the peg's value and halves it; a 1
// disappears. Each round you must reach the quota in a single launch. Beat
// it and the board restocks (once per multiple of the quota): empty slots
// fill up and matching numbers merge into bigger ones. Miss it and you lose
// a life and the board resets for another try.
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

// board geometry, in pixels
#define BOARD_L 40
#define BOARD_R 200
#define CEILING_Y 10
#define LAUNCH_X 120
#define LAUNCH_Y 18
#define FLOOR_Y 156          // SPRINGS bounces Nubby here once per launch
#define EXIT_Y 168           // Nubby is gone once it falls past this
#define NUM_SLOTS 21
#define PEG_R 7
#define NUBBY_R 4
#define BIG_NUBBY_R 6

#define AIM_MAX 56           // aim angle limit either side of straight down (256 = full turn)
#define START_LIVES 3
#define MAX_LIVES 5
#define MAX_ITEMS 4
#define SHOP_SLOTS 3
#define SHOP_EVERY 3         // rounds between shops
#define MAX_RESTOCKS 9
#define LAUNCH_TIMEOUT (60 * 20)

typedef struct { int16_t x, y; } Slot;
extern const Slot slots[NUM_SLOTS];

enum {
    ITEM_SPRINGS,   // the floor bounces Nubby back up once per launch
    ITEM_WALLS,     // wall bounces score +3
    ITEM_PUMP,      // the lowest peg doubles at the start of each round
    ITEM_BIG,       // Nubby is bigger
    ITEM_FIRST,     // the first hit of each launch scores x3
    ITEM_FLOATY,    // lower gravity
    ITEM_HEART,     // +1 life now and +1 to the life cap
    ITEM_RICH,      // +1 coin per restock
    NUM_ITEMS
};

typedef struct {
    const char *name;
    const char *desc;
    int price;
} ItemInfo;

extern const ItemInfo item_info[NUM_ITEMS];

// What happened during a step, for sound and effects.
typedef struct {
    uint8_t pop;        // a peg was hit (and maybe vanished)
    uint8_t gone;       // ...and it disappeared
    uint8_t wall;       // Nubby bounced off a wall
    uint8_t spring;     // the floor spring fired
    int value;          // points from the last peg hit
    int hits;           // hits so far this launch
} Events;

typedef enum { RESULT_CLEARED, RESULT_RETRY, RESULT_GAME_OVER } Result;

typedef struct {
    // run
    int round, quota, lives, max_lives, coins;
    uint8_t items[MAX_ITEMS];
    int nitems;
    uint32_t rng;

    // board: 0 = empty slot
    int32_t pegs[NUM_SLOTS];
    int32_t round_start[NUM_SLOTS];     // restored after a failed launch
    uint8_t cooldown[NUM_SLOTS];        // frames before a peg can be hit again
    uint8_t flash[NUM_SLOTS];

    // current launch (positions and velocities are 8.8 fixed point)
    int flying;
    int32_t x, y, vx, vy;
    int score, hits, frames, still, spring_used;

    // result of the last launch
    int restocks, perfect;

    int shop[SHOP_SLOTS];               // item ids, -1 once bought
} Game;

uint32_t game_rand(Game *g);
int game_has(const Game *g, int item);
int game_radius(const Game *g);
int game_potential(const Game *g);      // points left on the board

void game_new_run(Game *g, uint32_t seed);
void game_launch(Game *g, int angle);   // angle: 0 = straight down, +/- AIM_MAX
int game_step(Game *g, Events *ev);     // one frame; returns 1 when Nubby falls out
Result game_resolve(Game *g);           // after a launch: restock or lose a life
int game_shop_due(const Game *g);       // a shop comes before this round
void game_roll_shop(Game *g);
int game_buy(Game *g, int slot);        // 1 if bought

// Up to n points along the first part of a launch, for the aim guide.
int game_predict(const Game *g, int angle, int16_t *xs, int16_t *ys, int n);

#endif
