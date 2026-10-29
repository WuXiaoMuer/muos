#include "muos.h"

/* Linux i386 syscall ABI: number in eax, args in ebx/ecx/edx,
 * result in eax, invoked with `int 0x80`. */
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
