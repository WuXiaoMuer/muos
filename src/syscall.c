#include "syscall.h"
#include "user.h"
#include "fs.h"
#include "gdt.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "string.h"

/* Linux i386 syscall numbers (the ABI MuOS targets). This is the starter
 * set needed by freestanding user programs; anything else is rejected
 * with -ENOSYS and logged to serial so real binaries can drive the TODO
 * list (see ROADMAP stage 4). */
#define SYS_exit       1
#define SYS_read       3
#define SYS_write      4
#define SYS_open       5
#define SYS_close      6
#define SYS_unlink    10
#define SYS_chdir     12
#define SYS_time      13
#define SYS_lseek     19
#define SYS_getpid    20
#define SYS_getuid    24
#define SYS_access    33
#define SYS_rename    38
#define SYS_dup       41
#define SYS_brk       45
#define SYS_getgid    47
#define SYS_geteuid   49
#define SYS_getegid   50
#define SYS_ioctl     54
#define SYS_fcntl     55
#define SYS_umask     60
#define SYS_dup2      63
#define SYS_getppid   64
#define SYS_getgroups 80
#define SYS_gettimeofday 78
#define SYS_stat     106
#define SYS_lstat    107
#define SYS_fstat    108
#define SYS_llseek   140
#define SYS_getdents 141
#define SYS_readv    145
#define SYS_writev   146
#define SYS_old_mmap  90
#define SYS_munmap    91
#define SYS_uname    122
#define SYS_modify_ldt 123
#define SYS_mprotect 125
#define SYS_rt_sigaction   174
#define SYS_rt_sigprocmask 175
#define SYS_getcwd   183
#define SYS_sendfile 187
#define SYS_clock_gettime 265
#define SYS_utimes   271
#define SYS_futimesat 299
#define SYS_utimensat 320
#define SYS_mmap2    192
#define SYS_stat64   195
#define SYS_lstat64  196
#define SYS_fstat64  197
#define SYS_getuid32        199
#define SYS_getgid32        200
#define SYS_getgroups32     205
#define SYS_geteuid32       201
#define SYS_getegid32       202
#define SYS_getdents64      220
#define SYS_fcntl64         221
#define SYS_set_thread_area 243
#define SYS_get_thread_area 244
#define SYS_exit_group      252
#define SYS_set_tid_address 258
/* 32-bit time64 ABI used by modern musl for clock_gettime. */
#define SYS_clock_gettime64 403
#define SYS_sendfile64      239
#define SYS_statx           383

#define ERR_EPERM      1
#define ERR_ENOENT     2
#define ERR_EBADF      9
#define ERR_ENOMEM    12
#define ERR_EFAULT    14
#define ERR_ENODEV    19
#define ERR_ENOTDIR   20
#define ERR_EISDIR    21
#define ERR_EINVAL    22
#define ERR_EMFILE    24
#define ERR_ENOSPC    28
#define ERR_ENOTTY    25
#define ERR_ENAMETOOLONG 36
#define ERR_ENOSYS    38

/* open(2) flags and mmap(2) constants (Linux i386 values) */
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_DIRECTORY 0x10000

/* i386 struct stat64 (96 bytes) and legacy struct stat (64 bytes). */
#define STAT64_SIZE 96
#define STAT_OLD_SIZE 64
#define STATX_SIZE 256
#define STATX_BASIC_STATS 0x7ffu
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFCHR 0020000u
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define UTS_FIELD 65          /* sizeof each utsname field */
#define UTS_SIZE  390         /* 6 fields */

static uint32_t err(uint32_t code) {
    return (uint32_t)(-(int32_t)code);
}

static uint32_t do_write(uint32_t fd, uint32_t buf, uint32_t count) {
    if (count == 0) return 0;
    if (!user_ptr_ok(buf, count)) return err(ERR_EFAULT);

    if (fd == 1 || fd == 2) {
        if (!user_stdio_is_open((int)fd)) return err(ERR_EBADF);
        for (uint32_t i = 0; i < count; i++) {
            char c = ((const char*)buf)[i];
            vga_putchar(c);
            serial_putchar(c);
        }
        return count;
    }

    if (user_fd_is_dir((int)fd)) return err(ERR_EISDIR);
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return err(ERR_EBADF);
    uint32_t off = user_fd_offset((int)fd);
    int n = fs_write_at(slot, off, (const char*)buf, count);
    if (n < 0) return err(ERR_EBADF);
    user_fd_set_offset((int)fd, off + (uint32_t)n);
    return (uint32_t)n;
}

