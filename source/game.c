#include "game.h"

#define FIX(n) ((n) << 8)
#define GRAVITY 22                 // 8.8 px/frame^2
#define MAX_SPEED FIX(5)
#define SUBSTEPS 2
#define PEG_BOUNCE 140             // restitution, 8.8
#define WALL_BOUNCE 150
#define BUMPER_KICK FIX(3)         // minimum speed away from a bumper
#define BUMPER_COOLDOWN 8
#define MAX_BUMPS 8                // after this many kicks in a drop, bumpers go quiet

const ItemInfo item_info[NUM_ITEMS] = {
    [ITEM_HEAVY]  = { "HEAVY",  "WHITE PEGS WORTH 2",     6 },
    [ITEM_MULTI]  = { "MULTI",  "2 MORE PURPLE PEGS",     6 },
    [ITEM_EXTRA]  = { "EXTRA",  "1 MORE DROP EACH ROUND", 7 },
    [ITEM_SPRING] = { "SPRING", "MORE BUMPERS, WORTH 3",  5 },
    [ITEM_LUCKY]  = { "LUCKY",  "EVERY 7TH PEG GIVES +7", 5 },
    [ITEM_PIGGY]  = { "PIGGY",  "1 COIN PER 4 SAVED",     4 },
    [ITEM_BOOST]  = { "BOOST",  "EACH DROP STARTS AT 5",  5 },
    [ITEM_BUCKET] = { "BUCKET", "MIDDLE BUCKET X5",       6 },
};

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

int game_bucket_mult(const Game *g, int slot)
{
    static const int mults[NUM_BUCKETS] = { 2, 1, 3, 1, 2 };
    if (slot == NUM_BUCKETS / 2 && game_has(g, ITEM_BUCKET)) return 5;
    return mults[slot];
}

int game_target(int round)
{
    int t = 90;
    for (int r = 1; r < round; r++) t = (t * 7 / 5 + 5) / 10 * 10;   // +40% a round
    return t;
}

void game_new_run(Game *g, uint32_t seed)
{
    g->rng = seed ? seed : 0x2468ACE;
    g->round = 0;
    g->total_score = 0;
    g->coins = 0;
    g->nitems = 0;
    g->dropping = 0;
}

// ---------------------------------------------------------------- board

static void remove_peg(Game *g, int i)
{
    g->pegs[i] = g->pegs[--g->npegs];
}

static int pick_plus_peg(Game *g)
{
    for (int tries = 0; tries < 200; tries++) {
        int i = rand_below(g, g->npegs);
        if (g->pegs[i].type == PEG_PLUS) return i;
    }
    return -1;
}

static void make_board(Game *g)
{
    g->npegs = 0;
    for (int row = 0; row < 8; row++) {
        int y = 34 + row * 13;
        int odd = row & 1;
        for (int k = 0; k < (odd ? 8 : 9); k++) {
            if (g->npegs >= MAX_PEGS) break;
            if (rand_below(g, 100) < 14) continue;     // gaps keep boards varied
            Peg *p = &g->pegs[g->npegs++];
            p->x = (odd ? 60 : 52) + k * 16;
            p->y = y;
            p->type = PEG_PLUS;
            p->lit = p->flash = 0;
        }
    }

    // bumpers replace a peg and clear the pegs around them
    int bumpers = 2 + (game_has(g, ITEM_SPRING) ? 2 : 0);
    for (int b = 0; b < bumpers; b++) {
        int i = pick_plus_peg(g);
        if (i < 0 || g->pegs[i].y < 47 || g->pegs[i].y > 112) {
            b--;
            if (i < 0) break;
            continue;
        }
        Peg bp = g->pegs[i];
        bp.type = PEG_BUMPER;
        for (int j = g->npegs - 1; j >= 0; j--) {
            int dx = g->pegs[j].x - bp.x, dy = g->pegs[j].y - bp.y;
            if (dx * dx + dy * dy < 20 * 20) remove_peg(g, j);   // leave room for Nubby
        }
        g->pegs[g->npegs++] = bp;
    }

    int mults = 3 + (game_has(g, ITEM_MULTI) ? 2 : 0);
    for (int m = 0; m < mults; m++) {
        int i = pick_plus_peg(g);
        if (i >= 0) g->pegs[i].type = PEG_MULT;
    }
    for (int c = 0; c < 4; c++) {
        int i = pick_plus_peg(g);
        if (i >= 0) g->pegs[i].type = PEG_COIN;
    }
}

