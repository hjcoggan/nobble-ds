#include "game.h"

#define FIX(n) ((n) << 8)
#define GRAVITY 22                 // 8.8 px/frame^2
#define LAUNCH_SPEED (4 * 256)     // 8.8 px/frame
#define MAX_SPEED FIX(7)
#define SUBSTEPS 2
#define PEG_BOUNCE 230             // restitution off pegs, 8.8
#define WALL_BOUNCE 230
#define PEG_COOLDOWN 6
#define MAX_RICOCHETS 6            // ricochet pops per launch
#define WIND_PUSH 6          // sideways push per frame in the wind tunnel
#define WIND_FLIP 90         // frames between wind changes
#define LASER_EVERY 160      // frames between laser shots
#define LASER_BAND 12        // pegs this far above or below the beam are hit
#define LAYOUT_EVERY 3       // rounds on each layout
#define QUOTA_STEP 20        // quota rises 2% of the board each round (tenths of a percent)
#define MAX_DEPTH 2                // items triggering items triggering items

const BossInfo boss_info[NUM_BOSSES] = {
    [BOSS_NONE]  = { "", "", "" },
    [BOSS_LASER] = { "LASER GRID",    "A LASER WIPES OUT A",   "ROW EVERY 2 SECONDS" },
    [BOSS_WIND]  = { "WIND TUNNEL",   "GUSTS PUSH NOBBLE",      "LEFT AND RIGHT" },
    [BOSS_ARMOR] = { "ARMOUR PLATING", "ARMOURED PEGS NEED",   "A HIT TO CRACK FIRST" },
};

const Layout layouts[NUM_LAYOUTS] = {
    { "CLASSIC", 18, {
        { 53, 52 }, { 103, 52 }, { 153, 52 }, { 203, 52 }, { 78, 82 }, { 128, 82 },
        { 178, 82 }, { 53, 112 }, { 103, 112 }, { 153, 112 }, { 203, 112 }, { 78, 142 },
        { 128, 142 }, { 178, 142 }, { 53, 168 }, { 103, 168 }, { 153, 168 }, { 203, 168 },
    } },
    { "DIAMOND", 16, {
        { 106, 50 }, { 150, 50 }, { 84, 76 }, { 128, 76 }, { 172, 76 }, { 62, 102 },
        { 106, 102 }, { 150, 102 }, { 194, 102 }, { 40, 128 }, { 84, 128 }, { 128, 128 },
        { 172, 128 }, { 216, 128 }, { 106, 156 }, { 150, 156 },
    } },
    { "FUNNEL", 16, {
        { 40, 50 }, { 80, 50 }, { 176, 50 }, { 216, 50 }, { 62, 84 }, { 194, 84 },
        { 100, 86 }, { 156, 86 }, { 70, 118 }, { 186, 118 }, { 128, 122 }, { 96, 140 },
        { 160, 140 }, { 40, 166 }, { 128, 166 }, { 216, 166 },
    } },
    { "COLUMNS", 19, {
        { 44, 58 }, { 104, 58 }, { 152, 58 }, { 212, 58 }, { 44, 96 }, { 104, 96 },
        { 152, 96 }, { 212, 96 }, { 44, 134 }, { 104, 134 }, { 152, 134 }, { 212, 134 },
        { 74, 77 }, { 182, 77 }, { 74, 115 }, { 182, 115 }, { 74, 153 }, { 182, 153 },
        { 128, 166 },
    } },
    { "RING", 13, {
        { 200, 108 }, { 186, 142 }, { 150, 163 }, { 106, 163 }, { 70, 142 }, { 56, 108 },
        { 70, 74 }, { 106, 53 }, { 150, 53 }, { 186, 74 }, { 128, 110 }, { 40, 56 },
        { 216, 56 },
    } },
    { "PYRAMID", 15, {
        { 128, 52 }, { 105, 78 }, { 151, 78 }, { 82, 104 }, { 128, 104 }, { 174, 104 },
        { 59, 130 }, { 105, 130 }, { 151, 130 }, { 197, 130 }, { 36, 158 }, { 82, 158 },
        { 128, 158 }, { 174, 158 }, { 220, 158 },
    } },
    { "ZIGZAG", 13, {
        { 48, 50 }, { 128, 50 }, { 208, 50 }, { 88, 78 }, { 168, 78 }, { 48, 106 },
        { 128, 106 }, { 208, 106 }, { 88, 134 }, { 168, 134 }, { 48, 162 }, { 128, 162 },
        { 208, 162 },
    } },
    { "SCATTER", 13, {
        { 64, 54 }, { 150, 50 }, { 206, 70 }, { 40, 92 }, { 104, 88 }, { 170, 104 },
        { 222, 128 }, { 74, 126 }, { 132, 138 }, { 40, 160 }, { 104, 166 }, { 186, 158 },
        { 224, 168 },
    } },
};