static uint32_t do_read(uint32_t fd, uint32_t buf, uint32_t count) {
    if (count == 0) return 0;
    if (!user_ptr_ok(buf, count)) return err(ERR_EFAULT);

    if (fd == 0) {
        if (!user_stdio_is_open(0)) return err(ERR_EBADF);
        /* keyboard_getchar spins with interrupts enabled internally, which
         * is required here: int 0x80 also runs with IF=0. */
        char c = keyboard_getchar();
        *(char*)buf = c;
        vga_putchar(c);
        serial_putchar(c);
        return 1;
    }

    if (user_fd_is_dir((int)fd)) return err(ERR_EISDIR);
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return err(ERR_EBADF);
    uint32_t off = user_fd_offset((int)fd);
    int n = fs_read_at(slot, off, (char*)buf, count);
    if (n < 0) return err(ERR_EBADF);
    user_fd_set_offset((int)fd, off + (uint32_t)n);
    return (uint32_t)n;
}

/* Copy a user-space NUL-terminated string into a kernel buffer. */
static bool_t copy_user_string(uint32_t ptr, char* dst, uint32_t max) {
    if (max == 0 || !user_ptr_ok(ptr, 1)) return false;
    for (uint32_t i = 0; i < max; i++) {
        if (!user_ptr_ok(ptr + i, 1)) return false;
        char c = ((const char*)ptr)[i];
        dst[i] = c;
        if (c == '\0') return true;
    }
    dst[max - 1] = '\0';   /* silently truncate over-long paths */
    return true;
}

/* The RAM FS is flat: "/" is the only directory, "/name" is a regular
 * file. Map a user path onto that model.
 * Returns 0 with the file name in "name", 1 for the root directory,
 * or a negative errno. */
static int resolve_flat(const char* path, char* name, uint32_t max) {
    const char* p = path;
    for (;;) {
        while (*p == '/') p++;
        if (*p == '\0') { name[0] = '\0'; return 1; }
        if (p[0] == '.' && p[1] == '/') { p += 2; continue; }
        if (p[0] == '.' && (p[1] == '\0' || (p[1] == '.' && p[2] == '\0'))) {
            name[0] = '\0';      /* "." and ".." both resolve to "/" */
            return 1;
        }
        break;
    }
    uint32_t i = 0;
    for (; p[i]; i++) {
        if (p[i] == '/') return -ERR_ENOTDIR;   /* no subdirectories yet */
        if (i + 1 >= max) return -ERR_ENAMETOOLONG;
        name[i] = p[i];
    }
    name[i] = '\0';
    return 0;
}

static uint32_t do_open(uint32_t path, uint32_t flags, uint32_t mode) {
    (void)mode;
    char buf[128];
    if (!copy_user_string(path, buf, sizeof(buf))) return err(ERR_EFAULT);

    char name[FS_MAX_NAME];
    int kind = resolve_flat(buf, name, sizeof(name));
    if (kind < 0) return err((uint32_t)(-kind));

    if (kind == 1) {
        if (flags & (O_CREAT | O_TRUNC)) return err(ERR_EISDIR);
        int dfd = user_fd_alloc_dir();
        if (dfd < 0) return err(ERR_EMFILE);
        return (uint32_t)dfd;
    }

    int slot = fs_open(name);
    if (slot < 0) {
        if (flags & O_DIRECTORY) return err(ERR_ENOTDIR);
        if (!(flags & O_CREAT)) return err(ERR_ENOENT);
        slot = fs_create(name);
        if (slot < 0) return err(ERR_ENOSPC);
    } else if (flags & O_DIRECTORY) {
        return err(ERR_ENOTDIR);   /* a regular file is not a directory */
    }
    if (flags & O_TRUNC) fs_fd_truncate(slot);

    int fd = user_fd_alloc(slot);
    if (fd < 0) return err(ERR_EMFILE);
    return (uint32_t)fd;
}

static uint32_t do_close(uint32_t fd) {
    if (fd <= 2) {
        if (!user_stdio_is_open((int)fd)) return err(ERR_EBADF);
        user_stdio_close((int)fd);
        return 0;
    }
    if (user_fd_slot((int)fd) < 0 && !user_fd_is_dir((int)fd)) return err(ERR_EBADF);
    user_fd_close((int)fd);
    return 0;
}

/* ---- stat / dirent support for the flat RAM FS ---- */

static void put_u16(char* b, uint32_t off, uint16_t v) { memcpy(b + off, &v, 2); }
static void put_u32(char* b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }
static void put_u64(char* b, uint32_t off, uint64_t v) { memcpy(b + off, &v, 8); }

