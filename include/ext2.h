#ifndef MUOS_EXT2_H
#define MUOS_EXT2_H
#include "types.h"

/* Read-only ext2 (rev 0 / 128-byte inode) driver on the ATA block device.
 * Finds the first ext2 partition, reads inodes and blocks; supports the 12
 * direct blocks plus the single-indirect block. */
bool_t ext2_init(void);
bool_t ext2_present(void);
const char* ext2_volume(void);
int ext2_list(const char* path, char* out, uint32_t cap);
int ext2_read_file(const char* path, void* buf, uint32_t cap);

#endif
