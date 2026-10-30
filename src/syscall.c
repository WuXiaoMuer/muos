#include "syscall.h"
#include "user.h"
#include "fs.h"
#include "gdt.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "string.h"
#include "mm.h"
#include "pit.h"
#include "task.h"
#include "rtc.h"
#include "proc.h"
#include "tss.h"
#include "elf.h"
#include "module.h"
#include "pipe.h"

/* Linux i386 syscall numbers (the ABI MuOS targets). This is the starter
 * set needed by freestanding user programs; anything else is rejected
 * with -ENOSYS and logged to serial so real binaries can drive the TODO
 * list (see ROADMAP stage 4). */
#define SYS_exit       1
#define SYS_fork       2
#define SYS_read       3
#define SYS_write      4
#define SYS_open       5
#define SYS_close      6
#define SYS_waitpid    7
#define SYS_unlink    10
#define SYS_execve    11
#define SYS_chdir     12
#define SYS_time      13
#define SYS_lseek     19
#define SYS_getpid    20
#define SYS_getuid    24
#define SYS_access    33
#define SYS_kill      37
#define SYS_mkdir     39
#define SYS_rmdir     40
#define SYS_rename    38
#define SYS_setpgid   57
#define SYS_getpgrp   65
#define SYS_setsid    66
#define SYS_getpgid  132
#define SYS_getsid   147
#define SYS_vfork    190
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
#define SYS_pipe      42
#define SYS_statfs    99
#define SYS_fstatfs  100
#define SYS_sysinfo  116
#define SYS_getgroups 80
#define SYS_gettimeofday 78
#define SYS_wait4    114
#define SYS_clone    120
#define SYS_sigaltstack 186
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
#define SYS_statfs64        268
#define SYS_fstatfs64       269
#define SYS_set_thread_area 243
#define SYS_get_thread_area 244
#define SYS_exit_group      252
#define SYS_set_tid_address 258
/* 32-bit time64 ABI used by modern musl for clock_gettime. */
#define SYS_clock_gettime64 403
#define SYS_pipe2           331
#define SYS_sendfile64      239
#define SYS_statx           383

/* *at family (Linux i386 numbers) and their resolver flags. */
#define SYS_openat          295
#define SYS_mkdirat         296
#define SYS_fstatat64       300
#define SYS_unlinkat        301
#define SYS_renameat        302
#define SYS_readlinkat      305
#define SYS_fchmodat        306
#define SYS_faccessat       307
#define AT_FDCWD           (-100)
#define AT_EMPTY_PATH       0x1000
#define AT_REMOVEDIR        0x200

/* Metadata, truncation, sync and rlimit calls. */
#define SYS_chmod           15
#define SYS_sync            36
#define SYS_setrlimit       75
#define SYS_getrlimit       76
#define SYS_truncate        92
#define SYS_ftruncate       93
#define SYS_fchmod          94
#define SYS_fsync          118
#define SYS_fdatasync      144
#define SYS_ugetrlimit     191
#define SYS_truncate64     193
#define SYS_ftruncate64    194
#define SYS_prlimit64      340

#define RLIMIT_CPU      0
#define RLIMIT_FSIZE    1
#define RLIMIT_DATA     2
#define RLIMIT_STACK    3
#define RLIMIT_CORE     4
#define RLIMIT_RSS      5
#define RLIMIT_NPROC    6
#define RLIMIT_NOFILE   7
#define RLIMIT_MEMLOCK  8
#define RLIMIT_AS       9
#define RLIM_INFINITY   0xFFFFFFFFu
#define RLIMIT64_SIZE   16

#define ERR_EPERM      1
#define ERR_ENOENT     2
#define ERR_ESRCH      3
#define ERR_ENOEXEC    8
#define ERR_EBADF      9
#define ERR_ECHILD    10
#define ERR_EAGAIN    11
#define ERR_EACCES    13
#define ERR_EEXIST    17
#define ERR_ENOMEM    12
#define ERR_EFAULT    14
#define ERR_ENODEV    19
#define ERR_ENOTDIR   20
#define ERR_EISDIR    21
#define ERR_EINVAL    22
#define ERR_EMFILE    24
#define ERR_ENOSPC    28
#define ERR_EFBIG     27
#define ERR_ENOTTY    25
#define ERR_ERANGE    34
#define ERR_ENAMETOOLONG 36
#define ERR_ENOSYS    38
#define ERR_ENOTEMPTY 39

/* open(2) flags and mmap(2) constants (Linux i386 values) */
#define O_WRONLY    0x01
#define O_RDWR      0x02
#define O_ACCMODE   0x03
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_DIRECTORY 0x10000

/* i386 struct stat64 (96 bytes) and legacy struct stat (64 bytes). */
#define STAT64_SIZE 96
#define STAT_OLD_SIZE 64
#define STATX_SIZE 256
#define STATX_BASIC_STATS 0x7ffu
#define STATFS_OLD_SIZE 64
#define STATFS64_SIZE 84
#define SYSINFO_SIZE 64
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFCHR 0020000u
#define S_IFIFO 0010000u
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define UTS_FIELD 65          /* sizeof each utsname field */
#define UTS_SIZE  390         /* 6 fields */

static uint32_t err(uint32_t code) {
    return (uint32_t)(-(int32_t)code);
}

static uint32_t read_keyboard_char(uint32_t buf) {
    /* keyboard_getchar spins with interrupts enabled internally, which
     * is required here: int 0x80 also runs with IF=0. */
    char c = keyboard_getchar();
    *(char*)buf = c;
    vga_putchar(c);
    serial_putchar(c);
    return 1;
}

static uint32_t do_write(uint32_t fd, uint32_t buf, uint32_t count) {
    if (count == 0) return 0;
    if (!user_ptr_ok(buf, count)) return err(ERR_EFAULT);

    int kind = user_fd_kind((int)fd);
    if (kind < 0) return err(ERR_EBADF);
    switch (kind) {
        case USER_FD_FILE: {
            int slot = user_fd_slot((int)fd);
            if (slot < 0) return err(ERR_EBADF);
            uint32_t off = user_fd_offset((int)fd);
            int n = fs_write_at(slot, off, (const char*)buf, count);
            if (n < 0) return err(ERR_EBADF);
            user_fd_set_offset((int)fd, off + (uint32_t)n);
            return (uint32_t)n;
        }
        case USER_FD_DEV_NULL:
        case USER_FD_DEV_ZERO:
            return count;              /* writes are discarded */
        case USER_FD_STDOUT:
        case USER_FD_STDERR:
        case USER_FD_DEV_CONSOLE:
            for (uint32_t i = 0; i < count; i++) {
                char c = ((const char*)buf)[i];
                vga_putchar(c);
                serial_putchar(c);
            }
            return count;
        case USER_FD_VFILE:
            return err(ERR_EACCES);
        case USER_FD_PIPE_WRITE: {
            int id = user_fd_pipe_id((int)fd);
            if (id < 0) return err(ERR_EBADF);
            int n = pipe_write(id, (const char*)buf, count);
            return n < 0 ? err(ERR_EBADF) : (uint32_t)n;
        }
        case USER_FD_DIR:
            return err(ERR_EISDIR);
        default:
            return err(ERR_EBADF);     /* e.g. writing to stdin */
    }
}

static uint32_t do_read(uint32_t fd, uint32_t buf, uint32_t count) {
    if (count == 0) return 0;
    if (!user_ptr_ok(buf, count)) return err(ERR_EFAULT);

    int kind = user_fd_kind((int)fd);
    if (kind < 0) return err(ERR_EBADF);
    switch (kind) {
        case USER_FD_FILE: {
            int slot = user_fd_slot((int)fd);
            if (slot < 0) return err(ERR_EBADF);
            uint32_t off = user_fd_offset((int)fd);
            int n = fs_read_at(slot, off, (char*)buf, count);
            if (n < 0) return err(ERR_EBADF);
            user_fd_set_offset((int)fd, off + (uint32_t)n);
            return (uint32_t)n;
        }
        case USER_FD_VFILE: {
            const char* data = NULL;
            uint32_t len = user_fd_vfile((int)fd, &data);
            uint32_t off = user_fd_offset((int)fd);
            if (off >= len) return 0;
            uint32_t n = len - off;
            if (n > count) n = count;
            memcpy((void*)buf, data + off, n);
            user_fd_set_offset((int)fd, off + n);
            return n;
        }
        case USER_FD_DEV_NULL:
            return 0;
        case USER_FD_DEV_ZERO:
            memset((void*)buf, 0, count);
            return count;
        case USER_FD_STDIN:
        case USER_FD_DEV_CONSOLE:
            return read_keyboard_char(buf);
        case USER_FD_PIPE_READ: {
            int id = user_fd_pipe_id((int)fd);
            if (id < 0) return err(ERR_EBADF);
            int n = pipe_read(id, (char*)buf, count);
            return n < 0 ? err(ERR_EBADF) : (uint32_t)n;
        }
        case USER_FD_DIR:
            return err(ERR_EISDIR);
        default:
            return err(ERR_EBADF);     /* reading from stdout/stderr */
    }
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

/* ---- virtual namespace: flat RAM FS at "/" plus /dev, /proc, /etc ---- */

#define VFS_T_NONE  0
#define VFS_T_FILE  1
#define VFS_T_DIR    2
#define VFS_T_DEV    3
#define VFS_T_VFILE  4
#define VFS_T_MODULE 5

#define VFS_DIR_ROOT 1
#define VFS_DIR_DEV  2
#define VFS_DIR_PROC 3
#define VFS_DIR_ETC  4
#define VFS_DIR_PID  5
#define VFS_DIR_BIN  6
#define VFS_DIR_USRBIN 7
#define VFS_DIR_SBIN 8
#define VFS_DIR_REAL 9

#define VFS_DEV_NULL    1
#define VFS_DEV_ZERO    2
#define VFS_DEV_CONSOLE 3

#define VFS_VF_MEMINFO 1
#define VFS_VF_UPTIME  2
#define VFS_VF_VERSION 3
#define VFS_VF_CPUINFO 4
#define VFS_VF_MOUNTS  5
#define VFS_VF_MTAB    6
#define VFS_VF_PID_STAT    7
#define VFS_VF_PID_STATUS  8
#define VFS_VF_PID_CMDLINE 9
#define VFS_VF_PID_COMM    10
#define VFS_VF_LOADAVG     11
#define VFS_VF_STAT        12
#define VFS_VF_MAPS        13
#define VFS_VF_PID_MAPS    14

typedef struct {
    int  type;
    int  slot;       /* VFS_T_FILE */
    int  id;         /* directory / device / proc-file id */
    uint32_t pid;    /* VFS_DIR_PID / VFS_VF_PID_* */
    char path[USER_FD_PATH_MAX];
} vfs_ent_t;

static bool_t has_slash(const char* s) {
    for (; *s; s++) if (*s == '/') return true;
    return false;
}

/* Canonicalize a path against the process cwd ("." and ".." supported).
 * Returns 0 with an absolute path in out, or -ENAMETOOLONG. */
static uint32_t vfs_normalize(const char* in, char* out, uint32_t max) {
    char tmp[USER_FD_PATH_MAX];
    uint32_t n = 0;
    tmp[0] = '\0';

    if (in[0] != '/') {
        const char* cwd = user_cwd_get();
        if (cwd[0] == '/' && cwd[1] != '\0') {
            uint32_t cl = strlen(cwd);
            if (cl >= sizeof(tmp)) return err(ERR_ENAMETOOLONG);
            strncpy(tmp, cwd, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            n = cl;
        }
    }

    const char* p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - start);

        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n == 0) continue;
            uint32_t i = n;
            while (i > 0 && tmp[i - 1] != '/') i--;
            n = i ? i - 1 : 0;        /* cut at the slash; root is empty */
            tmp[n] = '\0';
            continue;
        }
        if (n + len + 2 >= sizeof(tmp)) return err(ERR_ENAMETOOLONG);
        tmp[n++] = '/';
        memcpy(tmp + n, start, len);
        n += len;
        tmp[n] = '\0';
    }

    if (n == 0) {
        if (max < 2) return err(ERR_ENAMETOOLONG);
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    if (n + 1 > max) return err(ERR_ENAMETOOLONG);
    memcpy(out, tmp, n + 1);
    return 0;
}