/* Fill the i386 struct stat64 layout (96 bytes). */
static void fill_stat64(char* st, uint32_t ino, uint32_t mode,
                        uint32_t nlink, uint32_t size) {
    memset(st, 0, STAT64_SIZE);
    put_u64(st, 0, 1);                 /* st_dev */
    put_u32(st, 12, ino);              /* legacy 32-bit inode */
    put_u32(st, 16, mode);
    put_u32(st, 20, nlink);
    put_u32(st, 24, 0);                /* st_uid */
    put_u32(st, 28, 0);                /* st_gid */
    put_u64(st, 32, 0);                /* st_rdev */
    put_u64(st, 44, (uint64_t)size);   /* st_size */
    put_u32(st, 52, 4096);             /* st_blksize */
    put_u64(st, 56, ((uint64_t)size + 511) / 512);
    put_u64(st, 88, (uint64_t)ino);    /* st_ino */
}

/* Fill the legacy i386 struct stat layout (64 bytes). */
static void fill_stat_old(char* st, uint32_t ino, uint32_t mode,
                          uint32_t nlink, uint32_t size) {
    memset(st, 0, STAT_OLD_SIZE);
    put_u16(st, 0, 1);                 /* st_dev */
    put_u32(st, 4, ino);
    put_u16(st, 8, (uint16_t)mode);
    put_u16(st, 10, (uint16_t)nlink);
    put_u16(st, 12, 0);                /* st_uid */
    put_u16(st, 14, 0);                /* st_gid */
    put_u32(st, 20, size);
    put_u32(st, 24, 4096);
    put_u32(st, 28, (size + 511) / 512);
}

static uint32_t do_stat(uint32_t path_ptr, uint32_t st_ptr, bool_t is64) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    if (!user_ptr_ok(st_ptr, is64 ? STAT64_SIZE : STAT_OLD_SIZE))
        return err(ERR_EFAULT);

    char name[FS_MAX_NAME];
    int kind = resolve_flat(buf, name, sizeof(name));
    if (kind < 0) return err((uint32_t)(-kind));

    char st[STAT64_SIZE];
    uint32_t ino, mode, nlink, size;
    if (kind == 1) {
        ino = 1; mode = S_IFDIR | 0755; nlink = 2; size = 0;
    } else {
        int slot = fs_open(name);
        if (slot < 0) return err(ERR_ENOENT);
        ino = (uint32_t)(slot + 1);
        mode = S_IFREG | 0644;
        nlink = 1;
        size = fs_fd_size(slot);
    }

    if (is64) fill_stat64(st, ino, mode, nlink, size);
    else      fill_stat_old(st, ino, mode, nlink, size);
    memcpy((void*)st_ptr, st, is64 ? STAT64_SIZE : STAT_OLD_SIZE);
    return 0;
}

static uint32_t do_fstat(uint32_t fd, uint32_t st_ptr, bool_t is64) {
    if (!user_ptr_ok(st_ptr, is64 ? STAT64_SIZE : STAT_OLD_SIZE))
        return err(ERR_EFAULT);

    char st[STAT64_SIZE];
    uint32_t ino, mode, nlink, size;
    if (fd <= 2) {
        if (!user_stdio_is_open((int)fd)) return err(ERR_EBADF);
        ino = fd + 1; mode = S_IFCHR | 0666; nlink = 1; size = 0;
    } else if (user_fd_is_dir((int)fd)) {
        ino = 1; mode = S_IFDIR | 0755; nlink = 2; size = 0;
    } else {
        int slot = user_fd_slot((int)fd);
        if (slot < 0) return err(ERR_EBADF);
        ino = (uint32_t)(slot + 1);
        mode = S_IFREG | 0644;
        nlink = 1;
        size = fs_fd_size(slot);
    }

    if (is64) fill_stat64(st, ino, mode, nlink, size);
    else      fill_stat_old(st, ino, mode, nlink, size);
    memcpy((void*)st_ptr, st, is64 ? STAT64_SIZE : STAT_OLD_SIZE);
    return 0;
}

/* Fill the i386 struct statx (256 bytes) with basic stats. */
static void fill_statx(char* st, uint32_t ino, uint32_t mode,
                       uint32_t nlink, uint32_t size) {
    memset(st, 0, STATX_SIZE);
    put_u32(st, 0, STATX_BASIC_STATS);   /* stx_mask */
    put_u32(st, 4, 4096);                /* stx_blksize */
    put_u32(st, 16, nlink);
    put_u32(st, 20, 0);                  /* stx_uid */
    put_u32(st, 24, 0);                  /* stx_gid */
    put_u16(st, 28, (uint16_t)mode);
    put_u64(st, 32, (uint64_t)ino);
    put_u64(st, 40, (uint64_t)size);
    put_u64(st, 48, ((uint64_t)size + 511) / 512);
}

/* statx(383): modern musl tries this before the stat64 family. An empty
 * path with AT_EMPTY_PATH means "stat the dirfd itself". */
