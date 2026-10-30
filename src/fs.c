#include "fs.h"
#include "string.h"
#include "rtc.h"
#include "ata.h"
#include "part.h"
#include "serial.h"

static fs_file_t files[FS_MAX_FILES];
static fs_dir_t  dirs[FS_MAX_DIRS];

/* MuFS write-through helpers (defined at the bottom of this file). */
static void persist_meta(void);
static void persist_slot(int i);
static void persist_slot_range(int i, uint32_t offset, uint32_t len);

static bool_t has_slash(const char* s) {
    for (; *s; s++) if (*s == '/') return true;
    return false;
}

static bool_t file_exists(const char* path) {
    if (!*path) return false;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && !files[i].deleted &&
            strcmp(files[i].name, path) == 0) return true;
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
        files[i].deleted = 0;
        files[i].refs = 0;
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
            files[i].deleted = 0;
            files[i].refs = 0;
            files[i].atime = files[i].mtime = files[i].ctime = rtc_now();
            persist_meta();
            persist_slot(i);
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
            persist_meta();
            return 0;
        }
    }
    return -1;
}

bool_t fs_dir_is_empty(const char* path) {
    if (!dir_exists(path)) return false;
    uint32_t n = strlen(path);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used || files[i].deleted) continue;
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
            persist_meta();
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
        if (files[i].used && !files[i].deleted &&
            child_name(path, files[i].name, &name)) n++;
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
        if (!files[i].used || files[i].deleted) continue;
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
    persist_meta();
    persist_slot(fd);
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
        if (files[i].used && !files[i].deleted && strcmp(files[i].name, name) == 0) {
            /* Keep the data alive while open fds still point at this slot. */
            files[i].name[0] = '\0';
            files[i].deleted = 1;
            files[i].ctime = rtc_now();
            if (files[i].refs == 0) {
                files[i].used = 0;
                files[i].size = 0;
                files[i].deleted = 0;
                files[i].atime = files[i].mtime = files[i].ctime = 0;
            }
            persist_meta();
            return 0;
        }
    }
    return -1;
}

int fs_open(const char* name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && !files[i].deleted &&
            strcmp(files[i].name, name) == 0) return i;
    }
    return -1;
}

int fs_ref(int slot) {
    if (slot < 0 || slot >= FS_MAX_FILES || !files[slot].used) return -1;
    files[slot].refs++;
    return 0;
}

int fs_unref(int slot) {
    if (slot < 0 || slot >= FS_MAX_FILES || !files[slot].used) return -1;
    if (files[slot].refs > 0) files[slot].refs--;
    if (files[slot].refs == 0 && files[slot].deleted) {
        files[slot].used = 0;
        files[slot].size = 0;
        files[slot].deleted = 0;
        files[slot].atime = files[slot].mtime = files[slot].ctime = 0;
        persist_meta();
    }
    return 0;
}

int fs_rename(int slot, const char* new_name) {
    if (slot < 0 || slot >= FS_MAX_FILES || !files[slot].used ||
        files[slot].deleted) return -1;
    char parent[FS_MAX_NAME];
    const char* base = NULL;
    if (!split_parent(new_name, parent, &base)) return -1;
    if (!dir_exists(parent) || dir_exists(new_name)) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (i != slot && files[i].used && !files[i].deleted &&
            strcmp(files[i].name, new_name) == 0)
            return -1;
    }
    strncpy(files[slot].name, new_name, FS_MAX_NAME - 1);
    files[slot].name[FS_MAX_NAME - 1] = '\0';
    files[slot].ctime = rtc_now();
    persist_meta();
    return 0;
}

/* True when name is `prefix` itself or a path below it; *tail is the offset
 * of the '/' or the terminating NUL. */
static bool_t name_under(const char* name, const char* prefix, uint32_t plen,
                         uint32_t* tail) {
    if (strncmp(name, prefix, plen) != 0) return false;
    if (name[plen] != '/' && name[plen] != '\0') return false;
    *tail = plen;
    return true;
}

/* Rename a whole directory subtree. The target must not exist and must not
 * be inside the source; all new names are validated before any mutation. */
