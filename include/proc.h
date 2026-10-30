#ifndef MUOS_PROC_H
#define MUOS_PROC_H
#include "types.h"
#include "user.h"
#include "isr.h"

#define PROC_MAX       8
#define PROC_NAME_MAX  32
#define PROC_CMDLINE_MAX 96

#define PROC_NSIG          32
#define PROC_SIG_DFL       0u
#define PROC_SIG_IGN       1u
#define PROC_SIGFRAME_MAX  4    /* nested signal frames kept per process */

typedef enum {
    PROC_FREE = 0,
    PROC_RUNNING,
    PROC_READY,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

typedef enum {
    PROC_WAIT_NONE = 0,
    PROC_WAIT_WAIT4,
    PROC_WAIT_WAITID,
    PROC_WAIT_POLL,      /* blocked in poll() with a deadline */
    PROC_WAIT_SIGSUSP,   /* blocked until a signal is delivered */
    PROC_WAIT_READ       /* blocked in read() on a pipe/console */
} proc_wait_t;

/* Refcounted address space: a thread group shares one block + page
 * directory; a plain process owns its own. Freed when refs hits 0. */
typedef struct {
    void*    mem;          /* block physical base (identity-addressable) */
    uint32_t mem_pages;
    uint32_t pd_phys;      /* private page directory (0 = kernel) */
    uint32_t tramp_phys;   /* sigreturn trampoline code page */
    uint32_t refs;
} proc_space_t;

/* Saved fd table header (refcounted so CLONE_FILES threads share one). */
typedef struct {
    uint32_t refs;
} proc_fdstate_t;

/* A process owns a private 8MB physical block mapped at USER_BASE by its
 * own page directory, or shares one with its thread group. The preemptive
 * scheduler saves/restores its register context, fd table, cwd, brk and
 * TLS base on every switch. */
typedef struct {
    bool_t   used;
    uint32_t pid;
    uint32_t ppid;
    uint32_t tgid;          /* thread group id (== pid for the leader) */
    proc_state_t state;
    int      exit_code;
    bool_t   faulted;
    bool_t   is_thread;     /* CLONE_THREAD: shares the address space */
    char     name[PROC_NAME_MAX];
    char     cmdline[PROC_CMDLINE_MAX];
    uint32_t start_ticks;

    /* Address space (refcounted, shared by CLONE_VM/CLONE_THREAD) */
    proc_space_t* space;
    void*    mem;          /* cached space->mem */
    uint32_t mem_pages;
    uint32_t pd_phys;

    /* Kernel stack used by Ring3 -> Ring0 transitions for this process */
    void*    kstack;
    uint32_t kstack_top;

    /* Saved user-process state */
    void*    fd_state;       /* proc_fdstate_t header + user fd table */
    bool_t   fds_retained;   /* fd_state counts file/pipe references */
    char     cwd[USER_FD_PATH_MAX];
    uint32_t brk_base, brk_cur;
    uint32_t tls_base, tls_limit, tls_flags;   /* GDT slot 6 per process */

    /* Registers to resume at (children start after the parent's fork) */
    uint32_t eip, esp, eflags;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t fs_sel, gs_sel;
    bool_t   has_ctx;

    /* Blocked wait bookkeeping (wait4/waitid/poll/sigsuspend). */
    proc_wait_t wait_kind;
    int32_t  wait_pid;
    uint32_t wait_status_ptr;
    uint32_t wait_infop;
    uint32_t blk_ptr;          /* poll fds array / user buffer */
    int32_t  blk_len;          /* nfds / byte count */
    int32_t  blk_fd;           /* read() fd for PROC_WAIT_READ */
    uint32_t blk_deadline;     /* absolute tick deadline */
    bool_t   blk_has_deadline;

    /* Signal state */
    uint32_t sig_pending;      /* pending signal bitmask */
    uint32_t sig_blocked;      /* blocked signal bitmask */
    uint32_t sig_handler[PROC_NSIG];
    uint32_t sig_flags[PROC_NSIG];
    uint32_t sig_restorer[PROC_NSIG];
    uint32_t sig_frame_sp[PROC_SIGFRAME_MAX];
    int      sig_depth;
    uint32_t alarm_tick;       /* absolute tick for SIGALRM, 0 = disarmed */
} proc_t;

void     proc_init(void);
proc_t*  proc_alloc(const char* name, uint32_t ppid);
proc_t*  proc_current(void);
void     proc_set_current(proc_t* p);
proc_t*  proc_find(uint32_t pid);
proc_t*  proc_get_index(int idx);
int      proc_count(void);
void     proc_reap(proc_t* p);
void     proc_cleanup_children(uint32_t pid);
char     proc_state_char(const proc_t* p);

/* Address space helpers (return false on allocation failure) */
bool_t   proc_snapshot_alloc(proc_t* p);
bool_t   proc_space_share(proc_t* child, proc_t* parent);
void     proc_snapshot_free(proc_t* p);

/* fd table helpers */
bool_t   proc_fdstate_alloc(proc_t* p);
bool_t   proc_fdstate_share(proc_t* child, proc_t* parent);
void     proc_fdstate_free(proc_t* p);

/* Switch CR3 and TSS.esp0 to this process (NULL/pid1 = kernel space). */
void     proc_switch_space(proc_t* p);

/* Preemptive scheduler entry points. All of them may rewrite the interrupt
 * frame so the normal asm epilogue iret's into another process. */
void     proc_sched_tick(registers_t* regs);
bool_t   proc_block_current(registers_t* regs, proc_wait_t kind,
                            int32_t pid, uint32_t status_ptr);
bool_t   proc_block_current_ext(registers_t* regs, proc_wait_t kind,
                                uint32_t ptr, int32_t len,
                                uint32_t deadline, bool_t has_deadline,
                                int32_t fd);
void     proc_exit_current(registers_t* regs, int code, bool_t faulted);
bool_t   proc_frame_switched(void);
void     proc_clear_frame_switched(void);

/* Timer tick: alarm expiries, poll timeouts (walks all processes). */
void     proc_tick_all(void);

/* ---- signals ---- */
void     proc_signal_reset(proc_t* p);
void     proc_signal_inherit(proc_t* child, proc_t* parent);
int      proc_signal_send(uint32_t pid, int sig);       /* < 0 = -errno */
bool_t   proc_signal_pending(void);                     /* current, deliverable */
uint32_t proc_signal_set_action(proc_t* p, int sig, uint32_t handler,
                                uint32_t flags, uint32_t restorer);
uint32_t proc_signal_get_action(proc_t* p, int sig, uint32_t* flags,
                                uint32_t* restorer);
bool_t   proc_sigreturn(registers_t* regs);             /* restore a frame */
/* Deliver pending signals to the current process by rewriting its frame.
 * Returns true when the frame was changed or the process was killed. */
bool_t   proc_signal_deliver(registers_t* regs);

/* Cross-process wakeups for blocking I/O. Safe from IRQ context. */
void     proc_notify_console(void);
void     proc_notify_pipe(int pipe_id);

#endif