const char *const trigger_text[NUM_TRIGGERS] = {
    [TRIG_PASSIVE]   = "ALWAYS:",
    [TRIG_LAUNCH]    = "ON LAUNCH:",
    [TRIG_FIRST_POP] = "FIRST PEG POPPED:",
    [TRIG_PEG_GONE]  = "PEG POPPED AWAY:",
    [TRIG_WALL]      = "WALL BOUNCE:",
    [TRIG_DIES]      = "NOBBLE FALLS OUT:",
    [TRIG_EVERY_8]   = "EVERY 8 PEGS POPPED:",
};

const ItemInfo item_info[NUM_ITEMS] = {
    [ITEM_SPRINGS]  = { "SPRINGS",  "BOUNCE BACK UP ONCE",   TRIG_DIES,      6 },
    [ITEM_SEEDER]   = { "SEEDER",   "ADD A PEG",             TRIG_LAUNCH,    4 },
    [ITEM_PUMP]     = { "PUMP",     "DOUBLE THE LOWEST PEG", TRIG_LAUNCH,    5 },
    [ITEM_ZAPPER]   = { "ZAPPER",   "POP THE HIGHEST PEG",   TRIG_FIRST_POP, 5 },
    [ITEM_DOUBLER]  = { "DOUBLER",  "DOUBLE A RANDOM PEG",   TRIG_FIRST_POP, 5 },
    [ITEM_RICOCHET] = { "RICOCHET", "POP A RANDOM PEG",      TRIG_WALL,      6 },
    [ITEM_PIGGY]    = { "PIGGY",    "1 IN 4 CHANCE: +1 COIN", TRIG_PEG_GONE, 4 },
    [ITEM_ENCORE]   = { "ENCORE",   "+25% OF LAUNCH SCORE",  TRIG_DIES,      6 },
    [ITEM_CHAIN]    = { "CHAIN",    "DOUBLE A RANDOM PEG",   TRIG_EVERY_8,   5 },
    [ITEM_BIG]      = { "BIG",      "NOBBLE IS BIGGER",       TRIG_PASSIVE,   6 },
    [ITEM_HEART]    = { "HEART",    "+1 LIFE AND MAX LIVES", TRIG_PASSIVE,   7 },
};

const PerkInfo perk_info[NUM_PERKS] = {
    [PERK_CONVEYOR]   = { "CONVEYOR", "EVERY 3 SECONDS:",      "TRIGGER ALL ITEMS" },
    [PERK_GREMLIN]  = { "GREMLIN",  "EVERY SECOND:",         "TRIGGER A RANDOM ITEM" },
    [PERK_IGNITION]   = { "IGNITION", "FIRST PEG POPPED:",     "TRIGGER 2 RANDOM ITEMS" },
    [PERK_RECYCLER]    = { "RECYCLER", "NOBBLE FALLS OUT: 50%",  "TRIGGER A RANDOM ITEM" },
    [PERK_BUMPER]  = { "BUMPER",   "WALL BOUNCE: 1 IN 4",   "TRIGGER A RANDOM ITEM" },
    [PERK_PAYDAY]   = { "PAYDAY",   "PASSING THE GOAL:",     "TRIGGER ALL ITEMS" },
    [PERK_JACKPOT] = { "JACKPOT",  "FIRST POP IS THE TOP",  "PEG: 3 RANDOM ITEMS" },
    [PERK_DOMINO]    = { "DOMINO",   "15 PEGS POPPED:",       "TRIGGER ALL ITEMS" },
};