int fs_rename_dir(const char* old_path, const char* new_path) {
    if (!old_path || !*old_path || !new_path || !*new_path) return -1;
    if (!dir_exists(old_path)) return -1;
    uint32_t ol = strlen(old_path);
    uint32_t nl = strlen(new_path);
    if (ol >= FS_MAX_NAME || nl >= FS_MAX_NAME) return -1;

    char parent[FS_MAX_NAME];
    const char* base = NULL;
    if (!split_parent(new_path, parent, &base)) return -1;
    if (!dir_exists(parent)) return -1;
    if (file_exists(new_path) || dir_exists(new_path)) return -1;
    if (strncmp(new_path, old_path, ol) == 0 &&
        (new_path[ol] == '/' || new_path[ol] == '\0')) return -1;

    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) continue;
        uint32_t tail = 0;
        if (!name_under(dirs[i].name, old_path, ol, &tail)) continue;
        if (nl + strlen(dirs[i].name + tail) >= FS_MAX_NAME) return -1;
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used || files[i].deleted) continue;
        uint32_t tail = 0;
        if (!name_under(files[i].name, old_path, ol, &tail)) continue;
        if (nl + strlen(files[i].name + tail) >= FS_MAX_NAME) return -1;
    }

    for (int i = 0; i < FS_MAX_DIRS; i++) {
        if (!dirs[i].used) continue;
        uint32_t tail = 0;
        if (!name_under(dirs[i].name, old_path, ol, &tail)) continue;
        char tmp[FS_MAX_NAME];
        uint32_t rest = strlen(dirs[i].name + tail);
        memcpy(tmp, new_path, nl);
        memcpy(tmp + nl, dirs[i].name + tail, rest + 1);
        memcpy(dirs[i].name, tmp, nl + rest + 1);
    }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used || files[i].deleted) continue;
        uint32_t tail = 0;
        if (!name_under(files[i].name, old_path, ol, &tail)) continue;
        char tmp[FS_MAX_NAME];
        uint32_t rest = strlen(files[i].name + tail);
        memcpy(tmp, new_path, nl);
        memcpy(tmp + nl, files[i].name + tail, rest + 1);
        memcpy(files[i].name, tmp, nl + rest + 1);
        files[i].ctime = rtc_now();
    }
    persist_meta();
    return 0;
}

int fs_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used && !files[i].deleted) n++;
    return n;
}

/* Slots actually occupied (including unlinked-but-open files). */
int fs_used_slots(void) {
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (files[i].used) n++;
    return n;
}

const char* fs_name(int idx) {
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && !files[i].deleted) {
            if (count == idx) return files[i].name;
            count++;
        }
    }
    return NULL;
}

uint32_t fs_size(int idx) {
    int count = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (files[i].used && !files[i].deleted) {
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
    persist_meta();
    persist_slot(fd);
    return 0;
}

/* Resize a file to an exact length. Growing zero-fills the new tail;
 * shrinking only drops the size (stale bytes are never exposed because
 * every reader stops at size). */
int fs_truncate(int fd, uint32_t len) {
    if (fd < 0 || fd >= FS_MAX_FILES || !files[fd].used) return -1;
    if (len > FS_MAX_SIZE) return -1;
    if (len > files[fd].size)
        memset(files[fd].data + files[fd].size, 0, len - files[fd].size);
    files[fd].size = len;
    files[fd].mtime = files[fd].ctime = rtc_now();
    persist_meta();
    persist_slot(fd);
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
    persist_meta();
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
    persist_meta();
    persist_slot_range(fd, offset, len);
    return (int)len;
}

/* ===================== MuFS persistence on ATA =====================
 * On-disk layout (512-byte sectors):
 *   0      superblock (magic/version/generation)
 *   2..5   file metadata table (16 × 88 bytes)
 *   6..8   directory metadata table (16 × 68 bytes)
 *   16..   data area, slot i = 64KB at LBA 16 + i*128
 * Mutations write metadata through immediately and, for data, only the
 * touched sector range. `fs_sync()` rewrites the whole image. */

#define MUFS_MAGIC        "MUFS"
#define MUFS_VERSION      1u
#define MUFS_SB_LBA       0
#define MUFS_META_LBA     2
#define MUFS_DIR_LBA      6
#define MUFS_DATA_LBA     16
#define MUFS_SLOT_SECTORS 128          /* 64KB per file slot */
/* Metadata write-ahead journal: sector 9 header, 10-12 meta copy,
 * 13-15 directory copy. A valid header means the commit was interrupted
 * and the copies must be replayed at mount time. */
#define MUFS_JL_LBA       9
#define MUFS_JMETA_LBA    10
#define MUFS_JDIR_LBA     13
#define MUFS_J_MAGIC      0x4D554C4Au    /* 'MULJ' */

typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t slot_sectors;
    uint32_t file_slots;
    uint32_t dir_slots;
    uint32_t generation;
    uint32_t reserved[7];
} __attribute__((packed)) mufs_sb_t;