static uint32_t do_statx(uint32_t dirfd, uint32_t path_ptr, uint32_t buf) {
    if (!user_ptr_ok(buf, STATX_SIZE)) return err(ERR_EFAULT);

    char path[128];
    if (!copy_user_string(path_ptr, path, sizeof(path))) return err(ERR_EFAULT);

    uint32_t ino, mode, nlink, size;
    if (path[0] == '\0') {
        int fd = (int32_t)dirfd;
        if (fd < 0) return err(ERR_ENOENT);
        if (fd <= 2) {
            if (!user_stdio_is_open(fd)) return err(ERR_EBADF);
            ino = (uint32_t)fd + 1; mode = S_IFCHR | 0666; nlink = 1; size = 0;
        } else if (user_fd_is_dir(fd)) {
            ino = 1; mode = S_IFDIR | 0755; nlink = 2; size = 0;
        } else {
            int slot = user_fd_slot(fd);
            if (slot < 0) return err(ERR_EBADF);
            ino = (uint32_t)(slot + 1);
            mode = S_IFREG | 0644;
            nlink = 1;
            size = fs_fd_size(slot);
        }
    } else {
        char name[FS_MAX_NAME];
        int kind = resolve_flat(path, name, sizeof(name));
        if (kind < 0) return err((uint32_t)(-kind));
        if (kind == 1) {
            ino = 1; mode = S_IFDIR | 0755; nlink = 2; size = 0;
        } else {
            int slot = fs_open(name);
            if (slot < 0) return err(ERR_ENOENT);
            ino = (uint32_t)(slot + 1);
            mode = S_IFREG | 0644;
            nlink = 1;
            size = fs_fd_size(slot);
        }
    }

    char st[STATX_SIZE];
    fill_statx(st, ino, mode, nlink, size);
    memcpy((void*)buf, st, STATX_SIZE);
    return 0;
}

/* Write a kernel buffer (sendfile path cannot use user_ptr_ok checks). */
static int kbuf_write(uint32_t fd, const char* data, uint32_t count) {
    if (fd == 1 || fd == 2) {
        if (!user_stdio_is_open((int)fd)) return -ERR_EBADF;
        for (uint32_t i = 0; i < count; i++) {
            vga_putchar(data[i]);
            serial_putchar(data[i]);
        }
        return (int)count;
    }
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return -ERR_EBADF;
    uint32_t off = user_fd_offset((int)fd);
    int n = fs_write_at(slot, off, data, count);
    if (n < 0) return -ERR_EBADF;
    user_fd_set_offset((int)fd, off + (uint32_t)n);
    return n;
}

/* sendfile(187)/sendfile64(239): used by BusyBox's fd-copy path. */
static uint32_t do_sendfile(uint32_t out_fd, uint32_t in_fd,
                            uint32_t off_ptr, uint32_t count) {
    int slot = user_fd_slot((int)in_fd);
    if (slot < 0) return err(ERR_EBADF);

    uint32_t off;
    if (off_ptr) {
        if (!user_ptr_ok(off_ptr, 4)) return err(ERR_EFAULT);
        memcpy(&off, (const void*)off_ptr, 4);
    } else {
        off = user_fd_offset((int)in_fd);
    }

    uint32_t total = 0;
    char buf[4096];
    while (total < count) {
        uint32_t chunk = count - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        int n = fs_read_at(slot, off, buf, chunk);
        if (n <= 0) break;
        int w = kbuf_write(out_fd, buf, (uint32_t)n);
        if (w < 0) {
            if (total == 0) return err((uint32_t)(-w));
            break;
        }
        total += (uint32_t)w;
        off += (uint32_t)w;
        if ((uint32_t)w < (uint32_t)n) break;
    }

    if (off_ptr) memcpy((void*)off_ptr, &off, 4);
    else user_fd_set_offset((int)in_fd, off);
    return total;
}

/* One entry of the root directory. idx 0 = ".", 1 = "..", then files. */
static bool_t dir_entry_at(uint32_t idx, char* name, uint32_t* ino,
                           uint8_t* type) {
    if (idx == 0) { strncpy(name, ".", FS_MAX_NAME); *ino = 1; *type = 4; return true; }
    if (idx == 1) { strncpy(name, "..", FS_MAX_NAME); *ino = 1; *type = 4; return true; }
    const char* n = fs_name((int)(idx - 2));
    if (!n) return false;
    strncpy(name, n, FS_MAX_NAME - 1);
    name[FS_MAX_NAME - 1] = '\0';
    *ino = idx + 1;
    *type = 8;          /* DT_REG */
    return true;
}