void game_start_round(Game *g)
{
    g->round++;
    g->target = game_target(g->round);
    g->round_score = 0;
    g->drops_left = BASE_DROPS + (game_has(g, ITEM_EXTRA) ? 1 : 0);
    g->dropping = 0;
    g->last_score = 0;
    make_board(g);
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

void game_drop(Game *g, int x)
{
    if (x < BOARD_L + NUBBY_R + 1) x = BOARD_L + NUBBY_R + 1;
    if (x > BOARD_R - NUBBY_R - 1) x = BOARD_R - NUBBY_R - 1;
    for (int i = 0; i < g->npegs; i++) g->pegs[i].lit = g->pegs[i].flash = 0;
    g->x = FIX(x);
    g->y = FIX(DROP_Y);
    g->vx = rand_below(g, 33) - 16;           // a whisker of sideways drift
    g->vy = 0;
    g->points = game_has(g, ITEM_BOOST) ? 5 : 0;
    g->mult = 1;
    g->hits = 0;
    g->frames = 0;
    g->slot = -1;
    g->still = 0;
    g->bumps = 0;
    g->drops_left--;
    g->dropping = 1;
}

static void score_peg(Game *g, Peg *p, Events *ev)
{
    if (p->type == PEG_BUMPER) {
        if (p->flash || g->bumps >= MAX_BUMPS) return;
        p->flash = BUMPER_COOLDOWN;
        g->bumps++;
        g->points += game_has(g, ITEM_SPRING) ? 3 : 1;
        ev->bumper = 1;
        return;
    }
    if (p->lit) return;
    p->lit = 1;
    p->flash = 6;
    g->hits++;
    switch (p->type) {
    case PEG_PLUS:
        g->points += game_has(g, ITEM_HEAVY) ? 2 : 1;
        ev->plus = 1;
        break;
    case PEG_MULT:
        g->mult++;
        ev->mult = 1;
        break;
    case PEG_COIN:
        g->points++;
        g->coins++;
        ev->coin = 1;
        break;
    }
    if (game_has(g, ITEM_LUCKY) && g->hits % 7 == 0) {
        g->points += 7;
        ev->lucky = 1;
    }
}

// Bounce Nubby off a circle at (cx, cy) pixels with radius r. Returns 1 on contact.
static int collide_circle(Game *g, int cx, int cy, int r, int bumper)
{
    int32_t dx = g->x - FIX(cx), dy = g->y - FIX(cy);
    int32_t reach = FIX(NUBBY_R + r);
    if (dx >= reach || dx <= -reach || dy >= reach || dy <= -reach) return 0;
    uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
    if (d2 >= (uint32_t)reach * (uint32_t)reach) return 0;
    int32_t d = isqrt(d2);                      // 8.8
    int32_t nx, ny;
    if (d < 16) {                               // dead centre: push straight up
        nx = 0;
        ny = -256;
        d = 0;
    } else {
        nx = dx * 256 / d;
        ny = dy * 256 / d;
    }
    // push out of the peg
    g->x += nx * (reach - d) / 256;
    g->y += ny * (reach - d) / 256;
    int32_t vn = (g->vx * nx + g->vy * ny) / 256;
    if (bumper) {
        int32_t out = vn < 0 ? -vn : 0;
        if (out < BUMPER_KICK) out = BUMPER_KICK;
        g->vx += nx * (out - vn) / 256;
        g->vy += ny * (out - vn) / 256;
    } else if (vn < 0) {
        int32_t j = vn * (256 + PEG_BOUNCE) / 256;
        g->vx -= nx * j / 256;
        g->vy -= ny * j / 256;
        // tiny random nudge so Nubby never balances on top of a peg
        g->vx += rand_below(g, 17) - 8;
    }
    return 1;
}

static void substep(Game *g, Events *ev)
{
    g->vy += GRAVITY / SUBSTEPS;
    if (g->vx > MAX_SPEED) g->vx = MAX_SPEED;
    if (g->vx < -MAX_SPEED) g->vx = -MAX_SPEED;
    if (g->vy > MAX_SPEED) g->vy = MAX_SPEED;
    if (g->vy < -MAX_SPEED) g->vy = -MAX_SPEED;
    g->x += g->vx / SUBSTEPS;
    g->y += g->vy / SUBSTEPS;

    // side walls, or the walls of the bucket Nubby fell into
    int left = BOARD_L, right = BOARD_R;
    if (g->slot >= 0) {
        left = BOARD_L + g->slot * BUCKET_W + 1;
        right = left + BUCKET_W - 2;
    }
    if (g->x < FIX(left + NUBBY_R)) {
        g->x = FIX(left + NUBBY_R);
        if (g->vx < 0) g->vx = -g->vx * WALL_BOUNCE / 256;
    }
    if (g->x > FIX(right - NUBBY_R)) {
        g->x = FIX(right - NUBBY_R);
        if (g->vx > 0) g->vx = -g->vx * WALL_BOUNCE / 256;
    }

    if (g->y < FIX(CEILING_Y + NUBBY_R)) {
        g->y = FIX(CEILING_Y + NUBBY_R);
        if (g->vy < 0) g->vy = -g->vy * WALL_BOUNCE / 256;
    }

    for (int i = 0; i < g->npegs; i++) {
        Peg *p = &g->pegs[i];
        int bumper = p->type == PEG_BUMPER;
        int kick = bumper && !p->flash && g->bumps < MAX_BUMPS;
        if (collide_circle(g, p->x, p->y, bumper ? BUMPER_R : PEG_R, kick)) score_peg(g, p, ev);
    }
    // tops of the bucket dividers
    for (int k = 1; k < NUM_BUCKETS; k++) collide_circle(g, BOARD_L + k * BUCKET_W, BUCKET_Y, 1, 0);

    if (g->slot < 0 && g->y >= FIX(BUCKET_Y)) {
        int s = ((g->x >> 8) - BOARD_L) / BUCKET_W;
        g->slot = s < 0 ? 0 : s >= NUM_BUCKETS ? NUM_BUCKETS - 1 : s;
    }
}

int game_step(Game *g, Events *ev)
{
    ev->plus = ev->mult = ev->coin = ev->bumper = ev->lucky = ev->landed = 0;
    for (int i = 0; i < g->npegs; i++)
        if (g->pegs[i].flash) g->pegs[i].flash--;
    if (!g->dropping) return 0;

    for (int s = 0; s < SUBSTEPS; s++) substep(g, ev);
    g->frames++;

    // if Nubby comes to rest on something, give it a shove
    int slow = g->vx < 40 && g->vx > -40 && g->vy < 40 && g->vy > -40;
    g->still = slow ? g->still + 1 : 0;
    if (g->still > 30) {
        g->vx = (game_rand(g) & 1) ? FIX(1) : -FIX(1);
        g->vy = -FIX(1);
        g->still = 0;
    }
    ev->hits = g->hits;

    int stuck = g->frames > DROP_TIMEOUT;
    if (g->y >= FIX(FLOOR_Y) || stuck) {
        if (g->slot < 0) {
            int s = ((g->x >> 8) - BOARD_L) / BUCKET_W;
            g->slot = s < 0 ? 0 : s >= NUM_BUCKETS ? NUM_BUCKETS - 1 : s;
        }
        g->last_bucket = g->slot;
        g->last_score = g->points * g->mult * game_bucket_mult(g, g->slot);
        g->round_score += g->last_score;
        g->total_score += g->last_score;
        g->dropping = 0;
        ev->landed = 1;
        return 1;
    }
    return 0;
}

RoundState game_round_state(const Game *g)
{
    if (g->dropping) return ROUND_PLAYING;
    if (g->round_score >= g->target) return ROUND_CLEARED;
    if (g->drops_left <= 0) return ROUND_FAILED;
    return ROUND_PLAYING;
}

int game_finish_round(Game *g)
{
    int earned = 3 + g->drops_left;
    if (game_has(g, ITEM_PIGGY)) earned += (g->coins + earned) / 4;
    g->coins += earned;
    return earned;
}

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
    return 1;
}