// sin for angles 0..64 (a quarter turn), 8.8
static const int16_t quarter_sin[65] = {
    0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 62, 68, 74, 80, 86, 92,
    98, 104, 109, 115, 121, 126, 132, 137, 142, 147, 152, 157, 162, 167, 172, 177,
    181, 185, 190, 194, 198, 202, 206, 209, 213, 216, 220, 223, 226, 229, 231, 234,
    237, 239, 241, 243, 245, 247, 248, 250, 251, 252, 253, 254, 255, 255, 256, 256, 256,
};

static int isin(int a)
{
    a &= 255;
    if (a <= 64) return quarter_sin[a];
    if (a <= 128) return quarter_sin[128 - a];
    if (a <= 192) return -quarter_sin[a - 128];
    return -quarter_sin[256 - a];
}
static int icos(int a) { return isin(a + 64); }

uint32_t game_rand(Game *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g->rng = x;
}

static int rand_below(Game *g, int n) { return (int)(game_rand(g) % (uint32_t)n); }

int game_has(const Game *g, int item)
{
    for (int i = 0; i < g->nitems; i++)
        if (g->items[i] == item) return 1;
    return 0;
}

int game_has_perk(const Game *g, int perk)
{
    for (int i = 0; i < g->nperks; i++)
        if (g->perks[i] == perk) return 1;
    return 0;
}

int game_radius(const Game *g) { return game_has(g, ITEM_BIG) ? BIG_NOBBLE_R : NOBBLE_R; }

int game_potential(const Game *g)
{
    int total = 0;
    for (int i = 0; i < g->nslots; i++)
        if (g->pegs[i]) total += g->pegs[i] * 2 - 1;     // 8 pays 8+4+2+1
    return total;
}

// New pegs double in value every other round, so the numbers keep climbing.
// (The quota is a share of the board, so this doesn't change the difficulty.)
static int32_t new_peg_value(const Game *g)
{
    int shift = (g->round - 1) / 2;
    return 1 << (shift > 20 ? 20 : shift);
}

// The quota is a share of everything on the board, rising each round.
static int quota_for(const Game *g)
{
    int pct = 150 + (g->round - 1) * QUOTA_STEP;   // tenths of a percent
    if (pct > 700) pct = 700;
    int q = game_potential(g) * pct / 1000;
    if (g->boss) q = q * 3 / 4;               // the hazard makes up the difference
    return q < 5 ? 5 : q;
}

int game_boss_for(int round)
{
    if (round % BOSS_EVERY) return BOSS_NONE;
    return 1 + (round / BOSS_EVERY - 1) % (NUM_BOSSES - 1);
}

static void begin_round(Game *g)
{
    g->boss = game_boss_for(g->round);
    // armour goes on the more valuable half of the pegs
    int32_t mid = 0;
    int n = 0;
    for (int i = 0; i < g->nslots; i++)
        if (g->pegs[i]) {
            mid += g->pegs[i];
            n++;
        }
    mid = n ? mid / n : 0;
    for (int i = 0; i < g->nslots; i++) {
        g->armor[i] = g->boss == BOSS_ARMOR && g->pegs[i] && g->pegs[i] >= mid && rand_below(g, 2);
        g->armor_start[i] = g->armor[i];
        g->round_start[i] = g->pegs[i];
    }
    g->wind = rand_below(g, 2) ? 1 : -1;
    g->laser_y = -1;
    g->laser_timer = 0;
    g->quota = quota_for(g);
    g->score = 0;
}

// Move to another layout, keeping the biggest pegs (in a shuffled order).
void game_set_layout(Game *g, int layout)
{
    int32_t vals[NUM_SLOTS];
    int n = 0;
    for (int i = 0; i < g->nslots; i++)
        if (g->pegs[i]) vals[n++] = g->pegs[i];
    for (int i = 1; i < n; i++)                         // biggest first
        for (int j = i; j > 0 && vals[j] > vals[j - 1]; j--) {
            int32_t t = vals[j];
            vals[j] = vals[j - 1];
            vals[j - 1] = t;
        }
    g->layout = layout;
    g->nslots = layouts[layout].n;
    if (n > g->nslots) n = g->nslots;
    for (int i = n - 1; i > 0; i--) {                   // shuffle what we keep
        int j = rand_below(g, i + 1);
        int32_t t = vals[i];
        vals[i] = vals[j];
        vals[j] = t;
    }
    for (int i = 0; i < NUM_SLOTS; i++) {
        g->slot[i] = layouts[layout].pos[i];
        g->pegs[i] = i < n ? vals[i] : i < g->nslots ? new_peg_value(g) : 0;
        g->cooldown[i] = g->flash[i] = g->armor[i] = 0;
    }
}

