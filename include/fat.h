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

/* VFS helpers: existence/type/size, and a random-access read starting at
 * `offset` (returns bytes copied, 0 past EOF, negative on error). */
bool_t fat_stat(const char* path, bool_t* is_dir, uint32_t* size);
int fat_read_at(const char* path, uint32_t offset, void* buf, uint32_t cap);

/* Create or overwrite a file (8.3 short name, root or existing subdir),
 * allocating clusters and updating both FAT copies. Returns bytes written
 * or negative on error. Writes are write-through (no cache). */
int fat_write_file(const char* path, const void* data, uint32_t len);

/* Delete a file (removes its LFN entries and frees the cluster chain).
 * Returns 0 on success, negative on error. */
int fat_delete_file(const char* path);
/* Create a directory (allocates one cluster, writes "." and "..").
 * Returns 0 on success, negative on error. */
int fat_mkdir(const char* path);
/* Rename a file inside the volume (entries only; data clusters stay). */
int fat_rename_file(const char* old_path, const char* new_path);

#endif
