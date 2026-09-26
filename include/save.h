// Best results, kept in cartridge SRAM.
#ifndef SAVE_H
#define SAVE_H

#include <stdint.h>

typedef struct {
    int32_t best_round;       // furthest round reached
    int32_t best_score;       // best total score in a run
} SaveData;

extern SaveData save;

void save_load(void);         // falls back to defaults if SRAM is blank or corrupt
void save_write(void);

#endif
