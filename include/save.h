// Best results, kept in a small file on the SD card (flash carts, or an
// emulator's DLDI SD image). If there is no card the game still runs, it just
// can't remember scores between sessions.
#ifndef SAVE_H
#define SAVE_H

#include <stdint.h>

typedef struct {
    int32_t best_round;       // furthest round reached
    int32_t best_score;       // best single launch
} SaveData;

extern SaveData save;
extern int save_available;

void save_load(void);         // falls back to defaults if there's no file or it's corrupt
void save_write(void);

#endif
