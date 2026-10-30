#ifndef MUOS_FS_H
#define MUOS_FS_H
#include "types.h"

#define FS_MAX_FILES    16
#define FS_MAX_DIRS     16
#define FS_MAX_NAME     64      /* full path relative to root, no leading '/' */
#define FS_MAX_SIZE     65536   /* 64KB per file; room for a static i386 ELF */

typedef struct {
    char   name[FS_MAX_NAME];
    char   data[FS_MAX_SIZE];
    uint32_t size;
    uint32_t used;    /* 1 = in use */
    uint32_t deleted; /* unlinked but still open: name cleared, data kept */
    uint32_t refs;    /* open fd count referencing this slot */
    uint32_t atime;   /* UTC seconds since 1970 */
    uint32_t mtime;
    uint32_t ctime;
} fs_file_t;

typedef struct {
    char   name[FS_MAX_NAME];   /* path relative to root */
    uint32_t used;
} fs_dir_t;

typedef struct {
    char     name[FS_MAX_NAME];  /* child name (single component) */
    bool_t   is_dir;
    uint32_t size;
    int      slot;               /* file slot when !is_dir */
} fs_dirent_t;

void fs_init(void);
int  fs_create(const char* path);
int  fs_mkdir(const char* path);
int  fs_rmdir(const char* path);
bool_t fs_is_dir(const char* path);
bool_t fs_is_file(const char* path);
bool_t fs_parent_is_dir(const char* path);
bool_t fs_dir_is_empty(const char* path);
int  fs_dir_count(const char* path);
bool_t fs_dir_get(const char* path, int idx, fs_dirent_t* out);
int  fs_write(int fd, const char* data, uint32_t len);
int  fs_read(int fd, char* buf, uint32_t maxlen);
int  fs_delete(const char* path);
int  fs_open(const char* path);
int  fs_rename(int slot, const char* new_name);   /* 0 = ok, -1 = bad/duplicate */
int  fs_rename_dir(const char* old_path, const char* new_path);
int  fs_count(void);
int  fs_used_slots(void);   /* occupied slots, unlinked-but-open included */

/* Open-file lifetime: an unlinked file stays alive until the last fd
 * releases it, so existing readers/writers never dangle. */
int  fs_ref(int slot);
int  fs_unref(int slot);
const char* fs_name(int idx);
uint32_t fs_size(int idx);   /* size of the idx-th *existing* file (ls) */

/* fd-oriented helpers used by the syscall layer (fd = slot index) */
const char* fs_fd_data(int fd);
uint32_t    fs_fd_size(int fd);
int         fs_fd_truncate(int fd);
int         fs_truncate(int fd, uint32_t len);   /* resize: grow zero-fills */
uint32_t    fs_fd_atime(int fd);
uint32_t    fs_fd_mtime(int fd);
uint32_t    fs_fd_ctime(int fd);
void        fs_set_times(int fd, uint32_t atime, uint32_t mtime);
int         fs_read_at(int fd, uint32_t offset, char* buf, uint32_t len);
int         fs_write_at(int fd, uint32_t offset, const char* data, uint32_t len);

#endif
