#include "syscall.h"
#include "user.h"
#include "fs.h"
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
#define SYS_lseek     19
#define SYS_getpid    20
#define SYS_getuid    24
#define SYS_brk       45
#define SYS_getgid    47
#define SYS_geteuid   49
#define SYS_getegid   50
#define SYS_getppid   64
#define SYS_old_mmap  90
#define SYS_munmap    91
#define SYS_uname    122
#define SYS_mprotect 125
#define SYS_mmap2    192

#define ERR_EPERM      1
#define ERR_ENOENT     2
#define ERR_EBADF      9
#define ERR_ENOMEM    12
#define ERR_EFAULT    14
#define ERR_ENODEV    19
#define ERR_EINVAL    22
#define ERR_EMFILE    24
#define ERR_ENOSPC    28
#define ERR_ENOSYS    38

/* open(2) flags and mmap(2) constants (Linux i386 values) */
#define O_CREAT   0x40
#define O_TRUNC   0x200
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
        for (uint32_t i = 0; i < count; i++) {
            char c = ((const char*)buf)[i];
            vga_putchar(c);
            serial_putchar(c);
        }
        return count;
    }

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
        /* keyboard_getchar spins with interrupts enabled internally, which
         * is required here: int 0x80 also runs with IF=0. */
        char c = keyboard_getchar();
        *(char*)buf = c;
        vga_putchar(c);
        serial_putchar(c);
        return 1;
    }

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

static uint32_t do_open(uint32_t path, uint32_t flags, uint32_t mode) {
    (void)mode;
    char name[FS_MAX_NAME];
    if (!copy_user_string(path, name, sizeof(name))) return err(ERR_EFAULT);

    int slot = fs_open(name);
    if (slot < 0) {
        if (!(flags & O_CREAT)) return err(ERR_ENOENT);
        slot = fs_create(name);
        if (slot < 0) return err(ERR_ENOSPC);
    }
    if (flags & O_TRUNC) fs_fd_truncate(slot);

    int fd = user_fd_alloc(slot);
    if (fd < 0) return err(ERR_EMFILE);
    return (uint32_t)fd;
}

static uint32_t do_close(uint32_t fd) {
    if (user_fd_slot((int)fd) < 0) return err(ERR_EBADF);
    user_fd_close((int)fd);
    return 0;
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

static uint32_t do_uname(uint32_t ptr) {
    if (!user_ptr_ok(ptr, UTS_SIZE)) return err(ERR_EFAULT);

    char buf[UTS_SIZE];
    memset(buf, 0, sizeof(buf));
    strncpy(buf + UTS_FIELD * 0, "MuOS",     UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 1, "muos",     UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 2, "0.4.0",    UTS_FIELD - 1);
    strncpy(buf + UTS_FIELD * 3, "MuOS v0.4", UTS_FIELD - 1);
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
        case SYS_lseek:
            ret = do_lseek(regs->ebx, regs->ecx, regs->edx);
            break;
        case SYS_getpid:
        case SYS_getppid:
            ret = regs->eax == SYS_getppid ? 0 : 1;
            break;
        case SYS_getuid:
        case SYS_geteuid:
        case SYS_getgid:
        case SYS_getegid:
            ret = 0;
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
        default:
            report_unknown(regs->eax);
            ret = err(ERR_ENOSYS);
            break;
    }

    regs->eax = ret;   /* syscall_common pops this into eax before iret */
}
