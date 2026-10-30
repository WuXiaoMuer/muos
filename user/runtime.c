#include "muos.h"

/* Linux i386 syscall ABI: number in eax, args in ebx/ecx/edx,
 * result in eax, invoked with `int 0x80`. */
static int syscall0(int nr) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr)
                      : "memory");
    return ret;
}

static int syscall1(int nr, int a) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr), "b"(a)
                      : "memory");
    return ret;
}

static int syscall2(int nr, int a, int b) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr), "b"(a), "c"(b)
                      : "memory");
    return ret;
}

static int syscall3(int nr, int a, int b, int c) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return ret;
}

static int syscall4(int nr, int a, int b, int c, int d) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d)
                      : "memory");
    return ret;
}

static int syscall5(int nr, int a, int b, int c, int d, int e) {
    int ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                      : "memory");
    return ret;
}

int muos_write(int fd, const void* buf, unsigned len) {
    return syscall3(4, fd, (int)buf, (int)len);
}

int muos_read(int fd, void* buf, unsigned len) {
    return syscall3(3, fd, (int)buf, (int)len);
}

int muos_open(const char* path, int flags) {
    return syscall3(5, (int)path, flags, 0);
}

int muos_close(int fd) {
    return syscall1(6, fd);
}

/* *at family (Linux i386 numbers 295/296/300/301/302/305/306/307). */
int muos_openat(int dirfd, const char* path, int flags) {
    return syscall4(295, dirfd, (int)path, flags, 0);
}

int muos_mkdirat(int dirfd, const char* path, int mode) {
    return syscall3(296, dirfd, (int)path, mode);
}

int muos_fstatat64(int dirfd, const char* path, char* st, int flags) {
    return syscall4(300, dirfd, (int)path, (int)st, flags);
}

int muos_unlinkat(int dirfd, const char* path, int flags) {
    return syscall3(301, dirfd, (int)path, flags);
}

int muos_renameat(int olddirfd, const char* oldpath,
                  int newdirfd, const char* newpath) {
    return syscall4(302, olddirfd, (int)oldpath, newdirfd, (int)newpath);
}

int muos_faccessat(int dirfd, const char* path, int mode) {
    return syscall3(307, dirfd, (int)path, mode);
}

int muos_fchmodat(int dirfd, const char* path, int mode) {
    return syscall3(306, dirfd, (int)path, mode);
}

int muos_readlinkat(int dirfd, const char* path, char* buf, int size) {
    return syscall4(305, dirfd, (int)path, (int)buf, size);
}

int muos_fork(void) {
    return syscall0(2);
}

int muos_waitpid(int pid, int* status, int options) {
    return syscall3(7, pid, (int)status, options);
}

int muos_execve(const char* path, char* const argv[], char* const envp[]) {
    return syscall3(11, (int)path, (int)argv, (int)envp);
}

int muos_pipe(int fds[2]) {
    return syscall1(42, (int)fds);
}

int muos_lseek(int fd, int offset, int whence) {
    return syscall3(19, fd, offset, whence);
}

int muos_uname(char* out) {
    return syscall1(122, (int)out);
}

unsigned muos_mmap(unsigned len) {
    /* old_mmap(90): pointer to {addr,len,prot,flags,fd,offset} */
    unsigned args[6];
    args[0] = 0;
    args[1] = len;
    args[2] = 3;        /* PROT_READ|PROT_WRITE */
    args[3] = 0x22;     /* MAP_PRIVATE|MAP_ANONYMOUS */
    args[4] = (unsigned)-1;
    args[5] = 0;
    return (unsigned)syscall1(90, (int)args);
}

unsigned muos_mmap_file(int fd, unsigned len, int prot, int flags, unsigned offset) {
    unsigned args[6];
    args[0] = 0;
    args[1] = len;
    args[2] = (unsigned)prot;
    args[3] = (unsigned)flags;
    args[4] = (unsigned)fd;
    args[5] = offset;
    return (unsigned)syscall1(90, (int)args);
}