void game_new_run(Game *g, uint32_t seed)
{
    g->rng = seed ? seed : 0x2468ACE;
    g->round = 1;
    g->lives = g->max_lives = START_LIVES;
    g->coins = 0;
    g->nitems = g->nperks = 0;
    g->flying = 0;
    g->depth = 0;
    g->ev = 0;
    g->restocks = g->perfect = 0;
    for (int s = 0; s < MAX_ITEMS; s++) g->item_flash[s] = 0;
    for (int s = 0; s < MAX_PERKS; s++) g->perk_flash[s] = 0;
    // a starter board on a random layout: mostly 1s and 2s with a few 4s
    g->layout = rand_below(g, NUM_LAYOUTS);
    g->nslots = layouts[g->layout].n;
    for (int i = 0; i < NUM_SLOTS; i++) {
        g->slot[i] = layouts[g->layout].pos[i];
        int r = rand_below(g, 100);
        g->pegs[i] = i >= g->nslots ? 0 : r < 55 ? 1 : r < 85 ? 2 : 4;
        g->cooldown[i] = g->flash[i] = g->armor[i] = 0;
    }
    begin_round(g);
}

// ---------------------------------------------------------------- items and perks

static void fire(Game *g, int trigger);
static void trigger_all(Game *g, int perk);
static void trigger_all(Game *g, int perk);

static int pick_peg(Game *g, int want)   // want: 0 random, 1 lowest, 2 highest
{
    int best = -1, n = 0;
    for (int i = 0; i < g->nslots; i++) {
        if (!g->pegs[i]) continue;
        if (want == 0) {
            if (rand_below(g, ++n) == 0) best = i;       // reservoir pick
        } else if (best < 0 || (want == 1 ? g->pegs[i] < g->pegs[best] : g->pegs[i] > g->pegs[best])) {
            best = i;
        }
    }
    return best;
}

static int highest_value(const Game *g)
{
    int32_t v = 0;
    for (int i = 0; i < g->nslots; i++)
        if (g->pegs[i] > v) v = g->pegs[i];
    return v;
}

static void check_goal(Game *g)
{
    if (g->passed_goal || g->score < g->quota) return;
    g->passed_goal = 1;
    if (game_has_perk(g, PERK_PAYDAY)) trigger_all(g, PERK_PAYDAY);
}

// Score peg i and halve it. Returns 1 if it vanished.
static int pop_peg(Game *g, int i)
{
    int32_t v = g->pegs[i];
    g->score += v;
    g->hits++;
    g->pegs[i] = v / 2;              // a 1 disappears
    if (!g->pegs[i]) g->armor[i] = 0;
    g->cooldown[i] = PEG_COOLDOWN;
    g->flash[i] = FLASH_FRAMES;
    if (g->ev) {
        g->ev->pop = 1;
        g->ev->gone |= g->pegs[i] == 0;
    }
    check_goal(g);
    if (g->hits % 8 == 0) fire(g, TRIG_EVERY_8);
    if (g->hits == 15 && game_has_perk(g, PERK_DOMINO)) trigger_all(g, PERK_DOMINO);
    if (!g->pegs[i]) fire(g, TRIG_PEG_GONE);
    return g->pegs[i] == 0;
}

