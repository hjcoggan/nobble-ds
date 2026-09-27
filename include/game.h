// Nubby's run: launch physics, popping pegs, quotas, restocks, items, perks
// and the shop. Hardware independent so it can be tested on the host.
//
// Pegs hold powers of two. A hit scores the peg's value and halves it; a 1
// disappears. Each round you must reach the quota in a single launch. Beat
// it and the board restocks (once per multiple of the quota): empty slots
// fill up and matching numbers merge into bigger ones. Miss it and you lose
// a life and the board resets for another try.
//
// Items fire on a trigger (on launch, first pop, wall bounce, ...). Perks
// force-trigger items: all of them, or ones picked at random, so the order
// you buy items in never matters.
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

// board geometry, in pixels: the whole 256x192 bottom (touch) screen
#define BOARD_L 10
#define BOARD_R 246
#define CEILING_Y 12
#define LAUNCH_X 128
#define LAUNCH_Y 22
#define FLOOR_Y 180          // where SPRINGS bounces Nubby back up
#define EXIT_Y 200           // Nubby is gone once it falls past this
#define NUM_SLOTS 33         // six rows of 5 and 6 pegs
#define CENTRE_SLOT 2        // top row, straight below the launcher
#define PEG_R 9
#define NUBBY_R 5
#define BIG_NUBBY_R 7

#define AIM_MAX 56           // aim angle limit either side of straight down (256 = full turn)
#define START_LIVES 3
#define MAX_ITEMS 5
#define MAX_PERKS 4
#define SHOP_SLOTS 3
#define PERK_CHOICES 2
#define SHOP_EVERY 3         // rounds between shops
#define PERK_EVERY 5         // rounds between perks
#define MAX_RESTOCKS 9
#define LAUNCH_TIMEOUT (60 * 20)
#define FLASH_FRAMES 8
#define BOSS_EVERY 5         // every 5th round is a boss round
#define BOSS_BONUS 3         // extra coins for beating one
#define NUM_ROWS 6           // peg rows, for the laser
#define LASER_WARN 30        // frames of warning before the laser fires
#define LASER_BEAM 12        // frames the beam stays on screen

typedef struct { int16_t x, y; } Slot;
extern const Slot slots[NUM_SLOTS];

// when an item fires
enum {
    TRIG_PASSIVE,       // always on
    TRIG_LAUNCH,
    TRIG_FIRST_POP,
    TRIG_PEG_GONE,      // a peg is popped away completely
    TRIG_WALL,          // Nubby bounces off a side wall
    TRIG_DIES,          // Nubby falls out of the board
    TRIG_EVERY_8,       // every 8 pegs popped
    NUM_TRIGGERS
};

enum {
    ITEM_SPRINGS,   // dies: bounce back up (once per launch)
    ITEM_SEEDER,    // launch: add a peg to an empty slot
    ITEM_PUMP,      // launch: double the lowest peg
    ITEM_ZAPPER,    // first pop: pop the highest peg
    ITEM_DOUBLER,   // first pop: double a random peg
    ITEM_RICOCHET,  // wall bounce: pop a random peg
    ITEM_PIGGY,     // peg gone: 1 in 4 chance of a coin
    ITEM_ENCORE,    // dies: +25% of this launch's score
    ITEM_CHAIN,     // every 8 pops: double a random peg
    ITEM_BIG,       // passive: Nubby is bigger
    ITEM_HEART,     // passive: +1 life now and +1 to the life cap
    NUM_ITEMS
};

enum {
    PERK_CHEESY,    // every 3 seconds in flight: trigger all items
    PERK_CHAOTIC,   // every second in flight: trigger a random item
    PERK_WAFFLE,    // first pop: trigger 2 random items
    PERK_KEBAB,     // Nubby dies: 50% chance to trigger a random item
    PERK_SPRINGY,   // wall bounce: 1 in 4 chance to trigger a random item
    PERK_TROPHY,    // passing the goal: trigger all items
    PERK_BUCKSHOT,  // first pop on the biggest peg: 3 random items
    PERK_HOUSE,     // 15 pegs popped: trigger all items
    NUM_PERKS
};