static int vfs_proc_file_id(const char* name) {
    if (strcmp(name, "meminfo") == 0) return VFS_VF_MEMINFO;
    if (strcmp(name, "uptime") == 0) return VFS_VF_UPTIME;
    if (strcmp(name, "version") == 0) return VFS_VF_VERSION;
    if (strcmp(name, "cpuinfo") == 0) return VFS_VF_CPUINFO;
    if (strcmp(name, "mounts") == 0) return VFS_VF_MOUNTS;
    if (strcmp(name, "loadavg") == 0) return VFS_VF_LOADAVG;
    if (strcmp(name, "stat") == 0) return VFS_VF_STAT;
    if (strcmp(name, "maps") == 0) return VFS_VF_MAPS;
    return 0;
}

static int vfs_proc_pid_file_id(const char* name) {
    if (strcmp(name, "stat") == 0) return VFS_VF_PID_STAT;
    if (strcmp(name, "status") == 0) return VFS_VF_PID_STATUS;
    if (strcmp(name, "cmdline") == 0) return VFS_VF_PID_CMDLINE;
    if (strcmp(name, "comm") == 0) return VFS_VF_PID_COMM;
    if (strcmp(name, "maps") == 0) return VFS_VF_PID_MAPS;
    return 0;
}

/* "/proc/self" resolves to the running process; other names must be digits. */
static bool_t vfs_parse_pid_token(const char* s, uint32_t* out) {
    if (strcmp(s, "self") == 0) {
        proc_t* p = proc_current();
        if (!p) return false;
        *out = p->pid;
        return true;
    }
    if (!*s) return false;
    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10u + (uint32_t)(*p - '0');
    }
    if (v == 0) return false;
    *out = v;
    return true;
}

static uint32_t vfs_lookup(const char* in, vfs_ent_t* e) {
    memset(e, 0, sizeof(*e));
    uint32_t rc = vfs_normalize(in, e->path, USER_FD_PATH_MAX);
    if ((int32_t)rc < 0) return rc;

    if (strcmp(e->path, "/") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_ROOT; return 0; }
    if (strcmp(e->path, "/dev") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_DEV; return 0; }
    if (strcmp(e->path, "/proc") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_PROC; return 0; }
    if (strcmp(e->path, "/etc") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_ETC; return 0; }
    if (strcmp(e->path, "/bin") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_BIN; return 0; }
    if (strcmp(e->path, "/usr/bin") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_USRBIN; return 0; }
    if (strcmp(e->path, "/sbin") == 0) { e->type = VFS_T_DIR; e->id = VFS_DIR_SBIN; return 0; }

    if (strncmp(e->path, "/dev/", 5) == 0) {
        const char* n = e->path + 5;
        if (has_slash(n)) return err(ERR_ENOENT);
        if (strcmp(n, "null") == 0) { e->type = VFS_T_DEV; e->id = VFS_DEV_NULL; return 0; }
        if (strcmp(n, "zero") == 0) { e->type = VFS_T_DEV; e->id = VFS_DEV_ZERO; return 0; }
        if (strcmp(n, "console") == 0) { e->type = VFS_T_DEV; e->id = VFS_DEV_CONSOLE; return 0; }
        return err(ERR_ENOENT);
    }
    if (strncmp(e->path, "/proc/", 6) == 0) {
        const char* n = e->path + 6;
        if (!has_slash(n)) {
            int id = vfs_proc_file_id(n);
            if (id) {
                e->type = VFS_T_VFILE;
                e->id = id;
                return 0;
            }
            uint32_t pid = 0;
            if (vfs_parse_pid_token(n, &pid) && proc_find(pid)) {
                e->type = VFS_T_DIR;
                e->id = VFS_DIR_PID;
                e->pid = pid;
                return 0;
            }
            return err(ERR_ENOENT);
        }

        const char* slash = n;
        while (*slash && *slash != '/') slash++;
        uint32_t tok_len = (uint32_t)(slash - n);
        if (tok_len == 0 || tok_len >= 16) return err(ERR_ENOENT);
        char token[16];
        memcpy(token, n, tok_len);
        token[tok_len] = '\0';

        uint32_t pid = 0;
        if (!vfs_parse_pid_token(token, &pid) || !proc_find(pid))
            return err(ERR_ENOENT);

        const char* fname = slash + 1;
        if (has_slash(fname)) return err(ERR_ENOENT);
        int fid = vfs_proc_pid_file_id(fname);
        if (!fid) return err(ERR_ENOENT);
        e->type = VFS_T_VFILE;
        e->id = fid;
        e->pid = pid;
        return 0;
    }
    if (strncmp(e->path, "/etc/", 5) == 0) {
        const char* n = e->path + 5;
        if (strcmp(n, "mtab") == 0) { e->type = VFS_T_VFILE; e->id = VFS_VF_MTAB; return 0; }
        return err(ERR_ENOENT);
    }

    /* Virtual executable namespace: any /bin/<name> maps to the first
     * Multiboot module (the BusyBox binary), so BusyBox's shell can exec
     * applets by PATH without a real /bin/busybox file. */
    const char* prog = NULL;
    if (strncmp(e->path, "/bin/", 5) == 0) prog = e->path + 5;
    else if (strncmp(e->path, "/usr/bin/", 9) == 0) prog = e->path + 9;
    else if (strncmp(e->path, "/sbin/", 6) == 0) prog = e->path + 6;
    if (prog) {
        if (!*prog || has_slash(prog) || modules_count() == 0)
            return err(ERR_ENOENT);
        e->type = VFS_T_MODULE;
        e->id = 0;
        return 0;
    }

    const char* name = e->path + 1;
    if (fs_is_dir(name)) {
        e->type = VFS_T_DIR;
        e->id = VFS_DIR_REAL;
        return 0;
    }
    int slot = fs_open(name);
    if (slot < 0) return err(ERR_ENOENT);
    e->type = VFS_T_FILE;
    e->slot = slot;
    return 0;
}

/* ---- generated /proc content ---- */

static uint32_t sb_add(char* b, uint32_t pos, uint32_t max, const char* s) {
    while (*s && pos + 1 < max) b[pos++] = *s++;
    b[pos] = '\0';
    return pos;
}

static uint32_t sb_dec(char* b, uint32_t pos, uint32_t max, uint32_t v) {
    char tmp[12];
    uint32_t n = u32_to_dec(tmp, v);
    if (n > max - 1 - pos) n = max - 1 - pos;
    memcpy(b + pos, tmp, n);
    pos += n;
    b[pos] = '\0';
    return pos;
}

static uint32_t sb_dec02(char* b, uint32_t pos, uint32_t max, uint32_t v) {
    if (v < 10) pos = sb_add(b, pos, max, "0");
    return sb_dec(b, pos, max, v);
}