// Carry out the item in `slot`.
static void run_item(Game *g, int slot)
{
    if (slot < 0 || slot >= g->nitems || g->depth > MAX_DEPTH) return;
    int i;
    g->depth++;
    g->item_flash[slot] = FLASH_FRAMES * 2;
    if (g->ev) g->ev->item = 1;
    switch (g->items[slot]) {
    case ITEM_SPRINGS:
        if (g->flying && !g->spring_used) {
            g->spring_used = 1;
            if (g->vy > 0) g->vy = -g->vy;
            g->vy -= FIX(1);
            if (g->vy < -MAX_SPEED) g->vy = -MAX_SPEED;
            if (g->y > FIX(FLOOR_Y)) g->y = FIX(FLOOR_Y);
            if (g->ev) g->ev->spring = 1;
        }
        break;
    case ITEM_SEEDER: {
        int empty = -1, n = 0;
        for (int k = 0; k < g->nslots; k++)
            if (!g->pegs[k] && rand_below(g, ++n) == 0) empty = k;
        if (empty >= 0) {
            g->pegs[empty] = new_peg_value(g);
            g->flash[empty] = FLASH_FRAMES;
        } else if ((i = pick_peg(g, 0)) >= 0) {
            g->pegs[i] *= 2;
        }
        break;
    }
    case ITEM_PUMP:
        if ((i = pick_peg(g, 1)) >= 0) {
            g->pegs[i] *= 2;
            g->flash[i] = FLASH_FRAMES;
        }
        break;
    case ITEM_ZAPPER:
        if ((i = pick_peg(g, 2)) >= 0) pop_peg(g, i);
        break;
    case ITEM_DOUBLER:
    case ITEM_CHAIN:
        if ((i = pick_peg(g, 0)) >= 0) {
            g->pegs[i] *= 2;
            g->flash[i] = FLASH_FRAMES;
        }
        break;
    case ITEM_RICOCHET:
        if (g->ricochets < MAX_RICOCHETS && (i = pick_peg(g, 0)) >= 0) {
            g->ricochets++;
            pop_peg(g, i);
        }
        break;
    case ITEM_PIGGY:
        if (rand_below(g, 4) == 0) g->coins++;
        break;
    case ITEM_ENCORE:
        g->score += (g->score + 3) / 4;     // rounded up, so small scores still grow
        check_goal(g);
        break;
    default:                            // passive items do nothing when triggered
        break;
    }
    g->depth--;
}

static void flash_perk(Game *g, int perk)
{
    for (int p = 0; p < g->nperks; p++)
        if (g->perks[p] == perk) g->perk_flash[p] = FLASH_FRAMES * 2;
}

// Fire n items picked at random (repeats allowed).
static void trigger_random(Game *g, int n, int perk)
{
    if (!g->nitems) return;
    flash_perk(g, perk);
    for (int k = 0; k < n; k++) run_item(g, rand_below(g, g->nitems));
}

static void trigger_all(Game *g, int perk)
{
    flash_perk(g, perk);
    for (int s = 0; s < g->nitems; s++) run_item(g, s);
}

// Fire every item with this trigger, in slot order.
static void fire(Game *g, int trigger)
{
    for (int s = 0; s < g->nitems; s++)
        if (item_info[g->items[s]].trigger == trigger) run_item(g, s);
}

// ---------------------------------------------------------------- physics

static int32_t isqrt(uint32_t n)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (int32_t)r;
}

void game_launch(Game *g, int angle)
{
    if (angle > AIM_MAX) angle = AIM_MAX;
    if (angle < -AIM_MAX) angle = -AIM_MAX;
    g->x = FIX(LAUNCH_X);
    g->y = FIX(LAUNCH_Y);
    g->vx = isin(angle) * LAUNCH_SPEED / 256;
    g->vy = icos(angle) * LAUNCH_SPEED / 256;
    g->score = g->hits = g->frames = g->still = g->spring_used = 0;
    g->ricochets = g->passed_goal = 0;
    g->laser_y = -1;
    g->flying = 1;
    fire(g, TRIG_LAUNCH);
}

// Bounce Nobble off a peg at (cx, cy). Returns 1 on contact.
static int collide(Game *g, int nr, int cx, int cy)
{
    int32_t dx = g->x - FIX(cx), dy = g->y - FIX(cy);
    int32_t reach = FIX(nr + PEG_R);
    if (dx >= reach || dx <= -reach || dy >= reach || dy <= -reach) return 0;
    uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
    if (d2 >= (uint32_t)reach * (uint32_t)reach) return 0;
    int32_t d = isqrt(d2);
    int32_t nx, ny;
    if (d < 16) {
        nx = 0;
        ny = -256;
        d = 0;
    } else {
        nx = dx * 256 / d;
        ny = dy * 256 / d;
    }
    g->x += nx * (reach - d) / 256;
    g->y += ny * (reach - d) / 256;
    int32_t vn = (g->vx * nx + g->vy * ny) / 256;
    if (vn < 0) {
        int32_t j = vn * (256 + PEG_BOUNCE) / 256;
        g->vx -= nx * j / 256;
        g->vy -= ny * j / 256;
        g->vx += rand_below(g, 17) - 8;     // never balance on a peg
    }
    return 1;
}

