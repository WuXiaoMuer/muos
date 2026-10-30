#ifndef MUOS_PART_H
#define MUOS_PART_H
#include "types.h"

/* Partition table support (MBR and GPT). Runs on top of the raw ATA PIO
 * block device; the first partition found is what FAT32/ext2 drivers use. */
#define PART_MAX 8

typedef struct {
    uint64_t start_lba;
    uint64_t sectors;
    uint8_t  type;          /* MBR type byte; 0xEE for GPT entries */
    bool_t   gpt;
    char     name[24];
} part_t;

/* Read sector 0 and build the partition list. Returns true when a valid
 * table (MBR signature or GPT header) was found. */
bool_t part_init(void);
bool_t part_present(void);
/* True when sector 0 looks like a partition table instead of a bare MuFS
 * volume; MuFS must not format such a disk. */
bool_t part_is_partitioned(void);

int    part_count(void);
bool_t part_get(int index, part_t* out);
bool_t part_find_type(uint8_t mbr_type, int* index);

#endif