static uint32_t vfs_vfile_text(int id, uint32_t pid, char* out, uint32_t max) {
    uint32_t pos = 0;
    out[0] = '\0';

    switch (id) {
        case VFS_VF_MEMINFO: {
            uint32_t total_kb = mm_get_total_pages() * (PAGE_SIZE / 1024);
            uint32_t free_kb = mm_get_free_pages() * (PAGE_SIZE / 1024);
            pos = sb_add(out, pos, max, "MemTotal:       ");
            pos = sb_dec(out, pos, max, total_kb);
            pos = sb_add(out, pos, max, " kB\nMemFree:        ");
            pos = sb_dec(out, pos, max, free_kb);
            pos = sb_add(out, pos, max, " kB\nMemAvailable:   ");
            pos = sb_dec(out, pos, max, free_kb);
            pos = sb_add(out, pos, max, " kB\nBuffers:        0 kB\nCached:         0 kB\n");
            pos = sb_add(out, pos, max, "SwapTotal:      0 kB\nSwapFree:       0 kB\n");
            break;
        }
        case VFS_VF_UPTIME: {
            uint32_t t = pit_get_ticks();   /* 100 Hz */
            pos = sb_dec(out, pos, max, t / 100);
            pos = sb_add(out, pos, max, ".");
            pos = sb_dec02(out, pos, max, t % 100);
            pos = sb_add(out, pos, max, " ");
            pos = sb_dec(out, pos, max, t / 100);
            pos = sb_add(out, pos, max, ".");
            pos = sb_dec02(out, pos, max, t % 100);
            pos = sb_add(out, pos, max, "\n");
            break;
        }
        case VFS_VF_VERSION:
            pos = sb_add(out, pos, max, "Linux version 1.2.0 (muos@i386) #1\n");
            break;
        case VFS_VF_CPUINFO:
            pos = sb_add(out, pos, max,
                         "processor\t: 0\nmodel name\t: MuOS virtual i386\n\n");
            break;
        case VFS_VF_MOUNTS:
        case VFS_VF_MTAB:
            pos = sb_add(out, pos, max,
                         "/dev/root / ext2 rw 0 0\n"
                         "proc /proc proc rw 0 0\n"
                         "devtmpfs /dev devtmpfs rw 0 0\n");
            break;
        case VFS_VF_PID_STAT: {
            proc_t* p = proc_find(pid);
            if (!p) break;
            char st[2] = { proc_state_char(p), '\0' };
            pos = sb_dec(out, pos, max, p->pid);
            pos = sb_add(out, pos, max, " (");
            pos = sb_add(out, pos, max, p->name);
            pos = sb_add(out, pos, max, ") ");
            pos = sb_add(out, pos, max, st);
            pos = sb_add(out, pos, max, " ");
            pos = sb_dec(out, pos, max, p->ppid);
            pos = sb_add(out, pos, max,
                         " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 ");
            pos = sb_dec(out, pos, max, p->start_ticks);
            pos = sb_add(out, pos, max, " ");
            pos = sb_dec(out, pos, max, USER_SIZE);
            /* Linux /proc/<pid>/stat has 52 fields; BusyBox walks to a fixed
             * field index and needs a space terminator after the last one. */
            for (int i = 0; i < 29; i++) pos = sb_add(out, pos, max, " 0");
            pos = sb_add(out, pos, max, " \n");
            break;
        }
        case VFS_VF_PID_STATUS: {
            proc_t* p = proc_find(pid);
            if (!p) break;
            pos = sb_add(out, pos, max, "Name:\t");
            pos = sb_add(out, pos, max, p->name);
            pos = sb_add(out, pos, max, "\nState:\t");
            if (p->state == PROC_RUNNING) pos = sb_add(out, pos, max, "R (running)");
            else if (p->state == PROC_ZOMBIE) pos = sb_add(out, pos, max, "Z (zombie)");
            else pos = sb_add(out, pos, max, "S (sleeping)");
            pos = sb_add(out, pos, max, "\nPid:\t");
            pos = sb_dec(out, pos, max, p->pid);
            pos = sb_add(out, pos, max, "\nPPid:\t");
            pos = sb_dec(out, pos, max, p->ppid);
            pos = sb_add(out, pos, max,
                         "\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nVmSize:\t");
            pos = sb_dec(out, pos, max, USER_SIZE / 1024);
            pos = sb_add(out, pos, max, " kB\nThreads:\t1\n");
            break;
        }
        case VFS_VF_PID_COMM: {
            proc_t* p = proc_find(pid);
            if (!p) break;
            pos = sb_add(out, pos, max, p->name);
            pos = sb_add(out, pos, max, "\n");
            break;
        }
        case VFS_VF_PID_CMDLINE: {
            proc_t* p = proc_find(pid);
            if (!p) break;
            pos = sb_add(out, pos, max, p->cmdline);
            pos = sb_add(out, pos, max, "\n");
            break;
        }
        case VFS_VF_LOADAVG: {
            proc_t* p = proc_current();
            pos = sb_add(out, pos, max, "0.00 0.00 0.00 1/");
            pos = sb_dec(out, pos, max, (uint32_t)proc_count());
            pos = sb_add(out, pos, max, " ");
            pos = sb_dec(out, pos, max, p ? p->pid : 1);
            pos = sb_add(out, pos, max, "\n");
            break;
        }
        case VFS_VF_STAT: {
            pos = sb_add(out, pos, max,
                         "cpu  0 0 0 0 0 0 0 0 0 0\n"
                         "intr 0\nctxt 0\nbtime ");
            pos = sb_dec(out, pos, max, rtc_now());
            pos = sb_add(out, pos, max, "\nprocesses ");
            pos = sb_dec(out, pos, max, (uint32_t)proc_count());
            pos = sb_add(out, pos, max,
                         "\nprocs_running 1\nprocs_blocked 0\n");
            break;
        }
        case VFS_VF_MAPS:
        case VFS_VF_PID_MAPS:
            pos = sb_add(out, pos, max,
                         "08048000-08848000 rwxp 00000000 00:00 0 [stack]\n");
            break;
        default:
            break;
    }
    return pos;
}

static uint32_t vfs_vfile_size(int id, uint32_t pid) {
    char tmp[USER_FD_VBUF_MAX];
    return vfs_vfile_text(id, pid, tmp, sizeof(tmp));
}

static uint32_t vfs_module_size(int idx) {
    uint32_t start = 0, end = 0;
    if (!modules_get((uint32_t)idx, &start, &end)) return 0;
    return end - start;
}

/* ---- directory enumeration ---- */

static int vfs_dir_id(const char* path) {
    if (strcmp(path, "/") == 0) return VFS_DIR_ROOT;
    if (strcmp(path, "/dev") == 0) return VFS_DIR_DEV;
    if (strcmp(path, "/proc") == 0) return VFS_DIR_PROC;
    if (strcmp(path, "/etc") == 0) return VFS_DIR_ETC;
    if (strcmp(path, "/bin") == 0) return VFS_DIR_BIN;
    if (strcmp(path, "/usr/bin") == 0) return VFS_DIR_USRBIN;
    if (strcmp(path, "/sbin") == 0) return VFS_DIR_SBIN;
    return 0;
}

static bool_t vfs_proc_pid_dir(const char* path, uint32_t* pid) {
    if (strncmp(path, "/proc/", 6) != 0) return false;
    const char* n = path + 6;
    if (*n == '\0' || has_slash(n)) return false;
    uint32_t v = 0;
    if (!vfs_parse_pid_token(n, &v) || !proc_find(v)) return false;
    *pid = v;
    return true;
}

static bool_t vfs_dir_meta(const char* path, int* id, uint32_t* pid) {
    int d = vfs_dir_id(path);
    if (d) { *id = d; *pid = 0; return true; }
    uint32_t v = 0;
    if (vfs_proc_pid_dir(path, &v)) {
        *id = VFS_DIR_PID;
        *pid = v;
        return true;
    }
    if (path[0] == '/' && fs_is_dir(path + 1)) {
        *id = VFS_DIR_REAL;
        *pid = 0;
        return true;
    }
    return false;
}

static bool_t vfs_dir_entry(const char* path, uint32_t idx, char* name,
                            uint32_t* ino, uint8_t* type) {
    int dir = 0;
    uint32_t dir_pid = 0;
    if (!vfs_dir_meta(path, &dir, &dir_pid)) return false;

    if (idx == 0) { strncpy(name, ".", USER_FD_PATH_MAX); *ino = 1; *type = 4; return true; }
    if (idx == 1) { strncpy(name, "..", USER_FD_PATH_MAX); *ino = 1; *type = 4; return true; }
    idx -= 2;

    if (dir == VFS_DIR_ROOT) {
        int entries = fs_dir_count("");
        if ((int)idx < entries) {
            fs_dirent_t de;
            if (!fs_dir_get("", (int)idx, &de)) return false;
            strncpy(name, de.name, USER_FD_PATH_MAX - 1);
            name[USER_FD_PATH_MAX - 1] = '\0';
            *ino = de.is_dir ? 1000 + idx : (uint32_t)(de.slot + 3);
            *type = de.is_dir ? 4 : 8;
            return true;
        }
        idx -= (uint32_t)entries;
        const char* extra[4] = { "bin", "dev", "proc", "etc" };
        if (idx < 4) {
            strncpy(name, extra[idx], USER_FD_PATH_MAX);
            *ino = 100 + idx;
            *type = 4;                  /* DT_DIR */
            return true;
        }
        return false;
    }

    if (dir == VFS_DIR_DEV) {
        const char* devs[3] = { "null", "zero", "console" };
        if (idx < 3) {
            strncpy(name, devs[idx], USER_FD_PATH_MAX);
            *ino = 200 + idx;
            *type = 2;                  /* DT_CHR */
            return true;
        }
        return false;
    }
    if (dir == VFS_DIR_PROC) {
        const char* files[8] = { "meminfo", "uptime", "version", "cpuinfo",
                                 "mounts", "loadavg", "stat", "maps" };
        if (idx < 8) {
            strncpy(name, files[idx], USER_FD_PATH_MAX);
            *ino = 300 + idx;
            *type = 8;
            return true;
        }
        idx -= 8;
        int n = proc_count();
        if ((int)idx < n) {
            proc_t* p = proc_get_index((int)idx);
            if (!p) return false;
            u32_to_dec(name, p->pid);
            *ino = 500 + p->pid * 8;
            *type = 4;
            return true;
        }
        return false;
    }
    if (dir == VFS_DIR_PID) {
        const char* files[5] = { "stat", "status", "comm", "cmdline", "maps" };
        if (idx < 5) {
            strncpy(name, files[idx], USER_FD_PATH_MAX);
            *ino = 600 + dir_pid * 8 + idx;
            *type = 8;
            return true;
        }
        return false;
    }
    if (dir == VFS_DIR_REAL) {
        fs_dirent_t de;
        const char* rel = path[0] == '/' ? path + 1 : path;
        if (!fs_dir_get(rel, (int)idx, &de)) return false;
        strncpy(name, de.name, USER_FD_PATH_MAX - 1);
        name[USER_FD_PATH_MAX - 1] = '\0';
        *ino = de.is_dir ? 1100 + idx : (uint32_t)(de.slot + 3);
        *type = de.is_dir ? 4 : 8;
        return true;
    }
    if (dir == VFS_DIR_BIN || dir == VFS_DIR_USRBIN || dir == VFS_DIR_SBIN) {
        const char* progs[5] = { "busybox", "sh", "ls", "cat", "ps" };
        if (idx < 5) {
            strncpy(name, progs[idx], USER_FD_PATH_MAX);
            *ino = 900 + (uint32_t)dir * 8 + idx;
            *type = 8;
            return true;
        }
        return false;
    }
    if (dir == VFS_DIR_ETC && idx < 1) {
        strncpy(name, "mtab", USER_FD_PATH_MAX);
        *ino = 400;
        *type = 8;
        return true;
    }
    return false;
}

static uint32_t do_open_buf(const char* buf, uint32_t flags, uint32_t mode) {
    (void)mode;
    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) {
        if ((uint32_t)(-(int32_t)rc) != ERR_ENOENT) return rc;
        if (!(flags & O_CREAT) || (flags & O_DIRECTORY)) return rc;

        const char* name = e.path + 1;
        if (!*name) return err(ERR_ENOTDIR);
        if (strlen(name) >= FS_MAX_NAME) return err(ERR_ENAMETOOLONG);
        if (!fs_parent_is_dir(name)) return err(ERR_ENOTDIR);
        int slot = fs_create(name);
        if (slot < 0) return err(ERR_ENOSPC);
        int fd = user_fd_alloc(slot);
        if (fd < 0) return err(ERR_EMFILE);
        return (uint32_t)fd;
    }

    switch (e.type) {
        case VFS_T_DIR: {
            if (flags & (O_CREAT | O_TRUNC)) return err(ERR_EISDIR);
            int fd = user_fd_alloc_dir(e.path);
            if (fd < 0) return err(ERR_EMFILE);
            return (uint32_t)fd;
        }
        case VFS_T_DEV: {
            int kind = USER_FD_DEV_NULL;
            if (e.id == VFS_DEV_ZERO) kind = USER_FD_DEV_ZERO;
            else if (e.id == VFS_DEV_CONSOLE) kind = USER_FD_DEV_CONSOLE;
            int fd = user_fd_alloc_dev(kind);
            if (fd < 0) return err(ERR_EMFILE);
            return (uint32_t)fd;
        }
        case VFS_T_VFILE: {
            if ((flags & O_ACCMODE) != 0) return err(ERR_EACCES);
            char tmp[USER_FD_VBUF_MAX];
            uint32_t len = vfs_vfile_text(e.id, e.pid, tmp, sizeof(tmp));
            int fd = user_fd_alloc_vfile(tmp, len);
            if (fd < 0) return err(ERR_EMFILE);
            return (uint32_t)fd;
        }
        case VFS_T_FILE: {
            if (flags & O_DIRECTORY) return err(ERR_ENOTDIR);
            if (flags & O_TRUNC) fs_fd_truncate(e.slot);
            int fd = user_fd_alloc(e.slot);
            if (fd < 0) return err(ERR_EMFILE);
            return (uint32_t)fd;
        }
        default:
            return err(ERR_ENOENT);
    }
}