// Nobble hit peg i.
static void hit_peg(Game *g, int i)
{
    if (g->armor[i]) {                  // boss armour: this hit only cracks it
        g->armor[i] = 0;
        g->cooldown[i] = PEG_COOLDOWN;
        g->flash[i] = FLASH_FRAMES;
        if (g->ev) g->ev->armor = 1;
        return;
    }
    int first = g->hits == 0;
    int was_top = g->pegs[i] == highest_value(g);
    pop_peg(g, i);
    if (!first) return;
    fire(g, TRIG_FIRST_POP);
    if (game_has_perk(g, PERK_IGNITION)) trigger_random(g, 2, PERK_IGNITION);
    if (was_top && game_has_perk(g, PERK_JACKPOT)) trigger_random(g, 3, PERK_JACKPOT);
}

// Walls and ceiling; returns 1 if Nobble bounced off a side wall.
static int walls(int32_t *x, int32_t *y, int32_t *vx, int32_t *vy, int r)
{
    int hit = 0;
    if (*x < FIX(BOARD_L + r)) {
        *x = FIX(BOARD_L + r);
        if (*vx < 0) {
            *vx = -*vx * WALL_BOUNCE / 256;
            hit = 1;
        }
    }
    if (*x > FIX(BOARD_R - r)) {
        *x = FIX(BOARD_R - r);
        if (*vx > 0) {
            *vx = -*vx * WALL_BOUNCE / 256;
            hit = 1;
        }
    }
    if (*y < FIX(CEILING_Y + r)) {
        *y = FIX(CEILING_Y + r);
        if (*vy < 0) *vy = -*vy * WALL_BOUNCE / 256;
    }
    return hit;
}

static void clamp_speed(int32_t *vx, int32_t *vy)
{
    if (*vx > MAX_SPEED) *vx = MAX_SPEED;
    if (*vx < -MAX_SPEED) *vx = -MAX_SPEED;
    if (*vy > MAX_SPEED) *vy = MAX_SPEED;
    if (*vy < -MAX_SPEED) *vy = -MAX_SPEED;
}

static void substep(Game *g, Events *ev)
{
    int r = game_radius(g);
    g->vy += GRAVITY / SUBSTEPS;
    if (g->boss == BOSS_WIND) g->vx += g->wind * WIND_PUSH / SUBSTEPS;
    clamp_speed(&g->vx, &g->vy);
    g->x += g->vx / SUBSTEPS;
    g->y += g->vy / SUBSTEPS;

    if (walls(&g->x, &g->y, &g->vx, &g->vy, r)) {
        ev->wall = 1;
        fire(g, TRIG_WALL);
        if (game_has_perk(g, PERK_BUMPER) && rand_below(g, 4) == 0) trigger_random(g, 1, PERK_BUMPER);
    }

    for (int i = 0; i < g->nslots; i++)
        if (g->pegs[i] && collide(g, r, g->slot[i].x, g->slot[i].y) && !g->cooldown[i]) hit_peg(g, i);
}

// Boss laser: every LASER_EVERY frames in flight it locks onto the height of
// a random peg, warns, then wipes out every peg in that band. Wiped pegs
// score nothing.
static void laser(Game *g, Events *ev)
{
    if (g->laser_y < 0) {
        if (g->frames % LASER_EVERY != LASER_EVERY / 2) return;
        int i = pick_peg(g, 0);
        if (i < 0) return;
        g->laser_y = g->slot[i].y;
        g->laser_timer = 0;
        return;
    }
    g->laser_timer++;
    if (g->laser_timer == LASER_WARN) {
        for (int i = 0; i < g->nslots; i++) {
            int dy = g->slot[i].y - g->laser_y;
            if (dy >= -LASER_BAND && dy <= LASER_BAND) g->pegs[i] = g->armor[i] = 0;
        }
        ev->laser = 1;
    }
    if (g->laser_timer >= LASER_WARN + LASER_BEAM) g->laser_y = -1;
}

