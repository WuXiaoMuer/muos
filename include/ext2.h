#ifndef MUOS_EXT2_H
#define MUOS_EXT2_H
#include "types.h"

/* Minimal ext2 driver on the ATA block device. Reads use the 12 direct
 * blocks plus the single-indirect block. Writes create or overwrite a file
 * (block bitmap + inode bitmap allocation, directory entry insertion). */
bool_t ext2_init(void);
bool_t ext2_present(void);
const char* ext2_volume(void);
int ext2_list(const char* path, char* out, uint32_t cap);
int ext2_read_file(const char* path, void* buf, uint32_t cap);
/* Create/truncate `path` and write `len` bytes. Returns len or -1. */
int ext2_write_file(const char* path, const void* data, uint32_t len);

#endif