static uint32_t do_open(uint32_t path, uint32_t flags, uint32_t mode) {
    char buf[128];
    if (!copy_user_string(path, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_open_buf(buf, flags, mode);
}

static uint32_t do_close(uint32_t fd) {
    if (user_fd_kind((int)fd) < 0) return err(ERR_EBADF);
    user_fd_close((int)fd);
    return 0;
}

/* ---- stat / dirent support for the flat RAM FS ---- */

static void put_u16(char* b, uint32_t off, uint16_t v) { memcpy(b + off, &v, 2); }
static void put_u32(char* b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }
static void put_u64(char* b, uint32_t off, uint64_t v) { memcpy(b + off, &v, 8); }

/* Fill the i386 struct stat64 layout (96 bytes). */
static void fill_stat64(char* st, uint32_t ino, uint32_t mode,
                        uint32_t nlink, uint32_t size,
                        uint32_t atime, uint32_t mtime, uint32_t ctime) {
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
    put_u32(st, 64, atime);            /* atime */
    put_u32(st, 72, mtime);            /* mtime */
    put_u32(st, 80, ctime);            /* ctime */
    put_u64(st, 88, (uint64_t)ino);    /* st_ino */
}

/* Fill the legacy i386 struct stat layout (64 bytes). */
static void fill_stat_old(char* st, uint32_t ino, uint32_t mode,
                          uint32_t nlink, uint32_t size,
                          uint32_t atime, uint32_t mtime, uint32_t ctime) {
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
    put_u32(st, 32, atime);
    put_u32(st, 40, mtime);
    put_u32(st, 48, ctime);
}

static bool_t vfs_ent_fields(const vfs_ent_t* e, uint32_t* ino,
                             uint32_t* mode, uint32_t* nlink, uint32_t* size,
                             uint32_t* atime, uint32_t* mtime, uint32_t* ctime) {
    uint32_t now = rtc_now();
    switch (e->type) {
        case VFS_T_FILE:
            *ino = (uint32_t)(e->slot + 3);
            *mode = S_IFREG | 0644;
            *nlink = 1;
            *size = fs_fd_size(e->slot);
            *atime = fs_fd_atime(e->slot);
            *mtime = fs_fd_mtime(e->slot);
            *ctime = fs_fd_ctime(e->slot);
            return true;
        case VFS_T_DIR: {
            uint32_t base = 1;
            if (e->id == VFS_DIR_DEV) base = 100;
            else if (e->id == VFS_DIR_PROC) base = 101;
            else if (e->id == VFS_DIR_ETC) base = 102;
            else if (e->id == VFS_DIR_PID) base = 500 + e->pid * 8;
            else if (e->id == VFS_DIR_REAL) base = 1000;
            *ino = base;
            *mode = S_IFDIR | 0755;
            *nlink = 2;
            *size = 0;
            *atime = *mtime = *ctime = now;
            return true;
        }
        case VFS_T_DEV:
            *ino = 200 + (uint32_t)e->id;
            *mode = S_IFCHR | 0666;
            *nlink = 1;
            *size = 0;
            *atime = *mtime = *ctime = now;
            return true;
        case VFS_T_VFILE:
            *ino = (e->id >= VFS_VF_PID_STAT && e->id <= VFS_VF_PID_MAPS)
                   ? 600 + e->pid * 8 + (uint32_t)(e->id - VFS_VF_PID_STAT)
                   : 300 + (uint32_t)e->id;
            *mode = S_IFREG | 0444;
            *nlink = 1;
            *size = vfs_vfile_size(e->id, e->pid);
            *atime = *mtime = *ctime = now;
            return true;
        case VFS_T_MODULE:
            *ino = 900 + (uint32_t)e->id;
            *mode = S_IFREG | 0755;       /* executable */
            *nlink = 1;
            *size = vfs_module_size(e->id);
            *atime = *mtime = *ctime = now;
            return true;
        default:
            return false;
    }
}

static bool_t vfs_fd_fields(int fd, uint32_t* ino, uint32_t* mode,
                            uint32_t* nlink, uint32_t* size,
                            uint32_t* atime, uint32_t* mtime, uint32_t* ctime) {
    int kind = user_fd_kind(fd);
    uint32_t now = rtc_now();
    switch (kind) {
        case USER_FD_STDIN:
        case USER_FD_STDOUT:
        case USER_FD_STDERR:
            *ino = (uint32_t)fd + 1;
            *mode = S_IFCHR | 0666;
            *nlink = 1;
            *size = 0;
            *atime = *mtime = *ctime = now;
            return true;
        case USER_FD_FILE: {
            int slot = user_fd_slot(fd);
            if (slot < 0) return false;
            *ino = (uint32_t)(slot + 3);
            *mode = S_IFREG | 0644;
            *nlink = 1;
            *size = fs_fd_size(slot);
            *atime = fs_fd_atime(slot);
            *mtime = fs_fd_mtime(slot);
            *ctime = fs_fd_ctime(slot);
            return true;
        }
        case USER_FD_DIR: {
            vfs_ent_t e;
            memset(&e, 0, sizeof(e));
            e.type = VFS_T_DIR;
            if (!vfs_dir_meta(user_fd_dir_path(fd), &e.id, &e.pid)) return false;
            return vfs_ent_fields(&e, ino, mode, nlink, size, atime, mtime, ctime);
        }
        case USER_FD_DEV_NULL:
        case USER_FD_DEV_ZERO:
        case USER_FD_DEV_CONSOLE:
            *ino = 200 + (uint32_t)(kind - USER_FD_DEV_NULL);
            *mode = S_IFCHR | 0666;
            *nlink = 1;
            *size = 0;
            *atime = *mtime = *ctime = now;
            return true;
        case USER_FD_VFILE:
            *ino = 300;
            *mode = S_IFREG | 0444;
            *nlink = 1;
            *size = user_fd_vfile(fd, NULL);
            *atime = *mtime = *ctime = now;
            return true;
        case USER_FD_PIPE_READ:
        case USER_FD_PIPE_WRITE: {
            int id = user_fd_pipe_id(fd);
            if (id < 0) return false;
            *ino = 950 + (uint32_t)id;
            *mode = S_IFIFO | 0600;
            *nlink = 1;
            *size = pipe_fill(id);
            *atime = *mtime = *ctime = now;
            return true;
        }
        default:
            return false;
    }
}

static uint32_t do_stat_buf(const char* buf, uint32_t st_ptr, bool_t is64) {
    if (!user_ptr_ok(st_ptr, is64 ? STAT64_SIZE : STAT_OLD_SIZE))
        return err(ERR_EFAULT);

    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;

    uint32_t ino, mode, nlink, size, at, mt, ct;
    if (!vfs_ent_fields(&e, &ino, &mode, &nlink, &size, &at, &mt, &ct))
        return err(ERR_ENOENT);

    char st[STAT64_SIZE];
    if (is64) fill_stat64(st, ino, mode, nlink, size, at, mt, ct);
    else      fill_stat_old(st, ino, mode, nlink, size, at, mt, ct);
    memcpy((void*)st_ptr, st, is64 ? STAT64_SIZE : STAT_OLD_SIZE);
    return 0;
}

static uint32_t do_stat(uint32_t path_ptr, uint32_t st_ptr, bool_t is64) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_stat_buf(buf, st_ptr, is64);
}

static uint32_t do_fstat(uint32_t fd, uint32_t st_ptr, bool_t is64) {
    if (!user_ptr_ok(st_ptr, is64 ? STAT64_SIZE : STAT_OLD_SIZE))
        return err(ERR_EFAULT);

    uint32_t ino, mode, nlink, size, at, mt, ct;
    if (!vfs_fd_fields((int)fd, &ino, &mode, &nlink, &size, &at, &mt, &ct))
        return err(ERR_EBADF);

    char st[STAT64_SIZE];
    if (is64) fill_stat64(st, ino, mode, nlink, size, at, mt, ct);
    else      fill_stat_old(st, ino, mode, nlink, size, at, mt, ct);
    memcpy((void*)st_ptr, st, is64 ? STAT64_SIZE : STAT_OLD_SIZE);
    return 0;
}

/* Fill the i386 struct statx (256 bytes) with basic stats. */
static void fill_statx(char* st, uint32_t ino, uint32_t mode,
                       uint32_t nlink, uint32_t size,
                       uint32_t atime, uint32_t mtime, uint32_t ctime) {
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
    put_u64(st, 64, (uint64_t)atime);    /* stx_atime.sec */
    put_u64(st, 96, (uint64_t)ctime);    /* stx_ctime.sec */
    put_u64(st, 112, (uint64_t)mtime);   /* stx_mtime.sec */
}

/* statx(383): modern musl tries this before the stat64 family. An empty
 * path with AT_EMPTY_PATH means "stat the dirfd itself". */
static uint32_t do_statx(uint32_t dirfd, uint32_t path_ptr, uint32_t buf) {
    if (!user_ptr_ok(buf, STATX_SIZE)) return err(ERR_EFAULT);

    char path[128];
    if (!copy_user_string(path_ptr, path, sizeof(path))) return err(ERR_EFAULT);

    uint32_t ino, mode, nlink, size, at, mt, ct;
    if (path[0] == '\0') {
        int fd = (int32_t)dirfd;
        if (fd < 0) return err(ERR_ENOENT);
        if (!vfs_fd_fields(fd, &ino, &mode, &nlink, &size, &at, &mt, &ct))
            return err(ERR_EBADF);
    } else {
        vfs_ent_t e;
        uint32_t rc = vfs_lookup(path, &e);
        if ((int32_t)rc < 0) return rc;
        if (!vfs_ent_fields(&e, &ino, &mode, &nlink, &size, &at, &mt, &ct))
            return err(ERR_ENOENT);
    }

    char st[STATX_SIZE];
    fill_statx(st, ino, mode, nlink, size, at, mt, ct);
    memcpy((void*)buf, st, STATX_SIZE);
    return 0;
}

/* ---- statfs / sysinfo (enough for df and free) ---- */

static uint32_t fs_free_slots(void) {
    int used = fs_count();
    return used >= 0 && used < FS_MAX_FILES ? (uint32_t)(FS_MAX_FILES - used) : 0;
}

static void fill_statfs64(char* st) {
    uint32_t free_slots = fs_free_slots();
    memset(st, 0, STATFS64_SIZE);
    put_u32(st, 0, 0x4D554F53);            /* f_type "MUOS" */
    put_u32(st, 4, FS_MAX_SIZE);           /* f_bsize */
    put_u64(st, 8, (uint64_t)FS_MAX_FILES);
    put_u64(st, 16, (uint64_t)free_slots);
    put_u64(st, 24, (uint64_t)free_slots);
    put_u64(st, 32, (uint64_t)FS_MAX_FILES);
    put_u64(st, 40, (uint64_t)free_slots);
    put_u32(st, 56, FS_MAX_NAME);          /* f_namelen */
    put_u32(st, 60, FS_MAX_SIZE);          /* f_frsize */
}

static void fill_statfs_old(char* st) {
    uint32_t free_slots = fs_free_slots();
    memset(st, 0, STATFS_OLD_SIZE);
    put_u32(st, 0, 0x4D554F53);
    put_u32(st, 4, FS_MAX_SIZE);
    put_u32(st, 8, FS_MAX_FILES);
    put_u32(st, 12, free_slots);
    put_u32(st, 16, free_slots);
    put_u32(st, 20, FS_MAX_FILES);
    put_u32(st, 24, free_slots);
    put_u32(st, 36, FS_MAX_NAME);
    put_u32(st, 40, FS_MAX_SIZE);
}

static uint32_t do_statfs_path(uint32_t path_ptr, uint32_t buf, bool_t is64) {
    char path[128];
    if (!copy_user_string(path_ptr, path, sizeof(path))) return err(ERR_EFAULT);
    if (!user_ptr_ok(buf, is64 ? STATFS64_SIZE : STATFS_OLD_SIZE))
        return err(ERR_EFAULT);

    vfs_ent_t e;
    uint32_t rc = vfs_lookup(path, &e);
    if ((int32_t)rc < 0) return rc;

    char st[STATFS64_SIZE];
    if (is64) fill_statfs64(st);
    else      fill_statfs_old(st);
    memcpy((void*)buf, st, is64 ? STATFS64_SIZE : STATFS_OLD_SIZE);
    return 0;
}

static uint32_t do_fstatfs(uint32_t fd, uint32_t buf, bool_t is64) {
    if (user_fd_kind((int)fd) < 0) return err(ERR_EBADF);
    if (!user_ptr_ok(buf, is64 ? STATFS64_SIZE : STATFS_OLD_SIZE))
        return err(ERR_EFAULT);

    char st[STATFS64_SIZE];
    if (is64) fill_statfs64(st);
    else      fill_statfs_old(st);
    memcpy((void*)buf, st, is64 ? STATFS64_SIZE : STATFS_OLD_SIZE);
    return 0;
}

static uint32_t do_sysinfo(uint32_t ptr) {
    if (!user_ptr_ok(ptr, SYSINFO_SIZE)) return err(ERR_EFAULT);

    char si[SYSINFO_SIZE];
    memset(si, 0, sizeof(si));
    put_u32(si, 0, pit_get_ticks() / 100);            /* uptime, seconds */
    put_u32(si, 16, mm_get_total_pages() * PAGE_SIZE);
    put_u32(si, 20, mm_get_free_pages() * PAGE_SIZE);
    put_u16(si, 40, (uint16_t)task_get_count());      /* procs */
    put_u32(si, 52, 1);                               /* mem_unit = 1 byte */
    memcpy((void*)ptr, si, SYSINFO_SIZE);
    return 0;
}

/* Write a kernel buffer (sendfile path cannot use user_ptr_ok checks). */
static int kbuf_write(uint32_t fd, const char* data, uint32_t count) {
    int kind = user_fd_kind((int)fd);
    switch (kind) {
        case USER_FD_FILE: {
            int slot = user_fd_slot((int)fd);
            if (slot < 0) return -ERR_EBADF;
            uint32_t off = user_fd_offset((int)fd);
            int n = fs_write_at(slot, off, data, count);
            if (n < 0) return -ERR_EBADF;
            user_fd_set_offset((int)fd, off + (uint32_t)n);
            return n;
        }
        case USER_FD_DEV_NULL:
        case USER_FD_DEV_ZERO:
            return (int)count;
        case USER_FD_STDOUT:
        case USER_FD_STDERR:
        case USER_FD_DEV_CONSOLE:
            for (uint32_t i = 0; i < count; i++) {
                vga_putchar(data[i]);
                serial_putchar(data[i]);
            }
            return (int)count;
        case USER_FD_VFILE:
            return -ERR_EACCES;
        case USER_FD_PIPE_WRITE: {
            int id = user_fd_pipe_id((int)fd);
            if (id < 0) return -ERR_EBADF;
            int n = pipe_write(id, data, count);
            return n < 0 ? -ERR_EBADF : n;
        }
        case USER_FD_DIR:
            return -ERR_EISDIR;
        default:
            return -ERR_EBADF;
    }
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

/* getdents(141) and getdents64(220): walk the open directory fd. */
static uint32_t do_getdents(uint32_t fd, uint32_t buf, uint32_t count,
                            bool_t is64) {
    if (!user_fd_is_dir((int)fd)) {
        return err(user_fd_kind((int)fd) >= 0 ? ERR_ENOTDIR : ERR_EBADF);
    }
    if (count < 8) return err(ERR_EINVAL);

    const char* dirpath = user_fd_dir_path((int)fd);
    uint32_t idx = user_fd_offset((int)fd);
    uint32_t total = 0;
    char name[USER_FD_PATH_MAX];
    uint32_t ino;
    uint8_t type;

    while (vfs_dir_entry(dirpath, idx, name, &ino, &type)) {
        uint32_t namelen = strlen(name);
        uint32_t reclen = is64
            ? ((19 + namelen + 1 + 7) & ~7u)
            : ((10 + namelen + 1 + 3) & ~3u);
        if (total + reclen > count) {
            if (total == 0) return err(ERR_EINVAL);
            break;
        }
        if (!user_ptr_ok(buf + total, reclen)) return err(ERR_EFAULT);

        char rec[96];
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

/* Size for the regular-file-like fds (RAM FS files and /proc snapshots). */
static bool_t fd_regular_size(int fd, uint32_t* size) {
    int kind = user_fd_kind(fd);
    if (kind == USER_FD_FILE) {
        int slot = user_fd_slot(fd);
        if (slot < 0) return false;
        *size = fs_fd_size(slot);
        return true;
    }
    if (kind == USER_FD_VFILE) {
        *size = user_fd_vfile(fd, NULL);
        return true;
    }
    return false;
}

static uint32_t do_lseek(uint32_t fd, uint32_t offset, uint32_t whence) {
    uint32_t fsize;
    if (!fd_regular_size((int)fd, &fsize)) return err(ERR_EBADF);

    int32_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int32_t)user_fd_offset((int)fd); break;
        case 2: base = (int32_t)fsize; break;
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
    uint32_t fsize;
    if (!fd_regular_size((int)fd, &fsize)) return err(ERR_EBADF);
    if (!user_ptr_ok(result, 8)) return err(ERR_EFAULT);

    int64_t base;
    switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)user_fd_offset((int)fd); break;
        case 2: base = (int64_t)fsize; break;
        default: return err(ERR_EINVAL);
    }
    int64_t pos = base + (int64_t)(((uint64_t)off_hi << 32) | off_lo);
    if (pos < 0 || pos > FS_MAX_SIZE) return err(ERR_EINVAL);
    user_fd_set_offset((int)fd, (uint32_t)pos);
    uint64_t out = (uint64_t)pos;
    memcpy((void*)result, &out, 8);
    return 0;
}

static uint32_t do_access_buf(const char* buf, uint32_t mode) {
    (void)mode;
    vfs_ent_t e;
    return vfs_lookup(buf, &e);
}

static uint32_t do_access(uint32_t path_ptr, uint32_t mode) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_access_buf(buf, mode);
}

