#include "fs.h"
#include "string.h"
#include "rtc.h"

static fs_file_t files[FS_MAX_FILES];
static fs_dir_t  dirs[FS_MAX_DIRS];

static bool_t has_slash(const char* s) {
    for (; *s; s++) if (*s == '/') return true;
    return false;
}

static bool_t file_exists(const char* path) {
    if (!*path) return false;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, path) == 0) return true;
    }
    return false;
}

static bool_t dir_exists(const char* path) {
    if (!*path) return true;                  /* root */
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && strcmp(dirs[i].name, path) == 0) return true;
    }
    return false;
}

/* Split "a/b/c" into parent "a/b" and base "c". Root has an empty parent. */
static bool_t split_parent(const char* path, char* parent, const char** base) {
    if (!path || !*path || path[0] == '/' || strlen(path) >= FS_MAX_NAME)
        return false;
    const char* slash = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/') {
            if (p == path || p[1] == '\0' || p[1] == '/') return false;
            slash = p;
        }
    }
    if (!slash) {
        parent[0] = '\0';
        *base = path;
        return true;
    }
    uint32_t n = (uint32_t)(slash - path);
    memcpy(parent, path, n);
    parent[n] = '\0';
    *base = slash + 1;
    return true;
}

void fs_init(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].name[0] = '\0';
        files[i].atime = files[i].mtime = files[i].ctime = 0;
    }
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        dirs[i].used = 0;
        dirs[i].name[0] = '\0';
    }
}

bool_t fs_is_file(const char* path) {
    return file_exists(path);
}

bool_t fs_is_dir(const char* path) {
    return dir_exists(path);
}

bool_t fs_parent_is_dir(const char* path) {
    char parent[FS_MAX_NAME];
    const char* base = NULL;
    if (!split_parent(path, parent, &base)) return false;
    return dir_exists(parent);
}

int fs_create(const char* path) {
    char parent[FS_MAX_NAME];
    const char* base = NULL;
    if (!split_parent(path, parent, &base)) return -1;
    if (!dir_exists(parent)) return -1;              /* parent not a directory */
    if (file_exists(path) || dir_exists(path)) return -1;

    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            strncpy(files[i].name, path, FS_MAX_NAME - 1);
            files[i].name[FS_MAX_NAME - 1] = '\0';
            files[i].size = 0;
            files[i].used = 1;
            files[i].atime = files[i].mtime = files[i].ctime = rtc_now();
            return i;
        }
    }
    return -1;  /* No free slots */
}

int fs_mkdir(const char* path) {
    char parent[FS_MAX_NAME];
    const char* base = NULL;
    if (!split_parent(path, parent, &base)) return -1;
    if (!dir_exists(parent)) return -1;
    if (file_exists(path) || dir_exists(path)) return -1;

    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) {
            strncpy(dirs[i].name, path, FS_MAX_NAME - 1);
            dirs[i].name[FS_MAX_NAME - 1] = '\0';
            dirs[i].used = 1;
            return 0;
        }
    }
    return -1;
}

bool_t fs_dir_is_empty(const char* path) {
    if (!dir_exists(path)) return false;
    uint32_t n = strlen(path);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (strncmp(files[i].name, path, n) == 0 && files[i].name[n] == '/')
            return false;
    }
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) continue;
        if (strncmp(dirs[i].name, path, n) == 0 && dirs[i].name[n] == '/')
            return false;
    }
    return true;
}

int fs_rmdir(const char* path) {
    if (!*path || !dir_exists(path) || !fs_dir_is_empty(path)) return -1;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && strcmp(dirs[i].name, path) == 0) {
            dirs[i].used = 0;
            dirs[i].name[0] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Enumerate a directory: subdirectories first, then files. */
static bool_t child_name(const char* dir, const char* path, const char** out) {
    if (!*dir) {
        if (has_slash(path)) return false;
        *out = path;
        return true;
    }
    uint32_t n = strlen(dir);
    if (strncmp(path, dir, n) != 0 || path[n] != '/') return false;
    const char* rest = path + n + 1;
    if (!*rest || has_slash(rest)) return false;
    *out = rest;
    return true;
}

int fs_dir_count(const char* path) {
    if (!dir_exists(path)) return -1;
    int n = 0;
    const char* name;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (dirs[i].used && child_name(path, dirs[i].name, &name)) n++;
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && child_name(path, files[i].name, &name)) n++;
    }
    return n;
}

