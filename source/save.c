#include <fat.h>
#include <stdio.h>
#include <string.h>
#include "save.h"

#define SAVE_PATH "fat:/nubby-ds.sav"
#define SAVE_MAGIC 0x4255554Eu    // "NUUB"
#define SAVE_VERSION 1

SaveData save;
int save_available;

typedef struct {
    uint32_t magic;
    uint32_t version;
    SaveData data;
    uint32_t checksum;
} SaveBlock;

static uint32_t checksum(const SaveBlock *b)
{
    const uint8_t *p = (const uint8_t *)b;
    uint32_t sum = 0x1234;
    for (unsigned i = 0; i < sizeof(SaveBlock) - sizeof(uint32_t); i++)
        sum = sum * 31 + p[i];
    return sum;
}

void save_load(void)
{
    memset(&save, 0, sizeof(save));
    save_available = fatInitDefault();
    if (!save_available) return;
    SaveBlock b;
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f) return;
    int ok = fread(&b, sizeof(b), 1, f) == 1;
    fclose(f);
    if (ok && b.magic == SAVE_MAGIC && b.version == SAVE_VERSION && b.checksum == checksum(&b))
        save = b.data;
}

void save_write(void)
{
    if (!save_available) return;
    SaveBlock b;
    memset(&b, 0, sizeof(b));
    b.magic = SAVE_MAGIC;
    b.version = SAVE_VERSION;
    b.data = save;
    b.checksum = checksum(&b);
    FILE *f = fopen(SAVE_PATH, "wb");
    if (!f) return;
    fwrite(&b, sizeof(b), 1, f);
    fclose(f);
}