/* RAM FS modes are fixed; chmod on real files/directories is a no-op. */
static uint32_t do_chmod_buf(const char* buf) {
    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;
    if (e.type == VFS_T_FILE) return 0;
    if (e.type == VFS_T_DIR && e.id == VFS_DIR_REAL) return 0;
    return err(ERR_EPERM);
}

static uint32_t do_chmod(uint32_t path_ptr, uint32_t mode) {
    (void)mode;
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_chmod_buf(buf);
}

static uint32_t do_fchmod(uint32_t fd, uint32_t mode) {
    (void)mode;
    int kind = user_fd_kind((int)fd);
    if (kind < 0) return err(ERR_EBADF);
    if (kind == USER_FD_FILE || kind == USER_FD_DIR) return 0;
    return err(ERR_EPERM);
}

static uint32_t do_ftruncate(uint32_t fd, uint32_t len) {
    int kind = user_fd_kind((int)fd);
    if (kind < 0) return err(ERR_EBADF);
    if (kind != USER_FD_FILE) return err(ERR_EINVAL);
    int slot = user_fd_slot((int)fd);
    if (slot < 0) return err(ERR_EBADF);
    return fs_truncate(slot, len) == 0 ? 0 : err(ERR_EINVAL);
}

static uint32_t do_truncate(uint32_t path_ptr, uint32_t len) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);

    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;
    if (e.type == VFS_T_DIR) return err(ERR_EISDIR);
    if (e.type != VFS_T_FILE) return err(ERR_EINVAL);
    return fs_truncate(e.slot, len) == 0 ? 0 : err(ERR_EINVAL);
}

static uint32_t do_truncate64(uint32_t path_ptr, uint32_t lo, uint32_t hi) {
    if (hi) return err(ERR_EFBIG);          /* RAM FS files are <= 64KB */
    return do_truncate(path_ptr, lo);
}

static uint32_t do_ftruncate64(uint32_t fd, uint32_t lo, uint32_t hi) {
    if (hi) return err(ERR_EFBIG);
    return do_ftruncate(fd, lo);
}

/* RAM FS writes are already in memory; sync is a successful no-op. */
static uint32_t do_fsync(uint32_t fd) {
    return user_fd_kind((int)fd) < 0 ? err(ERR_EBADF) : 0;
}

/* ---- rlimits ---- */

static bool_t rlimit_values(uint32_t resource, uint32_t* cur, uint32_t* max) {
    if (resource > 15) return false;
    switch (resource) {
        case RLIMIT_NOFILE: *cur = *max = USER_FD_MAX; break;
        case RLIMIT_STACK:
        case RLIMIT_DATA:
        case RLIMIT_AS:     *cur = *max = USER_SIZE; break;
        case RLIMIT_CORE:   *cur = *max = 0; break;
        default:            *cur = *max = RLIM_INFINITY; break;
    }
    return true;
}

static uint32_t do_getrlimit(uint32_t resource, uint32_t ptr) {
    if (!user_ptr_ok(ptr, 8)) return err(ERR_EFAULT);
    uint32_t cur, max;
    if (!rlimit_values(resource, &cur, &max)) return err(ERR_EINVAL);
    put_u32((char*)ptr, 0, cur);
    put_u32((char*)ptr, 4, max);
    return 0;
}

static uint32_t do_setrlimit(uint32_t resource, uint32_t ptr) {
    if (!user_ptr_ok(ptr, 8)) return err(ERR_EFAULT);
    if (resource > 15) return err(ERR_EINVAL);
    return 0;   /* limits are advisory in this single-user kernel */
}

