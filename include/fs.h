#ifndef MUOS_FS_H
#define MUOS_FS_H
#include "types.h"

#define FS_MAX_FILES    64
#define FS_MAX_NAME     32
#define FS_MAX_SIZE     1024

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
void fs_list(void);
int  fs_count(void);
const char* fs_name(int idx);
uint32_t fs_size(int idx);

#endif