bool_t fs_dir_get(const char* path, int idx, fs_dirent_t* out) {
    if (!dir_exists(path) || idx < 0 || !out) return false;
    const char* name;
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) continue;
        if (!child_name(path, dirs[i].name, &name)) continue;
        if (idx-- == 0) {
            strncpy(out->name, name, FS_MAX_NAME - 1);
            out->name[FS_MAX_NAME - 1] = '\0';
            out->is_dir = true;
            out->size = 0;
            out->slot = -1;
            return true;
        }
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) continue;
        if (!child_name(path, files[i].name, &name)) continue;
        if (idx-- == 0) {
            strncpy(out->name, name, FS_MAX_NAME - 1);
            out->name[FS_MAX_NAME - 1] = '\0';
            out->is_dir = false;
            out->size = files[i].size;
            out->slot = i;
            return true;
        }
    }
    return false;
}

int fs_write(int fd, const char* data, uint32_t len) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    if (len > FS_MAX_SIZE) len = FS_MAX_SIZE;
    memcpy(files[fd].data, data, len);
    files[fd].size = len;
    files[fd].mtime = files[fd].ctime = rtc_now();
    return (int)len;
}

int fs_read(int fd, char* buf, uint32_t maxlen) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    uint32_t n = files[fd].size;
    if (n > maxlen) n = maxlen;
    memcpy(buf, files[fd].data, n);
    files[fd].atime = rtc_now();
    return (int)n;
}

int fs_delete(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, name) == 0) {
            files[i].used = 0;
            files[i].size = 0;
            files[i].name[0] = '\0';
            files[i].atime = files[i].mtime = files[i].ctime = 0;
            return 0;
        }
    }
    return -1;
}

int fs_open(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return i;
    }
    return -1;
}

int fs_rename(int slot, const char* new_name) {
    if (slot < 0 || slot >= FS_MAX_FILES || !files[slot].used) return -1;
    char parent[FS_MAX_NAME];
    const char* base = NULL;
    if (!split_parent(new_name, parent, &base)) return -1;
    if (!dir_exists(parent) || dir_exists(new_name)) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (i != slot && files[i].used && strcmp(files[i].name, new_name) == 0)
            return -1;
    }
    strncpy(files[slot].name, new_name, FS_MAX_NAME - 1);
    files[slot].name[FS_MAX_NAME - 1] = '\0';
    files[slot].ctime = rtc_now();
    return 0;
}

int fs_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used) n++;
    return n;
}

const char* fs_name(int idx) {
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            if (count == idx) return files[i].name;
            count++;
        }
    }
    return NULL;
}

uint32_t fs_size(int idx) {
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used) {
            if (count == idx) return files[i].size;
            count++;
        }
    }
    return 0;
}

/* ---- fd-oriented API (fd is the slot index, as fs_open returns) ---- */

const char* fs_fd_data(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return NULL;
    return files[fd].data;
}

uint32_t fs_fd_size(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return 0;
    return files[fd].size;
}

int fs_fd_truncate(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    files[fd].size = 0;
    files[fd].mtime = files[fd].ctime = rtc_now();
    return 0;
}

uint32_t fs_fd_atime(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return 0;
    return files[fd].atime;
}

uint32_t fs_fd_mtime(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return 0;
    return files[fd].mtime;
}

uint32_t fs_fd_ctime(int fd) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return 0;
    return files[fd].ctime;
}

void fs_set_times(int fd, uint32_t atime, uint32_t mtime) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return;
    files[fd].atime = atime;
    files[fd].mtime = mtime;
    files[fd].ctime = rtc_now();
}

int fs_read_at(int fd, uint32_t offset, char* buf, uint32_t len) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    if (offset >= files[fd].size) return 0;
    uint32_t n = files[fd].size - offset;
    if (n > len) n = len;
    memcpy(buf, files[fd].data + offset, n);
    files[fd].atime = rtc_now();
    return (int)n;
}

int fs_write_at(int fd, uint32_t offset, const char* data, uint32_t len) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    if (offset >= FS_MAX_SIZE) return -1;
    if (len > FS_MAX_SIZE - offset) len = FS_MAX_SIZE - offset;
    if (len) memcpy(files[fd].data + offset, data, len);
    if (offset + len > files[fd].size) files[fd].size = offset + len;
    files[fd].mtime = files[fd].ctime = rtc_now();
    return (int)len;
}
