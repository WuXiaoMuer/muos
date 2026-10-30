#ifndef MUOS_PROC_H
#define MUOS_PROC_H
#include "types.h"
#include "user.h"
#include "isr.h"

#define PROC_MAX       8
#define PROC_NAME_MAX  32
#define PROC_CMDLINE_MAX 96

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
    PROC_WAIT_WAITID
} proc_wait_t;

/* A process owns a private 8MB physical block mapped at USER_BASE by its
 * own page directory. The preemptive scheduler saves/restores its register
 * context, fd table, cwd, brk and TLS base on every switch. */
typedef struct {
    bool_t   used;
    uint32_t pid;
    uint32_t ppid;
    proc_state_t state;
    int      exit_code;
    bool_t   faulted;
    char     name[PROC_NAME_MAX];
    char     cmdline[PROC_CMDLINE_MAX];
    uint32_t start_ticks;

    /* Address space: 8MB physical block + private page directory */
    void*    mem;          /* block physical base (identity-addressable) */
    uint32_t mem_pages;
    uint32_t pd_phys;      /* page directory physical address (0 = kernel) */

    /* Kernel stack used by Ring3 -> Ring0 transitions for this process */
    void*    kstack;
    uint32_t kstack_top;

    /* Saved user-process state */
    void*    fd_state;
    bool_t   fds_retained;   /* fd_state counts file/pipe references */
    char     cwd[USER_FD_PATH_MAX];
    uint32_t brk_base, brk_cur;
    uint32_t tls_base, tls_limit, tls_flags;   /* GDT slot 6 per process */

    /* Registers to resume at (children start after the parent's fork) */
    uint32_t eip, esp, eflags;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t fs_sel, gs_sel;
    bool_t   has_ctx;

    /* Blocked wait bookkeeping (wait4/waitid). */
    proc_wait_t wait_kind;
    int32_t  wait_pid;
    uint32_t wait_status_ptr;
    uint32_t wait_infop;
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

/* Snapshot/state helpers (return false on allocation failure) */
bool_t   proc_snapshot_alloc(proc_t* p);
void     proc_snapshot_free(proc_t* p);
bool_t   proc_fdstate_alloc(proc_t* p);

/* Switch CR3 and TSS.esp0 to this process (NULL/pid1 = kernel space). */
void     proc_switch_space(proc_t* p);

/* Preemptive scheduler entry points. All of them may rewrite the interrupt
 * frame so the normal asm epilogue iret's into another process. */
void     proc_sched_tick(registers_t* regs);
bool_t   proc_block_current(registers_t* regs, proc_wait_t kind,
                            int32_t pid, uint32_t status_ptr);
void     proc_exit_current(registers_t* regs, int code, bool_t faulted);
bool_t   proc_frame_switched(void);
void     proc_clear_frame_switched(void);

#endif
