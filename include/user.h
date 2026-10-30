#ifndef MUOS_USER_H
#define MUOS_USER_H
#include "types.h"
#include "isr.h"

/* User-mode sandbox. The kernel identity-maps all RAM, so user virtual
 * addresses are backed by the physical frames at the same addresses.
 * 0x08048000 is the classic Linux i386 ET_EXEC load address, which keeps
 * the door open for real static Linux binaries later. The range is
 * claimed from the physical allocator before first use. */
#define USER_BASE  0x08048000u
#define USER_SIZE  0x00800000u           /* 8MB sandbox */
#define USER_END   (USER_BASE + USER_SIZE)
#define USER_STACK_RESERVE 0x00010000u   /* 64KB kept free for the stack */
/* Last page of the sandbox: kernel-provided `mov eax,173; int 0x80`
 * sigreturn trampoline (the Linux vDSO trick), read-only to user code. */
#define USER_SIGTRAMP (USER_END - 0x1000u)

bool_t  user_is_active(void);
bool_t  user_ptr_ok(uint32_t ptr, uint32_t len);

/* Load and run an ELF image (embedded blob or RAM FS file). Blocks until
 * the Ring3 program exits or faults, then returns to the caller. The
 * optional arguments become argv[0]/argv[1] on the initial user stack. */
int     user_run_image(const void* image, uint32_t size,
                       const char* arg0, const char* arg1);
int     user_run_image_argv(const void* image, uint32_t size,
                            int argc, const char* const* argv);
int     user_run_embedded(const char* arg0, const char* arg1);

/* Per-process fd table (fd 0/1/2 are stdin/stdout/stderr). An entry is a
 * RAM FS file, a directory (offset = next entry index), a virtual /dev
 * node, or a snapshot of a /proc file. */
#define USER_FD_MAX 11          /* fds 0..10; 0/1/2 start as stdio */
#define USER_FD_PATH_MAX 64
#define USER_FD_VBUF_MAX 512

typedef enum {
    USER_FD_FREE = 0,
    USER_FD_STDIN,
    USER_FD_STDOUT,
    USER_FD_STDERR,
    USER_FD_FILE,
    USER_FD_DIR,
    USER_FD_DEV_NULL,
    USER_FD_DEV_ZERO,
    USER_FD_DEV_CONSOLE,
    USER_FD_VFILE,
    USER_FD_PIPE_READ,
    USER_FD_PIPE_WRITE
} user_fd_kind_t;

void     user_fds_reset(void);
int      user_fd_alloc(int fs_slot);
int      user_fd_alloc_dir(const char* path);
int      user_fd_alloc_dev(int kind);
int      user_fd_alloc_vfile(const char* data, uint32_t len);
int      user_fd_alloc_pipe(int pipe_id, bool_t write_end);
int      user_fd_pipe_id(int fd);
int      user_fd_kind(int fd);
bool_t   user_fd_is_dir(int fd);
const char* user_fd_dir_path(int fd);
uint32_t user_fd_vfile(int fd, const char** data);
int      user_fd_dup(int oldfd);
bool_t   user_fd_dup2(int oldfd, int newfd);
int      user_fd_slot(int fd);
uint32_t user_fd_offset(int fd);
bool_t   user_fd_set_offset(int fd, uint32_t offset);
void     user_fd_close(int fd);

/* poll events (mirror of the Linux i386 values) */
#define USER_POLLIN   0x001
#define USER_POLLOUT  0x004
#define USER_POLLERR  0x008
#define USER_POLLHUP  0x010
#define USER_POLLNVAL 0x020

/* Readiness of one fd in a *saved* fd-table snapshot. Lets the scheduler
 * wake a blocked process whose live table is not loaded. Returns the
 * revents mask (0 = not ready, USER_POLLNVAL = bad fd). */
int      user_fd_state_events(void* state, int fd, int events);
int      user_fd_state_kind(void* state, int fd);
int      user_fd_state_pipe(void* state, int fd);

/* Single-process cwd used for relative path resolution ("/" at launch). */
const char* user_cwd_get(void);
bool_t   user_cwd_set(const char* path);

/* ---- process state save/restore (sequential fork model) ---- */
uint32_t user_fd_state_bytes(void);
void     user_fd_state_save(void* dst);
void     user_fd_state_restore(const void* src);
void     user_fd_state_retain(void* state);    /* count this snapshot's refs */
void     user_fd_state_release(void* state);   /* drop this snapshot's refs */
void     user_get_brk_state(uint32_t* base, uint32_t* cur);
bool_t   user_restore_brk_state(uint32_t base, uint32_t cur);
int      user_last_exit_code(void);
bool_t   user_last_faulted(void);
void     user_clear_exit_state(void);
void     user_set_active(bool_t v);

/* Load/replace the current process image (execve). Keeps fds and cwd. */
int      user_exec_image(const void* image, uint32_t size, int argc,
                         const char* const* argv,
                         uint32_t* entry_out, uint32_t* sp_out);

/* Saved kernel ESP of the outer user_enter(); nested entries save/restore
 * it around user_enter_regs. The launch copy is kept stable so the
 * scheduler can return to the shell when no user process is runnable. */
extern uint32_t user_kernel_esp_save;
extern uint32_t user_kernel_esp_launch;

/* Enter Ring3 with a full register set (child after fork). */
void user_enter_regs(uint32_t entry, uint32_t user_sp, uint32_t eax,
                     uint32_t ebx, uint32_t ecx, uint32_t edx,
                     uint32_t esi, uint32_t edi, uint32_t ebp,
                     uint32_t fs_sel, uint32_t gs_sel);

/* Anonymous mmap arena: page-granular bump inside the sandbox.
 * Returns 0 when the request does not fit. */
uint32_t user_mmap_anon(uint32_t len);

/* Assembly helpers in isr_stubs.s */
void user_enter(uint32_t entry, uint32_t user_stack);
void user_return_to_kernel(void) __attribute__((noreturn));

/* Syscall/fault paths that leave user mode and resume the kernel caller.
 * Both never return to their C caller. */
void    user_exit(int code) __attribute__((noreturn));
/* Returns true only when the scheduler switched the faulting process out;
 * otherwise it never returns (resumes the shell). */
bool_t  user_handle_fault(registers_t* regs);

uint32_t user_get_brk(void);
bool_t   user_set_brk(uint32_t addr);

/* For self-tests: the embedded image and its size. */
const void* user_get_image(uint32_t* size);

#endif