/* prlimit64(340): modern musl asks for limits through this call. */
static uint32_t do_prlimit64(uint32_t pid, uint32_t resource,
                             uint32_t new_ptr, uint32_t old_ptr) {
    if (new_ptr && !user_ptr_ok(new_ptr, RLIMIT64_SIZE)) return err(ERR_EFAULT);
    if (old_ptr && !user_ptr_ok(old_ptr, RLIMIT64_SIZE)) return err(ERR_EFAULT);

    uint32_t cur, max;
    if (!rlimit_values(resource, &cur, &max)) return err(ERR_EINVAL);

    proc_t* self = proc_current();
    uint32_t self_pid = self ? self->pid : 1;
    if (pid != 0 && pid != self_pid) {
        if (!proc_find(pid)) return err(ERR_ESRCH);
        return err(ERR_EPERM);
    }

    if (old_ptr) {
        put_u64((char*)old_ptr, 0, (uint64_t)cur);
        put_u64((char*)old_ptr, 8, (uint64_t)max);
    }
    return 0;
}

static uint32_t do_chdir(uint32_t path_ptr) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;
    if (e.type != VFS_T_DIR) return err(ERR_ENOTDIR);
    return user_cwd_set(e.path) ? 0 : err(ERR_ENAMETOOLONG);
}

static uint32_t do_mkdir_buf(const char* buf) {
    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc == 0) return err(ERR_EEXIST);
    if ((uint32_t)(-(int32_t)rc) != ERR_ENOENT) return rc;

    const char* name = e.path + 1;
    if (!*name) return err(ERR_EEXIST);
    if (strlen(name) >= FS_MAX_NAME) return err(ERR_ENAMETOOLONG);
    if (!fs_parent_is_dir(name)) return err(ERR_ENOTDIR);
    return fs_mkdir(name) == 0 ? 0 : err(ERR_ENOSPC);
}

static uint32_t do_mkdir(uint32_t path_ptr) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_mkdir_buf(buf);
}

static uint32_t do_rmdir_buf(const char* buf) {
    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;

    if (e.type != VFS_T_DIR || e.id != VFS_DIR_REAL) return err(ERR_EPERM);
    const char* name = e.path + 1;
    if (!fs_dir_is_empty(name)) return err(ERR_ENOTEMPTY);
    return fs_rmdir(name) == 0 ? 0 : err(ERR_ENOENT);
}

static uint32_t do_rmdir(uint32_t path_ptr) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_rmdir_buf(buf);
}

static uint32_t do_unlink_buf(const char* buf) {
    vfs_ent_t e;
    uint32_t rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;
    switch (e.type) {
        case VFS_T_FILE:
            return fs_delete(e.path + 1) == 0 ? 0 : err(ERR_ENOENT);
        case VFS_T_DIR:
            return err(ERR_EISDIR);
        default:
            return err(ERR_EPERM);
    }
}

static uint32_t do_unlink(uint32_t path_ptr) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);
    return do_unlink_buf(buf);
}

static uint32_t do_rename_bufs(const char* obuf, const char* nbuf) {
    vfs_ent_t oe, ne;
    uint32_t rc = vfs_lookup(obuf, &oe);
    if ((int32_t)rc < 0) return rc;
    if (oe.type == VFS_T_DIR) return err(ERR_EISDIR);
    if (oe.type != VFS_T_FILE) return err(ERR_EPERM);

    rc = vfs_lookup(nbuf, &ne);
    if ((int32_t)rc < 0 && (uint32_t)(-(int32_t)rc) != ERR_ENOENT) return rc;
    if ((int32_t)rc == 0) {
        if (ne.type == VFS_T_DIR) return err(ERR_EISDIR);
        if (ne.type != VFS_T_FILE) return err(ERR_EPERM);
        if (ne.slot == oe.slot) return 0;    /* same file already */
        fs_delete(ne.path + 1);              /* rename replaces the target */
    }

    const char* nname = ne.path + 1;
    if (!*nname) return err(ERR_ENOTDIR);
    if (strlen(nname) >= FS_MAX_NAME) return err(ERR_ENAMETOOLONG);
    if (!fs_parent_is_dir(nname)) return err(ERR_ENOTDIR);
    if (fs_is_dir(nname)) return err(ERR_EISDIR);
    return fs_rename(oe.slot, nname) == 0 ? 0 : err(ERR_EINVAL);
}

static uint32_t do_rename(uint32_t old_ptr, uint32_t new_ptr) {
    char obuf[128], nbuf[128];
    if (!copy_user_string(old_ptr, obuf, sizeof(obuf))) return err(ERR_EFAULT);
    if (!copy_user_string(new_ptr, nbuf, sizeof(nbuf))) return err(ERR_EFAULT);
    return do_rename_bufs(obuf, nbuf);
}

/* ---- *at family ----
 * Path arguments are resolved against the dirfd: absolute paths ignore it,
 * AT_FDCWD means the process cwd, and a directory fd prefixes its absolute
 * path. The composed string is then handed to the same vfs_lookup path. */

static uint32_t at_resolve_raw(int32_t dirfd, const char* raw,
                               char* out, uint32_t max) {
    uint32_t n = strlen(raw);
    if (raw[0] == '/' || dirfd == AT_FDCWD) {
        if (n + 1 > max) return err(ERR_ENAMETOOLONG);
        memcpy(out, raw, n + 1);       /* relative: vfs_normalize uses cwd */
        return 0;
    }
    if (user_fd_kind(dirfd) < 0) return err(ERR_EBADF);
    if (!user_fd_is_dir(dirfd)) return err(ERR_ENOTDIR);

    const char* base = user_fd_dir_path(dirfd);
    uint32_t bl = strlen(base);
    uint32_t rl = n;
    if (bl + 1 + rl + 1 > max) return err(ERR_ENAMETOOLONG);
    memcpy(out, base, bl);
    out[bl] = '/';
    memcpy(out + bl + 1, raw, rl + 1);
    return 0;
}

/* Copy and resolve a non-empty *at path argument. */
static uint32_t at_path(int32_t dirfd, uint32_t path_ptr, char* out, uint32_t max) {
    char raw[USER_FD_PATH_MAX];
    if (!copy_user_string(path_ptr, raw, sizeof(raw))) return err(ERR_EFAULT);
    if (raw[0] == '\0') return err(ERR_ENOENT);
    return at_resolve_raw(dirfd, raw, out, max);
}

