#ifndef MUOS_FS_H
#define MUOS_FS_H
#include "types.h"

#define FS_MAX_FILES    16
#define FS_MAX_NAME     32
#define FS_MAX_SIZE     65536   /* 64KB per file; room for a static i386 ELF */

typedef struct {
    char   name[FS_MAX_NAME];
    char   data[FS_MAX_SIZE];
    uint32_t size;
    uint32_t used;    /* 1 = in use */
} fs_file_t;

void fs_init(void);
int  fs_create(const char* name);
int  fs_write(int fd, const char* data, uint32_t len);
int  fs_read(int fd, char* buf, uint32_t maxlen);
int  fs_delete(const char* name);
int  fs_open(const char* name);
int  fs_rename(int slot, const char* new_name);   /* 0 = ok, -1 = bad/duplicate */
int  fs_count(void);
const char* fs_name(int idx);
uint32_t fs_size(int idx);   /* size of the idx-th *existing* file (ls) */

/* fd-oriented helpers used by the syscall layer (fd = slot index) */
const char* fs_fd_data(int fd);
uint32_t    fs_fd_size(int fd);
int         fs_fd_truncate(int fd);
int         fs_read_at(int fd, uint32_t offset, char* buf, uint32_t len);
int         fs_write_at(int fd, uint32_t offset, const char* data, uint32_t len);

#endif