int muos_chmod(const char* path, int mode) {
    return syscall2(15, (int)path, mode);
}

int muos_truncate(const char* path, unsigned len) {
    return syscall2(92, (int)path, (int)len);
}

int muos_ftruncate(int fd, unsigned len) {
    return syscall2(93, fd, (int)len);
}

int muos_fsync(int fd) {
    return syscall1(118, fd);
}

int muos_getrlimit(int resource, unsigned* out) {
    return syscall2(76, resource, (int)out);
}

int muos_prlimit64(int resource, unsigned* out) {
    return syscall4(340, 0, resource, 0, (int)out);
}

int muos_poll(muos_pollfd_t* fds, int nfds, int timeout) {
    return syscall3(168, (int)fds, nfds, timeout);
}

int muos_select(int nfds, unsigned* r, unsigned* w, unsigned* e, unsigned* tv) {
    return syscall5(82, nfds, (int)r, (int)w, (int)e, (int)tv);
}

/* muos_pselect6 lives in syscall6.s: its 6th argument must travel in ebp,
 * which C inline asm cannot reserve while also pinning the other five. */

int muos_ppoll(muos_pollfd_t* fds, int nfds, unsigned* ts) {
    return syscall5(309, (int)fds, nfds, (int)ts, 0, 0);
}

int muos_nanosleep(unsigned* req, unsigned* rem) {
    return syscall2(162, (int)req, (int)rem);
}

int muos_readlink(const char* path, char* buf, unsigned size) {
    return syscall3(85, (int)path, (int)buf, (int)size);
}

int muos_chown(const char* path, int uid, int gid) {
    return syscall3(16, (int)path, uid, gid);
}

int muos_fchown(int fd, int uid, int gid) {
    return syscall3(95, fd, uid, gid);
}

int muos_fchownat(int dirfd, const char* path, int uid, int gid, int flags) {
    return syscall5(298, dirfd, (int)path, uid, gid, flags);
}

int muos_getrusage(int who, void* out) {
    return syscall2(77, who, (int)out);
}

unsigned muos_times(void* out) {
    return (unsigned)syscall1(43, (int)out);
}

int muos_munmap(unsigned addr, unsigned len) {
    return syscall2(91, (int)addr, (int)len);
}

void muos_exit(int code) {
    syscall1(1, code);
    for (;;) { /* the kernel never returns here */ }
}

unsigned muos_brk(unsigned addr) {
    return (unsigned)syscall1(45, (int)addr);
}

int muos_getpid(void) {
    return syscall1(20, 0);
}

static unsigned str_len(const char* s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

void muos_puts(const char* s) {
    muos_write(1, s, str_len(s));
}

void muos_putdec(int value) {
    char buf[12];
    unsigned p = 0;
    unsigned mag;
    if (value < 0) {
        buf[p++] = '-';
        mag = (unsigned)(-value);
    } else {
        mag = (unsigned)value;
    }
    char tmp[10];
    unsigned n = 0;
    if (mag == 0) {
        tmp[n++] = '0';
    } else {
        while (mag) { tmp[n++] = (char)('0' + (mag % 10)); mag /= 10; }
    }
    while (n) buf[p++] = tmp[--n];
    muos_write(1, buf, p);
}

void muos_puthex(unsigned value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[10];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[2 + i] = hex[(value >> ((7 - i) * 4)) & 0xF];
    }
    muos_write(1, buf, 10);
}

/* GCC may still emit calls to these even under -ffreestanding; the user
 * image links with -nostdlib, so provide the symbols here. */
void* memset(void* dst, int value, unsigned count) {
    unsigned char* d = (unsigned char*)dst;
    while (count--) *d++ = (unsigned char)value;
    return dst;
}

void* memcpy(void* dst, const void* src, unsigned count) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (count--) *d++ = *s++;
    return dst;
}
