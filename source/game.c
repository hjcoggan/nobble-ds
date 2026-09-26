#include "game.h"

#define FIX(n) ((n) << 8)
#define GRAVITY 12                 // 8.8 px/frame^2
#define FLOATY_GRAVITY 7
#define LAUNCH_SPEED (3 * 256)     // 8.8 px/frame
#define MAX_SPEED FIX(5)
#define SUBSTEPS 2
#define PEG_BOUNCE 230             // restitution off pegs, 8.8
#define WALL_BOUNCE 230
#define PEG_COOLDOWN 6
#define FLASH_FRAMES 8

// 6 rows alternating 4 and 3 pegs
const Slot slots[NUM_SLOTS] = {
    { 66, 42 }, { 102, 42 }, { 138, 42 }, { 174, 42 },
    { 84, 60 }, { 120, 60 }, { 156, 60 },
    { 66, 78 }, { 102, 78 }, { 138, 78 }, { 174, 78 },
    { 84, 96 }, { 120, 96 }, { 156, 96 },
    { 66, 114 }, { 102, 114 }, { 138, 114 }, { 174, 114 },
    { 84, 132 }, { 120, 132 }, { 156, 132 },
};

const ItemInfo item_info[NUM_ITEMS] = {
    [ITEM_SPRINGS] = { "SPRINGS", "FLOOR BOUNCES NUBBY ONCE", 6 },
    [ITEM_WALLS]   = { "WALLS",   "WALL BOUNCES SCORE +3",    4 },
    [ITEM_PUMP]    = { "PUMP",    "LOWEST PEG X2 EACH ROUND", 5 },
    [ITEM_BIG]     = { "BIG",     "NUBBY IS BIGGER",          6 },
    [ITEM_FIRST]   = { "FIRST",   "FIRST HIT SCORES X3",      5 },
    [ITEM_FLOATY]  = { "FLOATY",  "LOWER GRAVITY",            5 },
    [ITEM_HEART]   = { "HEART",   "+1 LIFE AND MAX LIVES",    7 },
    [ITEM_RICH]    = { "RICH",    "+1 COIN PER RESTOCK",      4 },
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

int game_radius(const Game *g) { return game_has(g, ITEM_BIG) ? BIG_NUBBY_R : NUBBY_R; }

int game_potential(const Game *g)
{
    int total = 0;
    for (int i = 0; i < NUM_SLOTS; i++)
        if (g->pegs[i]) total += g->pegs[i] * 2 - 1;     // 8 pays 8+4+2+1
    return total;
}

// The quota is a share of everything on the board, rising each round.
static int quota_for(const Game *g)
{
    int pct = 15 + (g->round - 1) * 3;
    if (pct > 70) pct = 70;
    int q = game_potential(g) * pct / 100;
    return q < 5 ? 5 : q;
}

static void begin_round(Game *g)
{
    if (game_has(g, ITEM_PUMP)) {
        int low = -1;
        for (int i = 0; i < NUM_SLOTS; i++)
            if (g->pegs[i] && (low < 0 || g->pegs[i] < g->pegs[low])) low = i;
        if (low >= 0) g->pegs[low] *= 2;
    }
    for (int i = 0; i < NUM_SLOTS; i++) g->round_start[i] = g->pegs[i];
    g->quota = quota_for(g);
    g->score = 0;
}

void game_new_run(Game *g, uint32_t seed)
{
    g->rng = seed ? seed : 0x2468ACE;
    g->round = 1;
    g->lives = g->max_lives = START_LIVES;
    g->coins = 0;
    g->nitems = 0;
    g->flying = 0;
    g->restocks = g->perfect = 0;
    // a starter board: mostly 1s and 2s with a couple of 4s
    for (int i = 0; i < NUM_SLOTS; i++) {
        int r = rand_below(g, 100);
        g->pegs[i] = r < 30 ? 0 : r < 70 ? 1 : r < 92 ? 2 : 4;
        g->cooldown[i] = g->flash[i] = 0;
    }
    begin_round(g);
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
    g->flying = 1;
}

// Bounce Nubby off a peg at (cx, cy). Returns 1 on contact.
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

static void pop(Game *g, int i, Events *ev)
{
    int v = g->pegs[i];
    int pts = v;
    if (g->hits == 0 && game_has(g, ITEM_FIRST)) pts *= 3;
    g->score += pts;
    g->hits++;
    g->pegs[i] = v / 2;              // a 1 disappears
    g->cooldown[i] = PEG_COOLDOWN;
    g->flash[i] = FLASH_FRAMES;
    ev->pop = 1;
    ev->gone = g->pegs[i] == 0;
    ev->value = pts;
}

// Walls and ceiling; returns 1 if Nubby bounced off a side wall.
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
    g->vy += (game_has(g, ITEM_FLOATY) ? FLOATY_GRAVITY : GRAVITY) / SUBSTEPS;
    clamp_speed(&g->vx, &g->vy);
    g->x += g->vx / SUBSTEPS;
    g->y += g->vy / SUBSTEPS;

    if (walls(&g->x, &g->y, &g->vx, &g->vy, r)) {
        if (game_has(g, ITEM_WALLS)) g->score += 3;
        ev->wall = 1;
    }

    for (int i = 0; i < NUM_SLOTS; i++) {
        if (g->pegs[i] && collide(g, r, slots[i].x, slots[i].y) && !g->cooldown[i]) pop(g, i, ev);
    }

    if (!g->spring_used && game_has(g, ITEM_SPRINGS) && g->y > FIX(FLOOR_Y) && g->vy > 0) {
        g->vy = -g->vy - FIX(1);
        if (g->vy < -MAX_SPEED) g->vy = -MAX_SPEED;
        g->spring_used = 1;
        ev->spring = 1;
    }
}

