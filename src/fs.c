#include "fs.h"
#include "string.h"

static fs_file_t files[FS_MAX_FILES];

void fs_init(void) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].size = 0;
        files[i].name[0] = '\0';
    }
}

int fs_create(const char* name) {
    /* Check for duplicate */
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, name) == 0)
            return -1;  /* Already exists */
    }
    /* Find free slot */
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
            strncpy(files[i].name, name, FS_MAX_NAME - 1);
            files[i].name[FS_MAX_NAME - 1] = '\0';
            files[i].size = 0;
            files[i].used = 1;
            return i;
        }
    }
    return -1;  /* No free slots */
}

int fs_write(int fd, const char* data, uint32_t len) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    if (len > FS_MAX_SIZE) len = FS_MAX_SIZE;
    memcpy(files[fd].data, data, len);
    files[fd].size = len;
    return (int)len;
}

int fs_read(int fd, char* buf, uint32_t maxlen) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    uint32_t n = files[fd].size;
    if (n > maxlen) n = maxlen;
    memcpy(buf, files[fd].data, n);
    return (int)n;
}

int fs_delete(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && strcmp(files[i].name, name) == 0) {
            files[i].used = 0;
            files[i].size = 0;
            files[i].name[0] = '\0';
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