static uint32_t do_openat(uint32_t dirfd, uint32_t path_ptr, uint32_t flags,
                          uint32_t mode) {
    char buf[128];
    uint32_t rc = at_path((int32_t)dirfd, path_ptr, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    return do_open_buf(buf, flags, mode);
}

static uint32_t do_mkdirat(uint32_t dirfd, uint32_t path_ptr) {
    char buf[128];
    uint32_t rc = at_path((int32_t)dirfd, path_ptr, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    return do_mkdir_buf(buf);
}

static uint32_t do_unlinkat(uint32_t dirfd, uint32_t path_ptr, uint32_t flags) {
    char buf[128];
    uint32_t rc = at_path((int32_t)dirfd, path_ptr, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    if (flags & AT_REMOVEDIR) return do_rmdir_buf(buf);
    return do_unlink_buf(buf);
}

static uint32_t do_renameat(uint32_t olddirfd, uint32_t old_ptr,
                            uint32_t newdirfd, uint32_t new_ptr) {
    char obuf[128], nbuf[128];
    uint32_t rc = at_path((int32_t)olddirfd, old_ptr, obuf, sizeof(obuf));
    if ((int32_t)rc < 0) return rc;
    rc = at_path((int32_t)newdirfd, new_ptr, nbuf, sizeof(nbuf));
    if ((int32_t)rc < 0) return rc;
    return do_rename_bufs(obuf, nbuf);
}

/* fstatat64(300): an empty path with AT_EMPTY_PATH stats the dirfd itself. */
static uint32_t do_fstatat(uint32_t dirfd, uint32_t path_ptr, uint32_t st_ptr,
                           uint32_t flags, bool_t is64) {
    char raw[USER_FD_PATH_MAX];
    if (!copy_user_string(path_ptr, raw, sizeof(raw))) return err(ERR_EFAULT);
    if (raw[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH)) return err(ERR_ENOENT);
        return do_fstat(dirfd, st_ptr, is64);
    }
    char buf[128];
    uint32_t rc = at_resolve_raw((int32_t)dirfd, raw, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    return do_stat_buf(buf, st_ptr, is64);
}

static uint32_t do_faccessat(uint32_t dirfd, uint32_t path_ptr, uint32_t mode) {
    char buf[128];
    uint32_t rc = at_path((int32_t)dirfd, path_ptr, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    return do_access_buf(buf, mode);
}

static uint32_t do_fchmodat(uint32_t dirfd, uint32_t path_ptr, uint32_t mode) {
    (void)mode;
    char buf[128];
    uint32_t rc = at_path((int32_t)dirfd, path_ptr, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    return do_chmod_buf(buf);
}

/* readlinkat(305): the VFS has no symlinks, so an existing target is
 * reported as "not a symlink" like Linux does. */
static uint32_t do_readlinkat(uint32_t dirfd, uint32_t path_ptr,
                              uint32_t buf_ptr, uint32_t bufsiz) {
    (void)buf_ptr; (void)bufsiz;
    char buf[128];
    uint32_t rc = at_path((int32_t)dirfd, path_ptr, buf, sizeof(buf));
    if ((int32_t)rc < 0) return rc;
    vfs_ent_t e;
    rc = vfs_lookup(buf, &e);
    if ((int32_t)rc < 0) return rc;
    return err(ERR_EINVAL);
}

/* utimes/futimesat/utimensat update the RAM FS timestamps. The time array
 * is either 2x timeval (usec) or 2x timespec (nsec, with UTIME_NOW/OMIT). */
#define UTIME_NOW  0x3FFFFFFFu
#define UTIME_OMIT 0x3FFFFFFEu

static uint32_t do_utime_path(uint32_t path_ptr, int fd, uint32_t times_ptr,
                              bool_t ns) {
    char buf[128];
    if (!copy_user_string(path_ptr, buf, sizeof(buf))) return err(ERR_EFAULT);

    int slot = -1;
    if (buf[0] == '\0') {
        if (fd < 0) return err(ERR_ENOENT);
        if (user_fd_kind(fd) < 0) return err(ERR_EBADF);
        slot = user_fd_slot(fd);            /* -1 for dirs/devices/vfiles */
    } else {
        vfs_ent_t e;
        uint32_t rc = vfs_lookup(buf, &e);
        if ((int32_t)rc < 0) return rc;
        if (e.type == VFS_T_FILE) slot = e.slot;
    }

    uint32_t now = rtc_now();
    if (!times_ptr) {
        if (slot >= 0) fs_set_times(slot, now, now);
        return 0;
    }
    if (!user_ptr_ok(times_ptr, 16)) return err(ERR_EFAULT);

    uint32_t asec, ans, msec, mns;
    memcpy(&asec, (const void*)times_ptr, 4);
    memcpy(&ans, (const void*)(times_ptr + 4), 4);
    memcpy(&msec, (const void*)(times_ptr + 8), 4);
    memcpy(&mns, (const void*)(times_ptr + 12), 4);

    if (slot < 0) return 0;                 /* timestamps ignored for non-files */

    uint32_t atime = asec, mtime = msec;
    if (ns) {
        if (ans == UTIME_NOW) atime = now;
        else if (ans == UTIME_OMIT) atime = fs_fd_atime(slot);
        if (mns == UTIME_NOW) mtime = now;
        else if (mns == UTIME_OMIT) mtime = fs_fd_mtime(slot);
    }
    fs_set_times(slot, atime, mtime);
    return 0;
}

static uint32_t do_getcwd(uint32_t buf, uint32_t size) {
    const char* cwd = user_cwd_get();
    uint32_t len = strlen(cwd);
    if (size <= len) return err(ERR_ERANGE);
    if (!user_ptr_ok(buf, len + 1)) return err(ERR_EFAULT);
    memcpy((void*)buf, cwd, len + 1);
    return len;
}

static uint32_t do_pipe(uint32_t fds_ptr, uint32_t flags) {
    (void)flags;
    if (!user_ptr_ok(fds_ptr, 8)) return err(ERR_EFAULT);

    int id = pipe_create();
    if (id < 0) return err(ERR_EMFILE);

    int rfd = user_fd_alloc_pipe(id, false);
    int wfd = user_fd_alloc_pipe(id, true);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) user_fd_close(rfd);
        if (wfd >= 0) user_fd_close(wfd);
        return err(ERR_EMFILE);
    }
    ((uint32_t*)fds_ptr)[0] = (uint32_t)rfd;
    ((uint32_t*)fds_ptr)[1] = (uint32_t)wfd;
    return 0;
}

static uint32_t do_dup(uint32_t fd) {
    if (user_fd_kind((int)fd) < 0) return err(ERR_EBADF);
    int nfd = user_fd_dup((int)fd);
    if (nfd < 0) return err(ERR_EMFILE);
    return (uint32_t)nfd;
}

/* ---- sequential fork/wait model ----
 * Only one address space is live. fork() copies the 8MB sandbox plus fd,
 * cwd and brk state into a child snapshot; when the parent calls waitpid,
 * the child is loaded into the sandbox and run to exit, then the parent
 * image is restored and waitpid returns. Each process owns a separate
 * kernel stack for its Ring3->Ring0 entries. */

static proc_t* find_child(int32_t want_pid) {
    proc_t* parent = proc_current();
    if (!parent) return NULL;
    int n = proc_count();
    for (int i = 0; i < n; i++) {
        proc_t* p = proc_get_index(i);
        if (!p->used || p->ppid != parent->pid) continue;
        if (want_pid > 0 && p->pid != (uint32_t)want_pid) continue;
        return p;
    }
    return NULL;
}

static uint32_t do_kill(uint32_t pid, uint32_t sig) {
    (void)sig;   /* signals are tracked but never delivered yet */
    if (pid == 0) return 0;
    return proc_find(pid) ? 0 : err(ERR_ESRCH);
}

static uint32_t do_fork(registers_t* regs) {
    proc_t* parent = proc_current();
    if (!parent || parent->pid == 1) return err(ERR_EPERM);

    proc_t* child = proc_alloc(parent->name, parent->pid);
    if (!child) return err(ERR_EAGAIN);

    if (!proc_snapshot_alloc(child) || !proc_fdstate_alloc(child)) {
        proc_reap(child);
        return err(ERR_ENOMEM);
    }
    memcpy(child->mem, (void*)USER_BASE, USER_SIZE);
    user_fd_state_save(child->fd_state);

    strncpy(child->cwd, user_cwd_get(), USER_FD_PATH_MAX - 1);
    child->cwd[USER_FD_PATH_MAX - 1] = '\0';
    user_get_brk_state(&child->brk_base, &child->brk_cur);
    gdt_get_user_tls(&child->tls_base, &child->tls_limit, &child->tls_flags);

    child->eip = regs->eip;
    child->esp = regs->useresp;
    child->eflags = regs->eflags;
    child->eax = 0;
    child->ebx = regs->ebx;
    child->ecx = regs->ecx;
    child->edx = regs->edx;
    child->esi = regs->esi;
    child->edi = regs->edi;
    child->ebp = regs->ebp;
    child->fs_sel = regs->fs ? regs->fs : 0x23;
    child->gs_sel = regs->gs ? regs->gs : 0x23;
    child->has_ctx = true;

    return child->pid;
}

/* Run one child snapshot inside the live sandbox, then restore the parent. */
static bool_t proc_run_child(proc_t* parent, proc_t* child) {
    if (!proc_snapshot_alloc(parent) || !proc_fdstate_alloc(parent)) return false;

    memcpy(parent->mem, (void*)USER_BASE, USER_SIZE);
    user_fd_state_save(parent->fd_state);
    strncpy(parent->cwd, user_cwd_get(), USER_FD_PATH_MAX - 1);
    parent->cwd[USER_FD_PATH_MAX - 1] = '\0';
    user_get_brk_state(&parent->brk_base, &parent->brk_cur);

    uint32_t saved_esp0 = tss_get_esp0();
    uint32_t saved_root_esp = user_kernel_esp_save;
    uint32_t parent_tls_base = 0, parent_tls_limit = 0, parent_tls_flags = 0;
    gdt_get_user_tls(&parent_tls_base, &parent_tls_limit, &parent_tls_flags);

    memcpy((void*)USER_BASE, child->mem, USER_SIZE);
    user_fd_state_restore(child->fd_state);
    user_cwd_set(child->cwd);
    user_restore_brk_state(child->brk_base, child->brk_cur);
    gdt_set_user_tls(child->tls_base, child->tls_limit, child->tls_flags);

    child->state = PROC_RUNNING;
    proc_set_current(child);
    user_set_active(true);
    user_clear_exit_state();
    tss_set_esp0(child->kstack_top);

    user_enter_regs(child->eip, child->esp, 0,
                    child->ebx, child->ecx, child->edx,
                    child->esi, child->edi, child->ebp,
                    child->fs_sel, child->gs_sel);

    int code = user_last_exit_code();
    bool_t faulted = user_last_faulted();
    child->state = PROC_ZOMBIE;
    child->exit_code = faulted ? 139 : code;
    child->has_ctx = false;
    proc_snapshot_free(child);

    memcpy((void*)USER_BASE, parent->mem, USER_SIZE);
    user_fd_state_restore(parent->fd_state);
    user_cwd_set(parent->cwd);
    user_restore_brk_state(parent->brk_base, parent->brk_cur);

    tss_set_esp0(saved_esp0);
    user_kernel_esp_save = saved_root_esp;
    gdt_set_user_tls(parent_tls_base, parent_tls_limit, parent_tls_flags);
    proc_set_current(parent);
    parent->state = PROC_RUNNING;
    user_set_active(true);
    return true;
}

static uint32_t do_wait4(uint32_t pid_arg, uint32_t status_ptr, uint32_t options) {
    proc_t* parent = proc_current();
    if (!parent || parent->pid == 1) return err(ERR_ECHILD);
    if (status_ptr && !user_ptr_ok(status_ptr, 4)) return err(ERR_EFAULT);

    int32_t want = (int32_t)pid_arg;
    for (;;) {
        proc_t* child = find_child(want);
        if (!child) return err(ERR_ECHILD);

        if (child->state == PROC_ZOMBIE) {
            uint32_t cpid = child->pid;
            int code = child->exit_code;
            if (status_ptr) *(uint32_t*)status_ptr = (uint32_t)(code & 0xFF) << 8;
            proc_reap(child);
            return cpid;
        }
        if (!child->has_ctx) return err(ERR_ECHILD);
        if (options & 1) return 0;                 /* WNOHANG */
        if (!proc_run_child(parent, child)) return err(ERR_ENOMEM);
    }
}

static uint32_t do_execve(registers_t* regs, uint32_t path_ptr, uint32_t argv_ptr) {
    char path[128];
    if (!copy_user_string(path_ptr, path, sizeof(path))) return err(ERR_EFAULT);

    vfs_ent_t e;
    uint32_t rc = vfs_lookup(path, &e);
    if ((int32_t)rc < 0) return rc;

    const char* img = NULL;
    uint32_t img_size = 0;
    if (e.type == VFS_T_FILE) {
        img = fs_fd_data(e.slot);
        img_size = fs_fd_size(e.slot);
    } else if (e.type == VFS_T_MODULE) {
        uint32_t start = 0, end = 0;
        if (modules_get((uint32_t)e.id, &start, &end)) {
            img = (const char*)start;
            img_size = end - start;
        }
    } else {
        return err(ERR_ENOEXEC);
    }
    if (!img || img_size == 0) return err(ERR_ENOEXEC);

    char args[16][64];
    int argc = 0;
    if (argv_ptr) {
        for (int i = 0; i < 16; i++) {
            uint32_t p = 0;
            if (!user_ptr_ok(argv_ptr + (uint32_t)i * 4, 4)) return err(ERR_EFAULT);
            memcpy(&p, (const void*)(argv_ptr + (uint32_t)i * 4), 4);
            if (!p) break;
            if (!copy_user_string(p, args[argc], sizeof(args[argc])))
                return err(ERR_EFAULT);
            argc++;
        }
    }
    if (argc == 0) {
        const char* base = path;
        for (const char* s = path; *s; s++) if (*s == '/') base = s + 1;
        strncpy(args[0], base, sizeof(args[0]) - 1);
        args[0][sizeof(args[0]) - 1] = '\0';
        argc = 1;
    }

    uint32_t entry = 0, end = 0;
    if (elf32_validate(img, img_size, USER_BASE, USER_END - USER_STACK_RESERVE,
                       &entry, &end) != 0) return err(ERR_ENOEXEC);

    const char* cargv[16];
    for (int i = 0; i < argc && i < 16; i++) cargv[i] = args[i];

    uint32_t sp = 0;
    rc = (uint32_t)user_exec_image(img, img_size, argc, cargv, &entry, &sp);
    if ((int32_t)rc < 0) return err(ERR_ENOEXEC);

    proc_t* p = proc_current();
    if (p) {
        strncpy(p->name, args[0], PROC_NAME_MAX - 1);
        p->name[PROC_NAME_MAX - 1] = '\0';
        p->cmdline[0] = '\0';
        uint32_t pos = 0;
        for (int i = 0; i < argc; i++) {
            if (i && pos + 1 < PROC_CMDLINE_MAX) p->cmdline[pos++] = ' ';
            for (const char* s = args[i]; *s && pos + 1 < PROC_CMDLINE_MAX; s++)
                p->cmdline[pos++] = *s;
        }
        p->cmdline[pos] = '\0';
    }

    regs->eip = entry;
    regs->useresp = sp;
    regs->ebx = regs->ecx = regs->edx = 0;
    regs->esi = regs->edi = regs->ebp = 0;
    return 0;
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

/* Wall clock comes from CMOS; sub-second detail is derived from PIT ticks
 * (100Hz -> 10ms granularity). CLOCK_MONOTONIC* uses uptime instead. */
static uint32_t do_gettimeofday(uint32_t tv, uint32_t tz) {
    uint32_t ticks = pit_get_ticks();
    if (tv) {
        if (!user_ptr_ok(tv, 8)) return err(ERR_EFAULT);
        put_u32((char*)tv, 0, rtc_now());
        put_u32((char*)tv, 4, (ticks % 100) * 10000u);
    }
    if (tz) {
        if (!user_ptr_ok(tz, 8)) return err(ERR_EFAULT);
        memset((void*)tz, 0, 8);
    }
    return 0;
}

static uint32_t do_clock_gettime(uint32_t clockid, uint32_t ts,
                                 uint32_t struct_size) {
    if (!ts) return err(ERR_EFAULT);
    if (!user_ptr_ok(ts, struct_size)) return err(ERR_EFAULT);

    uint32_t ticks = pit_get_ticks();
    uint32_t sec, nsec = (ticks % 100) * 10000000u;
    if (clockid == 1 || clockid == 4) {          /* MONOTONIC / MONOTONIC_RAW */
        sec = ticks / 100;
    } else {
        sec = rtc_now();
    }

    if (struct_size == 16) {
        put_u64((char*)ts, 0, (uint64_t)sec);
        put_u64((char*)ts, 8, (uint64_t)nsec);
    } else {
        put_u32((char*)ts, 0, sec);
        put_u32((char*)ts, 4, nsec);
    }
    return 0;
}

static uint32_t do_fcntl(uint32_t fd, uint32_t cmd) {
    if (user_fd_kind((int)fd) < 0) return err(ERR_EBADF);
    switch (cmd) {
        case 0:              /* F_DUPFD */
        case 1030:           /* F_DUPFD_CLOEXEC */
            return do_dup(fd);
        case 1: return 0;    /* F_GETFD */
        case 2: return 0;    /* F_SETFD */
        case 3: return 0;    /* F_GETFL: O_RDONLY */
        case 4: return 0;    /* F_SETFL */
        case 8: case 9: return 0;   /* F_SETOWN / F_GETOWN */
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

/* The sandbox is pre-mapped, so mappings are simulated inside it: anonymous
 * ones come from the bump arena, file-backed ones copy the file contents in
 * and zero-fill the remainder. MAP_FIXED is allowed inside the sandbox; the
 * caller (e.g. a future dynamic loader) is responsible for what it covers. */
static uint32_t do_mmap(uint32_t addr, uint32_t len, uint32_t prot,
                        uint32_t flags, int fd, uint32_t offset) {
    (void)prot;
    if (len == 0) return err(ERR_EINVAL);

    uint32_t rounded = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);
    if (rounded < len || rounded > USER_SIZE) return err(ERR_ENOMEM);

    uint32_t at;
    if (flags & MAP_FIXED) {
        if (addr & (PAGE_SIZE - 1u)) return err(ERR_EINVAL);
        if (addr < USER_BASE || rounded > USER_END - USER_STACK_RESERVE - addr)
            return err(ERR_ENOMEM);
        at = addr;
    } else {
        at = user_mmap_anon(len);
        if (!at) return err(ERR_ENOMEM);
    }

    if (flags & MAP_ANONYMOUS) {
        if (fd != -1) return err(ERR_EINVAL);
        if (flags & MAP_FIXED) memset((void*)at, 0, rounded);
        return at;                  /* the bump arena is already zeroed */
    }

    if (fd < 0) return err(ERR_EBADF);
    if (offset & (PAGE_SIZE - 1u)) return err(ERR_EINVAL);

    const char* data = NULL;
    uint32_t fsize = 0;
    int kind = user_fd_kind(fd);
    if (kind == USER_FD_FILE) {
        int slot = user_fd_slot(fd);
        if (slot < 0) return err(ERR_EBADF);
        data = fs_fd_data(slot);
        fsize = fs_fd_size(slot);
    } else if (kind == USER_FD_VFILE) {
        fsize = user_fd_vfile(fd, &data);   /* read-only kernel snapshot */
    } else {
        return err(ERR_ENODEV);
    }

    memset((void*)at, 0, rounded);
    if (data && offset < fsize) {
        uint32_t avail = fsize - offset;
        uint32_t n = avail < rounded ? avail : rounded;
        memcpy((void*)at, data + offset, n);
    }
    return at;
}

/* mmap2 takes the offset in 4096-byte pages (i386 Linux ABI). */
static uint32_t do_mmap2(uint32_t addr, uint32_t len, uint32_t prot,
                         uint32_t flags, int fd, uint32_t pgoff) {
    if (pgoff > 0xFFFFFFFFu / PAGE_SIZE) return err(ERR_EINVAL);
    return do_mmap(addr, len, prot, flags, fd, pgoff * PAGE_SIZE);
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

static void proc_record_tls(void) {
    proc_t* p = proc_current();
    if (!p) return;
    gdt_get_user_tls(&p->tls_base, &p->tls_limit, &p->tls_flags);
}

static uint32_t do_set_thread_area(uint32_t ptr) {
    if (!user_ptr_ok(ptr, sizeof(user_desc_t))) return err(ERR_EFAULT);
    user_desc_t d;
    memcpy(&d, (const void*)ptr, sizeof(d));

    if (d.entry_number != 0xFFFFFFFFu && d.entry_number != 6) {
        return err(ERR_EINVAL);
    }

    gdt_set_user_tls(d.base_addr, d.limit, d.flags);
    proc_record_tls();

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
        case SYS_fork:
        case SYS_vfork:
            ret = do_fork(regs);
            break;
        case SYS_clone:
            /* Threads (CLONE_VM / custom stack) need real concurrency. */
            if ((regs->ebx & 0x100) || regs->ecx != 0) ret = err(ERR_ENOSYS);
            else ret = do_fork(regs);
            break;
        case SYS_kill:
            ret = do_kill(regs->ebx, regs->ecx);
            break;
        case SYS_mkdir:
            ret = do_mkdir(regs->ebx);
            break;
        case SYS_rmdir:
            ret = do_rmdir(regs->ebx);
            break;
        case SYS_waitpid:
        case SYS_wait4:
            ret = do_wait4(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_execve:
            ret = do_execve(regs, regs->ebx, regs->ecx);
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
        case SYS_openat:
            ret = do_openat(regs->ebx, regs->ecx, regs->edx, regs->esi);
            break;
        case SYS_mkdirat:
            ret = do_mkdirat(regs->ebx, regs->ecx);
            break;
        case SYS_fstatat64:
            ret = do_fstatat(regs->ebx, regs->ecx, regs->edx, regs->esi, true);
            break;
        case SYS_unlinkat:
            ret = do_unlinkat(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_renameat:
            ret = do_renameat(regs->ebx, regs->ecx, regs->edx, regs->esi);
            break;
        case SYS_readlinkat:
            ret = do_readlinkat(regs->ebx, regs->ecx, regs->edx, regs->esi);
            break;
        case SYS_fchmodat:
            ret = do_fchmodat(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_faccessat:
            ret = do_faccessat(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_utimes:
            ret = do_utime_path(regs->ebx, -1, regs->ecx, false);
            break;
        case SYS_futimesat:
            ret = do_utime_path(regs->ecx, (int)regs->ebx, regs->edx, false);
            break;
        case SYS_utimensat:
            ret = do_utime_path(regs->ecx, (int)regs->ebx, regs->edx, true);
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
        case SYS_chmod:
            ret = do_chmod(regs->ebx, regs->ecx);
            break;
        case SYS_fchmod:
            ret = do_fchmod(regs->ebx, regs->ecx);
            break;
        case SYS_truncate:
            ret = do_truncate(regs->ebx, regs->ecx);
            break;
        case SYS_ftruncate:
            ret = do_ftruncate(regs->ebx, regs->ecx);
            break;
        case SYS_truncate64:
            ret = do_truncate64(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_ftruncate64:
            ret = do_ftruncate64(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_fsync:
        case SYS_fdatasync:
            ret = do_fsync(regs->ebx);
            break;
        case SYS_sync:
            ret = 0;   /* every write already lives in memory */
            break;
        case SYS_getrlimit:
        case SYS_ugetrlimit:
            ret = do_getrlimit(regs->ebx, regs->ecx);
            break;
        case SYS_setrlimit:
            ret = do_setrlimit(regs->ebx, regs->ecx);
            break;
        case SYS_prlimit64:
            ret = do_prlimit64(regs->ebx, regs->ecx, regs->edx, regs->esi);
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
        case SYS_statfs:
            ret = do_statfs_path(regs->ebx, regs->ecx, false);
            break;
        case SYS_statfs64:
            if (regs->ecx != STATFS64_SIZE) { ret = err(ERR_EINVAL); break; }
            ret = do_statfs_path(regs->ebx, regs->edx, true);
            break;
        case SYS_fstatfs:
            ret = do_fstatfs(regs->ebx, regs->ecx, false);
            break;
        case SYS_fstatfs64:
            if (regs->ecx != STATFS64_SIZE) { ret = err(ERR_EINVAL); break; }
            ret = do_fstatfs(regs->ebx, regs->edx, true);
            break;
        case SYS_sysinfo:
            ret = do_sysinfo(regs->ebx);
            break;
        case SYS_rt_sigaction:
        case SYS_rt_sigprocmask:
        case SYS_sigaltstack:
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
            ret = rtc_now();
            break;
        case SYS_gettimeofday:
            ret = do_gettimeofday(regs->ebx, regs->ecx);
            break;
        case SYS_clock_gettime:
            ret = do_clock_gettime(regs->ebx, regs->ecx, 8);
            break;
        case SYS_clock_gettime64:
            ret = do_clock_gettime(regs->ebx, regs->ecx, 16);
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
        case SYS_getppid: {
            proc_t* p = proc_current();
            if (regs->eax == SYS_getppid)
                ret = p ? p->ppid : 0;
            else
                ret = p ? p->pid : 1;
            break;
        }
        case SYS_getpgrp:
        case SYS_setsid:
        case SYS_getpgid:
        case SYS_getsid: {
            proc_t* p = proc_current();
            ret = p ? p->pid : 1;
            break;
        }
        case SYS_setpgid:
            ret = 0;
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
            ret = user_fd_dup2((int)regs->ebx, (int)regs->ecx)
                  ? regs->ecx : err(ERR_EBADF);
            break;
        case SYS_pipe:
        case SYS_pipe2:
            ret = do_pipe(regs->ebx, regs->ecx);
            break;
        case SYS_brk:
            ret = (regs->ebx == 0 || !user_set_brk(regs->ebx))
                  ? user_get_brk() : regs->ebx;
            break;
        case SYS_old_mmap:
            ret = do_old_mmap(regs->ebx);
            break;
        case SYS_mmap2:
            /* ebp carries the file offset in 4096-byte pages */
            ret = do_mmap2(regs->ebx, regs->ecx, regs->edx, regs->esi,
                           (int)regs->edi, regs->ebp);
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