/* getdents(141) and getdents64(220): both walk the same virtual root. */
static uint32_t do_getdents(uint32_t fd, uint32_t buf, uint32_t count,
                            bool_t is64) {
    if (!user_fd_is_dir((int)fd)) {
        return err(user_fd_slot((int)fd) >= 0 ? ERR_ENOTDIR : ERR_EBADF);
    }
    if (count < 8) return err(ERR_EINVAL);

    uint32_t idx = user_fd_offset((int)fd);
    uint32_t total = 0;
    char name[FS_MAX_NAME];
    uint32_t ino;
    uint8_t type;

    while (dir_entry_at(idx, name, &ino, &type)) {
        uint32_t namelen = strlen(name);
        uint32_t reclen = is64
            ? ((19 + namelen + 1 + 7) & ~7u)
            : ((10 + namelen + 1 + 3) & ~3u);
        if (total + reclen > count) {
            if (total == 0) return err(ERR_EINVAL);
            break;
        }
        if (!user_ptr_ok(buf + total, reclen)) return err(ERR_EFAULT);

        char rec[64];
        memset(rec, 0, sizeof(rec));
        if (is64) {
            put_u64(rec, 0, (uint64_t)ino);
            put_u64(rec, 8, (uint64_t)(idx + 1));   /* d_off: next index */
            put_u16(rec, 16, (uint16_t)reclen);
            rec[18] = (char)type;
            rec[19] = '\0';
            strncpy(rec + 19, name, namelen);
        } else {
            put_u32(rec, 0, ino);
            put_u32(rec, 4, idx + 1);
            put_u16(rec, 8, (uint16_t)reclen);
            rec[10] = '\0';
            strncpy(rec + 10, name, namelen);
            rec[reclen - 1] = (char)type;   /* kernel hides d_type here */
        }
        memcpy((void*)(buf + total), rec, reclen);
        idx++;
        total += reclen;
    }
    user_fd_set_offset((int)fd, idx);
    return total;
}

static uint32_t do_lseek(uint32_t fd, uint32_t offset, uint32_t whence) {
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return err(ERR_EBADF);

    int32_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int32_t)user_fd_offset((int)fd); break;
        case 2: base = (int32_t)fs_fd_size(slot); break;
        default: return err(ERR_EINVAL);
    }

    int64_t pos = (int64_t)base + (int32_t)offset;
    if (pos < 0 || pos > FS_MAX_SIZE) return err(ERR_EINVAL);
    user_fd_set_offset((int)fd, (uint32_t)pos);
    return (uint32_t)pos;
}

/* lseek_ll(140): musl uses llseek on 32-bit i386. Returns 0 and stores
 * the 64-bit result in *result (user pointer). */
static uint32_t do_llseek(uint32_t fd, uint32_t off_lo, uint32_t off_hi,
                          uint32_t result, uint32_t whence) {
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return err(ERR_EBADF);
    if (!user_ptr_ok(result, 8)) return err(ERR_EFAULT);

    int64_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)user_fd_offset((int)fd); break;
        case 2: base = (int64_t)fs_fd_size(slot); break;
        default: return err(ERR_EINVAL);
    }
    int64_t pos = base + (int64_t)(((uint64_t)off_hi << 32) | off_lo);
    if (pos < 0 || pos > FS_MAX_SIZE) return err(ERR_EINVAL);
    user_fd_set_offset((int)fd, (uint32_t)pos);
    uint64_t out = (uint64_t)pos;
    memcpy((void*)result, &out, 8);
    return 0;
}

static uint32_t do_access(uint32_t path_ptr, uint32_t mode) {
    (void)mode;
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    char name[FS_MAX_NAME];
    int kind = resolve_flat(buf, name, sizeof(name));
    if (kind < 0) return err((uint32_t)(-kind));
    if (kind == 1) return 0;
    return fs_open(name) >= 0 ? 0 : err(ERR_ENOENT);
}

static uint32_t do_chdir(uint32_t path_ptr) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    char name[FS_MAX_NAME];
    int kind = resolve_flat(buf, name, sizeof(name));
    if (kind < 0) return err((uint32_t)(-kind));
    return kind == 1 ? 0 : err(ERR_ENOTDIR);
}

static uint32_t do_unlink(uint32_t path_ptr) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    char name[FS_MAX_NAME];
    int kind = resolve_flat(buf, name, sizeof(name));
    if (kind < 0) return err((uint32_t)(-kind));
    if (kind == 1) return err(ERR_EPERM);   /* cannot unlink "/" */
    return fs_delete(name) == 0 ? 0 : err(ERR_ENOENT);
}

