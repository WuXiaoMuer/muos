#ifndef MUOS_FAT_H
#define MUOS_FAT_H
#include "types.h"

/* Read-only FAT32 driver (MBR/GPT partition or bare volume). Paths use
 * forward slashes and are matched case-insensitively; long names are
 * assembled from LFN entries when present. */
bool_t fat_init(void);
bool_t fat_present(void);
const char* fat_label(void);
uint32_t fat_part_lba(void);

/* Append a human-readable directory listing to `out` (NUL-terminated).
 * Returns bytes written, or negative on error. */
int fat_list(const char* path, char* out, uint32_t cap);
/* Copy at most `cap` bytes of the file into `buf`. Returns the byte count
 * actually copied, or negative on error. */
int fat_read_file(const char* path, void* buf, uint32_t cap);

#endif
