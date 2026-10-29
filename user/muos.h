#ifndef MUOS_APP_H
#define MUOS_APP_H

/* Minimal runtime API available to Ring3 C programs. This is NOT libc:
 * programs built with these headers run under MuOS's Linux i386 syscall
 * ABI (int 0x80) with a freestanding startup. */

/* open(2) flags (Linux i386 values) */
#define MUOS_O_WRONLY 0x1
#define MUOS_O_CREAT  0x40
#define MUOS_O_TRUNC  0x200

int  muos_write(int fd, const void* buf, unsigned len);
int  muos_read(int fd, void* buf, unsigned len);
int  muos_open(const char* path, int flags);
int  muos_close(int fd);
int  muos_lseek(int fd, int offset, int whence);
int  muos_fork(void);
int  muos_waitpid(int pid, int* status, int options);
int  muos_execve(const char* path, char* const argv[], char* const envp[]);
void muos_exit(int code) __attribute__((noreturn));
unsigned muos_brk(unsigned addr);
int  muos_getpid(void);
int  muos_uname(char* out /* 390 bytes */);
unsigned muos_mmap(unsigned len);
int  muos_munmap(unsigned addr, unsigned len);

void muos_puts(const char* s);
void muos_putdec(int value);
void muos_puthex(unsigned value);

#endif