static uint32_t do_rename(uint32_t old_ptr, uint32_t new_ptr) {
    char obuf[128], nbuf[128];
    if (!copy_user_string(old_ptr, obuf, sizeof(obuf))) return err(ERR_EFAULT);
    if (!copy_user_string(new_ptr, nbuf, sizeof(nbuf))) return err(ERR_EFAULT);

    char oname[FS_MAX_NAME], nname[FS_MAX_NAME];
    int okind = resolve_flat(obuf, oname, sizeof(oname));
    int nkind = resolve_flat(nbuf, nname, sizeof(nname));
    if (okind < 0) return err((uint32_t)(-okind));
    if (nkind < 0) return err((uint32_t)(-nkind));
    if (okind == 1 || nkind == 1) return err(ERR_EPERM);

    int slot = fs_open(oname);
    if (slot < 0) return err(ERR_ENOENT);
    int target = fs_open(nname);
    if (target == slot) return 0;            /* same file already */
    if (target >= 0) fs_delete(nname);       /* rename replaces the target */
    return fs_rename(slot, nname) == 0 ? 0 : err(ERR_EINVAL);
}

/* utimes/futimesat/utimensat: the RAM FS stores no timestamps, but the
 * path must exist so tools behave like they do on Linux. */
static uint32_t do_utime_path(uint32_t path_ptr, int fd) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);

    if (buf[0] == '\0') {
        if (fd < 0) return err(ERR_ENOENT);
        if (fd <= 2) return user_stdio_is_open(fd) ? 0 : err(ERR_EBADF);
        if (user_fd_is_dir(fd) || user_fd_slot(fd) >= 0) return 0;
        return err(ERR_EBADF);
    }

    char name[FS_MAX_NAME];
    int kind = resolve_flat(buf, name, sizeof(name));
    if (kind < 0) return err((uint32_t)(-kind));
    if (kind == 1) return 0;
    return fs_open(name) >= 0 ? 0 : err(ERR_ENOENT);
}

static uint32_t do_getcwd(uint32_t buf, uint32_t size) {
    if (size < 2) return err(ERR_EINVAL);
    if (!user_ptr_ok(buf, 2)) return err(ERR_EFAULT);
    ((char*)buf)[0] = '/';
    ((char*)buf)[1] = '\0';
    return 1;
}

static uint32_t do_dup(uint32_t fd) {
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return err(ERR_EBADF);
    int nfd = user_fd_alloc(slot);
    if (nfd < 0) return err(ERR_EMFILE);
    user_fd_set_offset(nfd, user_fd_offset((int)fd));
    return (uint32_t)nfd;
}

/* musl flushes buffered stdio with writev, so a working writev is required
 * for BusyBox applets that print through stdio (ls, ps, ...). */
typedef struct {
    uint32_t base;
    uint32_t len;
} iovec32_t;

static uint32_t do_writev(uint32_t fd, uint32_t iov_ptr, uint32_t count) {
    if (count > 1024) return err(ERR_EINVAL);
    if (!user_ptr_ok(iov_ptr, count * 8)) return err(ERR_EFAULT);
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        iovec32_t v;
        memcpy(&v, (const void*)(iov_ptr + i * 8), sizeof(v));
        uint32_t n = do_write(fd, v.base, v.len);
        if ((int32_t)n < 0) return total ? total : n;
        total += n;
    }
    return total;
}

static uint32_t do_readv(uint32_t fd, uint32_t iov_ptr, uint32_t count) {
    if (count > 1024) return err(ERR_EINVAL);
    if (!user_ptr_ok(iov_ptr, count * 8)) return err(ERR_EFAULT);
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        iovec32_t v;
        memcpy(&v, (const void*)(iov_ptr + i * 8), sizeof(v));
        uint32_t n = do_read(fd, v.base, v.len);
        if ((int32_t)n < 0) return total ? total : n;
        total += n;
        if (n < v.len) break;   /* short read ends the vector */
    }
    return total;
}

/* No RTC-backed wall clock yet: report the epoch (1970) deterministically.
 * clk_id is ignored; all clocks read the same counter. */
static uint32_t do_gettimeofday(uint32_t tv, uint32_t tz) {
    if (tv) {
        if (!user_ptr_ok(tv, 8)) return err(ERR_EFAULT);
        memset((void*)tv, 0, 8);
    }
    if (tz) {
        if (!user_ptr_ok(tz, 8)) return err(ERR_EFAULT);
        memset((void*)tz, 0, 8);
    }
    return 0;
}

static uint32_t do_clock_gettime(uint32_t ts, uint32_t struct_size) {
    if (!ts) return err(ERR_EFAULT);
    if (!user_ptr_ok(ts, struct_size)) return err(ERR_EFAULT);
    memset((void*)ts, 0, struct_size);
    return 0;
}