// Boss rounds bring a hazard to the board.
enum {
    BOSS_NONE,
    BOSS_LASER,     // a laser wipes out a row of pegs every couple of seconds
    BOSS_WIND,      // gusts push Nubby sideways, switching direction
    BOSS_ARMOR,     // some pegs are armoured: the first hit only breaks the armour
    NUM_BOSSES
};

typedef struct {
    const char *name;
    const char *line1, *line2;
} BossInfo;

extern const BossInfo boss_info[NUM_BOSSES];

typedef struct {
    const char *name;
    const char *effect;
    uint8_t trigger;
    uint8_t price;
} ItemInfo;

typedef struct {
    const char *name;
    const char *line1, *line2;
} PerkInfo;

extern const ItemInfo item_info[NUM_ITEMS];
extern const char *const trigger_text[NUM_TRIGGERS];
extern const PerkInfo perk_info[NUM_PERKS];

// What happened during a step, for sound and effects.
typedef struct {
    uint8_t pop;        // a peg was hit (and maybe vanished)
    uint8_t gone;       // ...and it disappeared
    uint8_t wall;       // Nubby bounced off a wall
    uint8_t spring;     // the springs fired
    uint8_t item;       // an item fired
    uint8_t laser;      // the boss laser fired
    uint8_t armor;      // an armoured peg lost its armour
    int hits;           // hits so far this launch
} Events;

typedef enum { RESULT_CLEARED, RESULT_RETRY, RESULT_GAME_OVER } Result;

typedef struct {
    // run
    int round, quota, lives, max_lives, coins;
    uint8_t items[MAX_ITEMS];
    int nitems;
    uint8_t perks[MAX_PERKS];
    int nperks;
    uint32_t rng;

    // board: 0 = empty slot
    int32_t pegs[NUM_SLOTS];
    int32_t round_start[NUM_SLOTS];     // restored after a failed launch
    uint8_t cooldown[NUM_SLOTS];        // frames before a peg can be hit again
    uint8_t flash[NUM_SLOTS];
    uint8_t item_flash[MAX_ITEMS];      // for the HUD
    uint8_t perk_flash[MAX_PERKS];

    // current launch (positions and velocities are 8.8 fixed point)
    int flying;
    int32_t x, y, vx, vy;
    int score, hits, frames, still, spring_used, ricochets, passed_goal;
    int depth;                          // guards items triggering each other forever
    Events *ev;

    // result of the last launch
    int restocks, perfect;

    // boss round hazards
    int boss;
    uint8_t armor[NUM_SLOTS], armor_start[NUM_SLOTS];
    int wind;                           // -1 left, 1 right
    int laser_row, laser_timer;         // row -1 when idle; timer counts the warning then the beam

    int shop[SHOP_SLOTS];               // item ids, -1 once bought
    int perk_offer[PERK_CHOICES];
} Game;

uint32_t game_rand(Game *g);
int game_has(const Game *g, int item);
int game_has_perk(const Game *g, int perk);
int game_radius(const Game *g);
int game_potential(const Game *g);      // points left on the board
int game_boss_for(int round);           // BOSS_NONE or the hazard for that round
int game_row_y(int row);

void game_new_run(Game *g, uint32_t seed);
void game_launch(Game *g, int angle);   // angle: 0 = straight down, +/- AIM_MAX
int game_step(Game *g, Events *ev);     // one frame; returns 1 when Nubby falls out
Result game_resolve(Game *g);           // after a launch: restock or lose a life

int game_shop_due(const Game *g);       // a shop comes before this round
void game_roll_shop(Game *g);
int game_buy(Game *g, int slot);        // 1 if bought
// Buy with full hands: the item in `replace` is thrown away. 1 if bought.
int game_buy_swap(Game *g, int slot, int replace);
int game_refund(int item);              // coins back for an item swapped away

int game_perk_due(const Game *g);       // a perk choice comes before this round
void game_roll_perks(Game *g);
void game_take_perk(Game *g, int choice);

// The aim angle that points the launcher at screen position (x, y), clamped
// to +/- AIM_MAX. Used for touch aiming.
int game_aim_at(int x, int y);

// Up to n points along the first part of a launch, for the aim guide.
int game_predict(const Game *g, int angle, int16_t *xs, int16_t *ys, int n);

#endif