int game_step(Game *g, Events *ev)
{
    ev->pop = ev->gone = ev->wall = ev->spring = ev->item = ev->laser = ev->armor = 0;
    for (int i = 0; i < g->nslots; i++) {
        if (g->cooldown[i]) g->cooldown[i]--;
        if (g->flash[i]) g->flash[i]--;
    }
    for (int s = 0; s < MAX_ITEMS; s++)
        if (g->item_flash[s]) g->item_flash[s]--;
    for (int p = 0; p < MAX_PERKS; p++)
        if (g->perk_flash[p]) g->perk_flash[p]--;
    if (!g->flying) return 0;

    g->ev = ev;
    for (int s = 0; s < SUBSTEPS; s++) substep(g, ev);
    g->frames++;

    if (g->boss == BOSS_WIND && g->frames % WIND_FLIP == 0) g->wind = -g->wind;
    if (g->boss == BOSS_LASER) laser(g, ev);

    if (game_has_perk(g, PERK_CONVEYOR) && g->frames % 180 == 0) trigger_all(g, PERK_CONVEYOR);
    if (game_has_perk(g, PERK_GREMLIN) && g->frames % 60 == 0) trigger_random(g, 1, PERK_GREMLIN);

    // if Nobble comes to rest on something, give it a shove
    int slow = g->vx < 40 && g->vx > -40 && g->vy < 40 && g->vy > -40;
    g->still = slow ? g->still + 1 : 0;
    if (g->still > 30) {
        g->vx = (game_rand(g) & 1) ? FIX(1) : -FIX(1);
        g->vy = -FIX(1);
        g->still = 0;
    }

    int out = 0;
    if (g->y > FIX(EXIT_Y) || g->frames > LAUNCH_TIMEOUT) {
        fire(g, TRIG_DIES);
        if (game_has_perk(g, PERK_RECYCLER) && rand_below(g, 2) == 0) trigger_random(g, 1, PERK_RECYCLER);
        // springs can pull Nobble back from the brink
        out = g->y > FIX(FLOOR_Y) || g->frames > LAUNCH_TIMEOUT;
        if (out) g->flying = 0;
    }
    ev->hits = g->hits;
    g->ev = 0;
    return out;
}

// ---------------------------------------------------------------- rounds

// A restock: pegs with the same number merge in pairs (one doubles, the
// other leaves an empty slot), then every empty slot gets a new peg.
static void restock(Game *g)
{
    for (int i = 0; i < g->nslots; i++) {
        if (!g->pegs[i]) continue;
        for (int j = i + 1; j < g->nslots; j++)
            if (g->pegs[j] == g->pegs[i]) {
                g->pegs[i] *= 2;
                g->pegs[j] = 0;
                break;
            }
    }
    int32_t v = new_peg_value(g);
    for (int i = 0; i < g->nslots; i++)
        if (!g->pegs[i]) g->pegs[i] = v;
    g->coins++;
}

Result game_resolve(Game *g)
{
    int left = 0;
    for (int i = 0; i < g->nslots; i++) left += g->pegs[i] != 0;
    g->perfect = left == 0;
    if (g->perfect) g->score *= 2;           // popped every peg

    if (g->score >= g->quota) {
        g->restocks = g->score / g->quota;
        if (g->restocks > MAX_RESTOCKS) g->restocks = MAX_RESTOCKS;
        for (int k = 0; k < g->restocks; k++) restock(g);
        if (g->lives < g->max_lives) g->lives++;
        if (g->boss) g->coins += BOSS_BONUS;
        g->round++;
        if ((g->round - 1) % LAYOUT_EVERY == 0) {           // a new layout every few rounds
            int next = rand_below(g, NUM_LAYOUTS - 1);
            game_set_layout(g, next >= g->layout ? next + 1 : next);
        }
        begin_round(g);
        return RESULT_CLEARED;
    }

    g->restocks = 0;
    g->lives--;
    for (int i = 0; i < g->nslots; i++) {
        g->pegs[i] = g->round_start[i];
        g->armor[i] = g->armor_start[i];
    }
    g->laser_y = -1;
    return g->lives > 0 ? RESULT_RETRY : RESULT_GAME_OVER;
}

// ---------------------------------------------------------------- shop and perks

int game_shop_due(const Game *g) { return g->round > 1 && (g->round - 1) % SHOP_EVERY == 0; }