static uint32_t do_fcntl(uint32_t fd, uint32_t cmd) {
    if (fd <= 2) {
        if (!user_stdio_is_open((int)fd)) return err(ERR_EBADF);
    } else if (!user_fd_is_dir((int)fd) && user_fd_slot((int)fd) < 0) {
        return err(ERR_EBADF);
    }
    switch (cmd) {
        case 1: return 0;    /* F_GETFD */
        case 2: return 0;    /* F_SETFD */
        case 3: return 0;    /* F_GETFL: O_RDONLY */
        case 4: return 0;    /* F_SETFL */
        default: return err(ERR_EINVAL);
    }
}

static uint32_t do_uname(uint32_t ptr) {
    if (!user_ptr_ok(ptr, UTS_SIZE)) return err(ERR_EFAULT);

    char buf[UTS_SIZE];
    memset(buf, 0, sizeof(buf));
    strncpy(buf + UTS_FIELD * 0, "MuOS",     UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 1, "muos",     UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 2, "1.2.0",    UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 3, "MuOS v1.2", UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 4, "i386",     UTS_FIELD - 1);
    memcpy((void*)ptr, buf, UTS_SIZE);
    return 0;
}

/* Anonymous mappings only: the sandbox is pre-mapped, so this is a bump
 * allocator. File-backed mmap is rejected until a real VFS exists. */
static uint32_t do_mmap(uint32_t addr, uint32_t len, uint32_t prot,
                        uint32_t flags, int fd, uint32_t offset) {
    (void)addr; (void)prot; (void)offset;
    if (!(flags & MAP_ANONYMOUS) || (flags & MAP_FIXED)) return err(ERR_EINVAL);
    if (fd != -1) return err(ERR_ENODEV);
    uint32_t p = user_mmap_anon(len);
    if (!p) return err(ERR_ENOMEM);
    return p;
}

static uint32_t do_old_mmap(uint32_t args_ptr) {
    if (!user_ptr_ok(args_ptr, 6 * 4)) return err(ERR_EFAULT);
    uint32_t a[6];
    memcpy(a, (const void*)args_ptr, sizeof(a));
    return do_mmap(a[0], a[1], a[2], a[3], (int)a[4], a[5]);
}

/* Linux i386 user_desc (16 bytes): entry_number, base, limit, flags.
 * Flags bits: 0 seg_32bit, 1-2 contents, 3 read_exec_only,
 * 4 limit_in_pages, 5 seg_not_present, 6 useable. */
typedef struct {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
} __attribute__((packed)) user_desc_t;

static uint32_t do_set_thread_area(uint32_t ptr) {
    if (!user_ptr_ok(ptr, sizeof(user_desc_t))) return err(ERR_EFAULT);
    user_desc_t d;
    memcpy(&d, (const void*)ptr, sizeof(d));

    if (d.entry_number != 0xFFFFFFFFu && d.entry_number != 6) {
        return err(ERR_EINVAL);
    }

    gdt_set_user_tls(d.base_addr, d.limit, d.flags);

    /* Tell libc which selector to load: slot 6 -> 0x33 with RPL3. */
    d.entry_number = 6;
    memcpy((void*)ptr, &d, sizeof(d));
    return 0;
}

static uint32_t do_get_thread_area(uint32_t ptr) {
    if (!user_ptr_ok(ptr, sizeof(user_desc_t))) return err(ERR_EFAULT);
    user_desc_t d;
    uint32_t base = 0, limit = 0, flags = 0;
    gdt_get_user_tls(&base, &limit, &flags);
    d.entry_number = 6;
    d.base_addr = base;
    d.limit = limit;
    d.flags = flags;
    memcpy((void*)ptr, &d, sizeof(d));
    return 0;
}

static void report_unknown(uint32_t nr) {
    static uint32_t last_reported = 0xFFFFFFFFu;
    if (nr != last_reported) {
        last_reported = nr;
        serial_write("[syscall] unimplemented: ");
        { char b[12]; u32_to_dec(b, nr); serial_write(b); }
        serial_write("\n");
    }
}