typedef struct {
    char     name[FS_MAX_NAME];
    uint32_t size;
    uint32_t used;
    uint32_t deleted;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
} __attribute__((packed)) mufs_file_meta_t;

typedef struct {
    char     name[FS_MAX_NAME];
    uint32_t used;
} __attribute__((packed)) mufs_dir_meta_t;

static bool_t persist_active = false;
static uint32_t persist_gen = 0;
static mufs_file_meta_t meta_buf[FS_MAX_FILES];
static mufs_dir_meta_t  dmeta_buf[FS_MAX_DIRS];

static uint32_t slot_lba(int i) {
    return MUFS_DATA_LBA + (uint32_t)i * MUFS_SLOT_SECTORS;
}

/* Rewrite the two metadata tables (3 + 3 sectors). */
static void persist_meta(void) {
    if (!persist_active) return;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        memcpy(meta_buf[i].name, files[i].name, FS_MAX_NAME);
        meta_buf[i].size = files[i].size;
        meta_buf[i].used = files[i].used;
        meta_buf[i].deleted = files[i].deleted;
        meta_buf[i].atime = files[i].atime;
        meta_buf[i].mtime = files[i].mtime;
        meta_buf[i].ctime = files[i].ctime;
    }
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        memcpy(dmeta_buf[i].name, dirs[i].name, FS_MAX_NAME);
        dmeta_buf[i].used = dirs[i].used;
    }

    /* Journal: data copies first, then the valid header; after the real
     * metadata is written, clear the header. */
    uint32_t jh[8];
    memset(jh, 0, sizeof(jh));
    jh[0] = MUFS_J_MAGIC;
    jh[1] = persist_gen;
    ata_write_bytes(MUFS_JMETA_LBA * 512, meta_buf, (uint32_t)sizeof(meta_buf));
    ata_write_bytes(MUFS_JDIR_LBA * 512, dmeta_buf, (uint32_t)sizeof(dmeta_buf));
    ata_write_bytes(MUFS_JL_LBA * 512, jh, sizeof(jh));

    ata_write_bytes(MUFS_META_LBA * 512, meta_buf, (uint32_t)sizeof(meta_buf));
    ata_write_bytes(MUFS_DIR_LBA * 512, dmeta_buf, (uint32_t)sizeof(dmeta_buf));

    jh[0] = 0;
    ata_write_bytes(MUFS_JL_LBA * 512, jh, sizeof(jh));
}

/* Write the whole file body of one slot (size rounded to sectors). */
static void persist_slot(int i) {
    if (!persist_active || i < 0 || i >= FS_MAX_FILES) return;
    uint32_t n = (files[i].size + 511u) & ~511u;
    if (n == 0) return;
    ata_write_bytes(slot_lba(i) * 512, files[i].data, n);
}

/* Write only the sectors touched by one write_at. */
static void persist_slot_range(int i, uint32_t offset, uint32_t len) {
    if (!persist_active || i < 0 || i >= FS_MAX_FILES || len == 0) return;
    uint32_t first = offset & ~511u;
    uint32_t end = (offset + len + 511u) & ~511u;
    if (end > FS_MAX_SIZE) end = FS_MAX_SIZE;
    if (end <= first) return;
    ata_write_bytes(slot_lba(i) * 512 + first, files[i].data + first, end - first);
}