int game_step(Game *g, Events *ev)
{
    ev->pop = ev->gone = ev->wall = ev->spring = 0;
    ev->value = 0;
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (g->cooldown[i]) g->cooldown[i]--;
        if (g->flash[i]) g->flash[i]--;
    }
    if (!g->flying) return 0;

    for (int s = 0; s < SUBSTEPS; s++) substep(g, ev);
    g->frames++;
    ev->hits = g->hits;

    // if Nubby comes to rest on something, give it a shove
    int slow = g->vx < 40 && g->vx > -40 && g->vy < 40 && g->vy > -40;
    g->still = slow ? g->still + 1 : 0;
    if (g->still > 30) {
        g->vx = (game_rand(g) & 1) ? FIX(1) : -FIX(1);
        g->vy = -FIX(1);
        g->still = 0;
    }

    if (g->y > FIX(EXIT_Y) || g->frames > LAUNCH_TIMEOUT) {
        g->flying = 0;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- rounds

// A restock fills every empty slot with a new peg worth 2^(round/4), and
// any peg already showing that value swallows a copy of it and doubles.
static void restock(Game *g)
{
    int32_t v = 1 << (g->round / 4);
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (g->pegs[i] == v) g->pegs[i] *= 2;
        else if (!g->pegs[i]) g->pegs[i] = v;
    }
    g->coins += 1 + (game_has(g, ITEM_RICH) ? 1 : 0);
}

Result game_resolve(Game *g)
{
    int left = 0;
    for (int i = 0; i < NUM_SLOTS; i++) left += g->pegs[i] != 0;
    g->perfect = left == 0;
    if (g->perfect) g->score *= 2;           // popped every peg

    if (g->score >= g->quota) {
        g->restocks = g->score / g->quota;
        if (g->restocks > MAX_RESTOCKS) g->restocks = MAX_RESTOCKS;
        for (int k = 0; k < g->restocks; k++) restock(g);
        if (g->lives < g->max_lives) g->lives++;
        g->round++;
        begin_round(g);
        return RESULT_CLEARED;
    }

    g->restocks = 0;
    g->lives--;
    for (int i = 0; i < NUM_SLOTS; i++) g->pegs[i] = g->round_start[i];
    return g->lives > 0 ? RESULT_RETRY : RESULT_GAME_OVER;
}

int game_shop_due(const Game *g) { return g->round > 1 && (g->round - 1) % SHOP_EVERY == 0; }

// ---------------------------------------------------------------- shop

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

int game_buy(Game *g, int slot)
{
    int item = g->shop[slot];
    if (item < 0 || g->nitems >= MAX_ITEMS || g->coins < item_info[item].price) return 0;
    g->coins -= item_info[item].price;
    g->items[g->nitems++] = (uint8_t)item;
    g->shop[slot] = -1;
    if (item == ITEM_HEART) {
        g->max_lives++;
        g->lives++;
    }
    return 1;
}

// ---------------------------------------------------------------- aim guide

int game_predict(const Game *g, int angle, int16_t *xs, int16_t *ys, int n)
{
    int r = game_radius(g);
    int grav = game_has(g, ITEM_FLOATY) ? FLOATY_GRAVITY : GRAVITY;
    int32_t x = FIX(LAUNCH_X), y = FIX(LAUNCH_Y);
    int32_t vx = isin(angle) * LAUNCH_SPEED / 256, vy = icos(angle) * LAUNCH_SPEED / 256;
    int32_t reach = FIX(r + PEG_R);
    int count = 0;
    for (int f = 1; f <= 60 && count < n; f++) {
        for (int s = 0; s < SUBSTEPS; s++) {
            vy += grav / SUBSTEPS;
            clamp_speed(&vx, &vy);
            x += vx / SUBSTEPS;
            y += vy / SUBSTEPS;
            walls(&x, &y, &vx, &vy, r);
            for (int i = 0; i < NUM_SLOTS; i++) {
                int32_t dx = x - FIX(slots[i].x), dy = y - FIX(slots[i].y);
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
