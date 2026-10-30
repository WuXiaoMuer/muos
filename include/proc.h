#ifndef MUOS_PROC_H
#define MUOS_PROC_H
#include "types.h"
#include "user.h"

#define PROC_MAX       8
#define PROC_NAME_MAX  32
#define PROC_CMDLINE_MAX 96

typedef enum {
    PROC_FREE = 0,
    PROC_RUNNING,
    PROC_READY,
    PROC_ZOMBIE
} proc_state_t;

/* Sequential process model: only one address space is live at a time;
 * a forked child owns an 8MB memory snapshot plus its own kernel stack
 * and fd/cwd/brk state, and runs when the parent waits for it. */
typedef struct {
    bool_t   used;
    uint32_t pid;
    uint32_t ppid;
    proc_state_t state;
    int      exit_code;
    char     name[PROC_NAME_MAX];
    char     cmdline[PROC_CMDLINE_MAX];
    uint32_t start_ticks;

    /* Address space snapshot (USER_SIZE bytes) */
    void*    mem;
    uint32_t mem_pages;

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

#endif