void game_roll_shop(Game *g)
{
    int pool[NUM_ITEMS], n = 0;
    for (int i = 0; i < NUM_ITEMS; i++)
        if (!game_has(g, i)) pool[n++] = i;
    for (int s = 0; s < SHOP_SLOTS; s++) {
        if (n == 0) {
            g->shop[s] = -1;
            continue;
        }
        int k = rand_below(g, n);
        g->shop[s] = pool[k];
        pool[k] = pool[--n];
    }
}

int game_refund(int item) { return item_info[item].price / 2; }

int game_buy(Game *g, int slot)
{
    return game_buy_swap(g, slot, -1);
}

int game_buy_swap(Game *g, int slot, int replace)
{
    int item = g->shop[slot];
    if (item < 0 || g->coins < item_info[item].price) return 0;
    if (replace < 0 ? g->nitems >= MAX_ITEMS : replace >= g->nitems) return 0;
    g->coins -= item_info[item].price;
    if (replace < 0) {
        g->items[g->nitems++] = (uint8_t)item;
    } else {
        if (g->items[replace] == ITEM_HEART) {      // its extra life goes with it
            g->max_lives--;
            if (g->lives > g->max_lives) g->lives = g->max_lives;
        }
        g->coins += game_refund(g->items[replace]);
        g->items[replace] = (uint8_t)item;
        g->item_flash[replace] = 0;
    }
    g->shop[slot] = -1;
    if (item == ITEM_HEART) {
        g->max_lives++;
        g->lives++;
    }
    return 1;
}

int game_perk_due(const Game *g)
{
    return g->round > 1 && (g->round - 1) % PERK_EVERY == 0 && g->nperks < MAX_PERKS;
}

void game_roll_perks(Game *g)
{
    int pool[NUM_PERKS], n = 0;
    for (int p = 0; p < NUM_PERKS; p++)
        if (!game_has_perk(g, p)) pool[n++] = p;
    for (int c = 0; c < PERK_CHOICES; c++) {
        int k = rand_below(g, n);
        g->perk_offer[c] = pool[k];
        pool[k] = pool[--n];
    }
}

void game_take_perk(Game *g, int choice)
{
    if (g->nperks < MAX_PERKS) g->perks[g->nperks++] = (uint8_t)g->perk_offer[choice];
}

// ---------------------------------------------------------------- aim guide

int game_aim_at(int x, int y)
{
    int32_t dx = x - LAUNCH_X, dy = y - LAUNCH_Y;
    if (dy <= 0) return dx < 0 ? -AIM_MAX : AIM_MAX;      // touched above the launcher
    int best = 0;
    int32_t best_err = 0x7FFFFFFF;
    for (int a = -AIM_MAX; a <= AIM_MAX; a++) {
        int32_t err = isin(a) * dy - icos(a) * dx;         // 0 when the angle points at (x, y)
        if (err < 0) err = -err;
        if (err < best_err) {
            best_err = err;
            best = a;
        }
    }
    return best;
}

int game_predict(const Game *g, int angle, int16_t *xs, int16_t *ys, int n)
{
    int r = game_radius(g);
    int32_t x = FIX(LAUNCH_X), y = FIX(LAUNCH_Y);
    int32_t vx = isin(angle) * LAUNCH_SPEED / 256, vy = icos(angle) * LAUNCH_SPEED / 256;
    int32_t reach = FIX(r + PEG_R);
    int count = 0;
    for (int f = 1; f <= 60 && count < n; f++) {
        for (int s = 0; s < SUBSTEPS; s++) {
            vy += GRAVITY / SUBSTEPS;
            if (g->boss == BOSS_WIND) vx += g->wind * WIND_PUSH / SUBSTEPS;
            clamp_speed(&vx, &vy);
            x += vx / SUBSTEPS;
            y += vy / SUBSTEPS;
            walls(&x, &y, &vx, &vy, r);
            for (int i = 0; i < g->nslots; i++) {
                int32_t dx = x - FIX(g->slot[i].x), dy = y - FIX(g->slot[i].y);
                if (g->pegs[i] && dx < reach && dx > -reach && dy < reach && dy > -reach &&
                    (uint32_t)(dx * dx + dy * dy) < (uint32_t)reach * (uint32_t)reach)
                    return count;                  // stop at the first peg
            }
        }
        if (f % 4 == 0) {
            xs[count] = x >> 8;
            ys[count] = y >> 8;
            count++;
        }
    }
    return count;
}