void syscall_handler(registers_t* regs) {
    uint32_t ret;

    switch (regs->eax) {
        case SYS_exit:
            user_exit((int)regs->ebx);   /* never returns */
            ret = 0;
            break;
        case SYS_read:
            ret = do_read(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_write:
            ret = do_write(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_open:
            ret = do_open(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_close:
            ret = do_close(regs->ebx);
            break;
        case SYS_unlink:
            ret = do_unlink(regs->ebx);
            break;
        case SYS_rename:
            ret = do_rename(regs->ebx, regs->ecx);
            break;
        case SYS_utimes:
            ret = do_utime_path(regs->ebx, -1);
            break;
        case SYS_futimesat:
        case SYS_utimensat:
            ret = do_utime_path(regs->ecx, (int)regs->ebx);
            break;
        case SYS_lseek:
            ret = do_lseek(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_llseek:
            ret = do_llseek(regs->ebx, regs->ecx, regs->edx,
                            regs->esi, regs->edi);
            break;
        case SYS_chdir:
            ret = do_chdir(regs->ebx);
            break;
        case SYS_access:
            ret = do_access(regs->ebx, regs->ecx);
            break;
        case SYS_getcwd:
            ret = do_getcwd(regs->ebx, regs->ecx);
            break;
        case SYS_getdents:
            ret = do_getdents(regs->ebx, regs->ecx, regs->edx, false);
            break;
        case SYS_getdents64:
            ret = do_getdents(regs->ebx, regs->ecx, regs->edx, true);
            break;
        case SYS_stat:
        case SYS_lstat:
            ret = do_stat(regs->ebx, regs->ecx, false);
            break;
        case SYS_stat64:
        case SYS_lstat64:
            ret = do_stat(regs->ebx, regs->ecx, true);
            break;
        case SYS_fstat:
            ret = do_fstat(regs->ebx, regs->ecx, false);
            break;
        case SYS_fstat64:
            ret = do_fstat(regs->ebx, regs->ecx, true);
            break;
        case SYS_fcntl:
        case SYS_fcntl64:
            ret = do_fcntl(regs->ebx, regs->ecx);
            break;
        case SYS_rt_sigaction:
        case SYS_rt_sigprocmask:
        case SYS_umask:
            ret = 0;   /* no signals/umask state: report success */
            break;
        case SYS_getgroups:
        case SYS_getgroups32:
            ret = 0;   /* root with no supplementary groups */
            break;
        case SYS_readv:
            ret = do_readv(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_writev:
            ret = do_writev(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_time:
            ret = 0;
            break;
        case SYS_gettimeofday:
            ret = do_gettimeofday(regs->ebx, regs->ecx);
            break;
        case SYS_clock_gettime:
            ret = do_clock_gettime(regs->ecx, 8);
            break;
        case SYS_clock_gettime64:
            ret = do_clock_gettime(regs->ecx, 16);
            break;
        case SYS_sendfile:
        case SYS_sendfile64:
            ret = do_sendfile(regs->ebx, regs->ecx, regs->edx, regs->esi);
            break;
        case SYS_statx:
            ret = do_statx(regs->ebx, regs->ecx, regs->edi);
            break;
        case SYS_dup:
            ret = do_dup(regs->ebx);
            break;
        case SYS_getpid:
        case SYS_getppid:
            ret = regs->eax == SYS_getppid ? 0 : 1;
            break;
        case SYS_getuid:
        case SYS_geteuid:
        case SYS_getgid:
        case SYS_getegid:
        case SYS_getuid32:
        case SYS_geteuid32:
        case SYS_getgid32:
        case SYS_getegid32:
            ret = 0;
            break;
        case SYS_ioctl:
            ret = err(ERR_ENOTTY);   /* not a real tty yet */
            break;
        case SYS_dup2:
            ret = (regs->ebx == regs->ecx) ? regs->ecx : err(ERR_EBADF);
            break;
        case SYS_brk:
            ret = (regs->ebx == 0 || !user_set_brk(regs->ebx))
                  ? user_get_brk() : regs->ebx;
            break;
        case SYS_old_mmap:
            ret = do_old_mmap(regs->ebx);
            break;
        case SYS_mmap2:
            /* pgoffset (ebp) must be 0 for anonymous mappings */
            ret = do_mmap(regs->ebx, regs->ecx, regs->edx, regs->esi, (int)regs->edi, regs->ebp);
            break;
        case SYS_munmap:
        case SYS_mprotect:
            ret = 0;   /* flat, always-mapped sandbox: nothing to do */
            break;
        case SYS_uname:
            ret = do_uname(regs->ebx);
            break;
        case SYS_set_thread_area:
            ret = do_set_thread_area(regs->ebx);
            break;
        case SYS_get_thread_area:
            ret = do_get_thread_area(regs->ebx);
            break;
        case SYS_modify_ldt:
            /* set_thread_area is implemented, so musl/glibc should not
             * need the LDT fallback. Do not fake success here. */
            ret = err(ERR_ENOSYS);
            break;
        case SYS_exit_group:
            user_exit((int)regs->ebx);   /* never returns */
            ret = 0;
            break;
        case SYS_set_tid_address:
            ret = 1;
            break;
        default:
            report_unknown(regs->eax);
            ret = err(ERR_ENOSYS);
            break;
    }

    regs->eax = ret;   /* syscall_common pops this into eax before iret */
}
