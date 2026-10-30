#ifndef MUOS_APP_H
#define MUOS_APP_H

/* Minimal runtime API available to Ring3 C programs. This is NOT libc:
 * programs built with these headers run under MuOS's Linux i386 syscall
 * ABI (int 0x80) with a freestanding startup. */

/* open(2) flags (Linux i386 values) */
#define MUOS_O_WRONLY 0x1
#define MUOS_O_CREAT  0x40
#define MUOS_O_TRUNC  0x200
#define MUOS_O_DIRECTORY 0x10000

/* mmap(2) prot/flags and rlimit resources */
#define MUOS_PROT_READ  0x1
#define MUOS_MAP_PRIVATE 0x2
#define MUOS_MAP_FIXED  0x10
#define MUOS_MAP_ANONYMOUS 0x20
#define MUOS_RLIMIT_NOFILE 7

/* *at resolver flags (Linux i386 values) */
#define MUOS_AT_FDCWD     (-100)
#define MUOS_AT_REMOVEDIR 0x200
#define MUOS_AT_EMPTY_PATH 0x1000
#define MUOS_STAT64_SIZE 96

int  muos_write(int fd, const void* buf, unsigned len);
int  muos_read(int fd, void* buf, unsigned len);
int  muos_open(const char* path, int flags);
int  muos_close(int fd);
int  muos_lseek(int fd, int offset, int whence);
int  muos_openat(int dirfd, const char* path, int flags);
int  muos_mkdirat(int dirfd, const char* path, int mode);
int  muos_fstatat64(int dirfd, const char* path, char* st, int flags);
int  muos_unlinkat(int dirfd, const char* path, int flags);
int  muos_renameat(int olddirfd, const char* oldpath,
                   int newdirfd, const char* newpath);
int  muos_faccessat(int dirfd, const char* path, int mode);
int  muos_fchmodat(int dirfd, const char* path, int mode);
int  muos_readlinkat(int dirfd, const char* path, char* buf, int size);
int  muos_fork(void);
int  muos_waitpid(int pid, int* status, int options);
int  muos_execve(const char* path, char* const argv[], char* const envp[]);
int  muos_pipe(int fds[2]);
void muos_exit(int code) __attribute__((noreturn));
unsigned muos_brk(unsigned addr);
int  muos_getpid(void);
int  muos_uname(char* out /* 390 bytes */);
unsigned muos_mmap(unsigned len);
int  muos_munmap(unsigned addr, unsigned len);
unsigned muos_mmap_file(int fd, unsigned len, int prot, int flags, unsigned offset);
int  muos_chmod(const char* path, int mode);
int  muos_truncate(const char* path, unsigned len);
int  muos_ftruncate(int fd, unsigned len);
int  muos_fsync(int fd);
int  muos_getrlimit(int resource, unsigned* out /* 2 words */);
int  muos_prlimit64(int resource, unsigned* out /* 4 words: cur/max u64 */);

/* poll/select readiness (non-blocking: a timeout is treated as elapsed) */
typedef struct {
    int   fd;
    short events;
    short revents;
} muos_pollfd_t;

#define MUOS_POLLIN   0x001
#define MUOS_POLLOUT  0x004
#define MUOS_POLLHUP  0x010
#define MUOS_POLLNVAL 0x020

int muos_poll(muos_pollfd_t* fds, int nfds, int timeout);
int muos_select(int nfds, unsigned* r, unsigned* w, unsigned* e, unsigned* tv);
int muos_pselect6(int nfds, unsigned* r, unsigned* w, unsigned* e,
                  unsigned* ts, unsigned* sig);
int muos_ppoll(muos_pollfd_t* fds, int nfds, unsigned* ts);

int  muos_nanosleep(unsigned* req /* {sec,nsec} */, unsigned* rem);
int  muos_readlink(const char* path, char* buf, unsigned size);
int  muos_chown(const char* path, int uid, int gid);
int  muos_fchown(int fd, int uid, int gid);
int  muos_fchownat(int dirfd, const char* path, int uid, int gid, int flags);
int  muos_getrusage(int who, void* out /* 72 bytes */);
unsigned muos_times(void* out /* 16 bytes or 0 */);
int  muos_waitid(int idtype, int id, void* infop /* 128 bytes */, int options);

/* Signals and threads */
int  muos_kill(int pid, int sig);
int  muos_signal(int sig, void (*handler)(int));
int  muos_sigprocmask(int how, const unsigned* set, unsigned* old);
int  muos_alarm(unsigned seconds);
int  muos_gettid(void);
/* clone a thread: fn(arg) runs on stack_top (16-byte aligned by the
 * helper); returns the thread id to the caller and 0 inside the thread. */
int  muos_thread_start(void (*fn)(void*), void* arg, void* stack_top);

/* ---- sockets (IPv4 only; values mirror Linux i386) ---- */
#define MUOS_AF_INET      2
#define MUOS_SOCK_STREAM  1
#define MUOS_SOCK_DGRAM   2
#define MUOS_SOCK_RAW     3
#define MUOS_IPPROTO_ICMP 1

typedef struct {
    unsigned short sin_family;
    unsigned short sin_port;    /* network byte order */
    unsigned       sin_addr;    /* network byte order */
    unsigned       pad[2];
} muos_sockaddr_in_t;

int muos_socket(int domain, int type, int proto);
int muos_bind(int fd, const void* addr, int len);
int muos_listen(int fd, int backlog);
int muos_accept(int fd);
int muos_connect(int fd, const void* addr, int len);
int muos_send(int fd, const void* buf, unsigned len, int flags);
int muos_recv(int fd, void* buf, unsigned len, int flags);
int muos_sendto(int fd, const void* buf, unsigned len, int flags,
                const void* addr, int alen);
int muos_recvfrom(int fd, void* buf, unsigned len, int flags,
                  void* addr, int* alen);

void muos_puts(const char* s);
void muos_putdec(int value);
void muos_puthex(unsigned value);

#endif