void fs_persist_mount(void) {
    persist_active = false;
    persist_gen = 0;
    if (!ata_present()) return;

    mufs_sb_t sb;

    /* Replay an interrupted metadata commit before reading the real tables. */
    {
        uint32_t jh[8];
        if (ata_read_bytes(MUFS_JL_LBA * 512, jh, sizeof(jh)) == (int)sizeof(jh) &&
            jh[0] == MUFS_J_MAGIC) {
            if (ata_read_bytes(MUFS_JMETA_LBA * 512, meta_buf,
                               (uint32_t)sizeof(meta_buf)) == (int)sizeof(meta_buf) &&
                ata_read_bytes(MUFS_JDIR_LBA * 512, dmeta_buf,
                               (uint32_t)sizeof(dmeta_buf)) == (int)sizeof(dmeta_buf)) {
                ata_write_bytes(MUFS_META_LBA * 512, meta_buf,
                                (uint32_t)sizeof(meta_buf));
                ata_write_bytes(MUFS_DIR_LBA * 512, dmeta_buf,
                                (uint32_t)sizeof(dmeta_buf));
                serial_write("[mufs] replayed metadata journal\n");
            }
            jh[0] = 0;
            ata_write_bytes(MUFS_JL_LBA * 512, jh, sizeof(jh));
        }
    }

    if (ata_read_bytes(MUFS_SB_LBA * 512, &sb, sizeof(sb)) == (int)sizeof(sb) &&
        memcmp(sb.magic, MUFS_MAGIC, 4) == 0 &&
        sb.version == MUFS_VERSION &&
        sb.slot_sectors == MUFS_SLOT_SECTORS &&
        sb.file_slots == FS_MAX_FILES && sb.dir_slots == FS_MAX_DIRS) {
        if (ata_read_bytes(MUFS_META_LBA * 512, meta_buf,
                           (uint32_t)sizeof(meta_buf)) != (int)sizeof(meta_buf))
            return;
        if (ata_read_bytes(MUFS_DIR_LBA * 512, dmeta_buf,
                           (uint32_t)sizeof(dmeta_buf)) != (int)sizeof(dmeta_buf))
            return;

        for (int i = 0; i < FS_MAX_FILES; i++) {
            memcpy(files[i].name, meta_buf[i].name, FS_MAX_NAME);
            files[i].name[FS_MAX_NAME - 1] = '\0';
            files[i].used = meta_buf[i].used;
            files[i].deleted = meta_buf[i].deleted;
            files[i].refs = 0;
            files[i].size = meta_buf[i].size;
            if (files[i].size > FS_MAX_SIZE) files[i].size = FS_MAX_SIZE;
            files[i].atime = meta_buf[i].atime;
            files[i].mtime = meta_buf[i].mtime;
            files[i].ctime = meta_buf[i].ctime;
            if (files[i].used && files[i].size) {
                ata_read_bytes(slot_lba(i) * 512, files[i].data, files[i].size);
            }
        }
        for (int i = 0; i < FS_MAX_DIRS; i++) {
            memcpy(dirs[i].name, dmeta_buf[i].name, FS_MAX_NAME);
            dirs[i].name[FS_MAX_NAME - 1] = '\0';
            dirs[i].used = dmeta_buf[i].used;
        }
        persist_gen = sb.generation;
        persist_active = true;
        return;
    }

    /* A partitioned disk belongs to FAT32/ext2, not to whole-disk MuFS:
     * never format it, just stay memory-only. */
    if (part_is_partitioned()) return;

    /* No valid image: format an empty namespace. */
    for (int i = 0; i < FS_MAX_FILES; i++) {
        memset(&files[i], 0, sizeof(files[i]));
        memset(&meta_buf[i], 0, sizeof(meta_buf[i]));
    }
    for (int i = 0; i < FS_MAX_DIRS; i++) {
        memset(&dirs[i], 0, sizeof(dirs[i]));
        memset(&dmeta_buf[i], 0, sizeof(dmeta_buf[i]));
    }
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, MUFS_MAGIC, 4);
    sb.version = MUFS_VERSION;
    sb.slot_sectors = MUFS_SLOT_SECTORS;
    sb.file_slots = FS_MAX_FILES;
    sb.dir_slots = FS_MAX_DIRS;
    sb.generation = 1;
    persist_gen = 1;
    persist_active = true;
    ata_write_bytes(MUFS_SB_LBA * 512, &sb, sizeof(sb));
    persist_meta();
}

int fs_sync(void) {
    if (!persist_active) return 0;

    persist_gen++;
    mufs_sb_t sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, MUFS_MAGIC, 4);
    sb.version = MUFS_VERSION;
    sb.slot_sectors = MUFS_SLOT_SECTORS;
    sb.file_slots = FS_MAX_FILES;
    sb.dir_slots = FS_MAX_DIRS;
    sb.generation = persist_gen;
    ata_write_bytes(MUFS_SB_LBA * 512, &sb, sizeof(sb));
    persist_meta();
    for (int i = 0; i < FS_MAX_FILES; i++) persist_slot(i);
    return 0;
}

bool_t fs_persist_active(void) { return persist_active; }
uint32_t fs_persist_generation(void) { return persist_gen; }
