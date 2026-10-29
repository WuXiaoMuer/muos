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

/* Per-process fd table (fd 0/1/2 are stdin/stdout/stderr). File entries
 * map to RAM FS slot indices and keep a byte offset. USER_FD_DIR marks an
 * open directory (only "/" exists); its offset is the next entry index. */
#define USER_FD_MAX 8
#define USER_FD_DIR (-2)
void     user_fds_reset(void);
int      user_fd_alloc(int fs_slot);
int      user_fd_alloc_dir(void);
bool_t   user_fd_is_dir(int fd);
bool_t   user_stdio_is_open(int fd);
void     user_stdio_close(int fd);
int      user_fd_slot(int fd);
uint32_t user_fd_offset(int fd);
bool_t   user_fd_set_offset(int fd, uint32_t offset);
void     user_fd_close(int fd);

/* Anonymous mmap arena: page-granular bump inside the sandbox.
 * Returns 0 when the request does not fit. */
uint32_t user_mmap_anon(uint32_t len);

/* Assembly helpers in isr_stubs.s */
void user_enter(uint32_t entry, uint32_t user_stack);
void user_return_to_kernel(void) __attribute__((noreturn));

/* Syscall/fault paths that leave user mode and resume the kernel caller.
 * Both never return to their C caller. */
void    user_exit(int code) __attribute__((noreturn));
void    user_handle_fault(registers_t* regs) __attribute__((noreturn));

uint32_t user_get_brk(void);
bool_t   user_set_brk(uint32_t addr);

/* For self-tests: the embedded image and its size. */
const void* user_get_image(uint32_t* size);

#endif
