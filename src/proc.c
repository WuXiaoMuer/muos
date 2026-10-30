#include "proc.h"
#include "mm.h"
#include "kheap.h"
#include "pit.h"
#include "tss.h"
#include "gdt.h"
#include "string.h"

static proc_t procs[PROC_MAX];
static proc_t* current_proc = NULL;
static uint32_t next_pid = 2;   /* pid 1 is the kernel task */

static uint32_t kernel_stack_pages(void) {
    return (TSS_KERNEL_STACK_PAGES * 4096u) / PAGE_SIZE;
}

void proc_init(void) {
    memset(procs, 0, sizeof(procs));
    current_proc = NULL;
    next_pid = 2;

    proc_t* k = &procs[0];
    k->used = true;
    k->pid = 1;
    k->ppid = 0;
    k->state = PROC_RUNNING;
    k->exit_code = 0;
    strncpy(k->name, "kernel", PROC_NAME_MAX - 1);
    strncpy(k->cmdline, "kernel", PROC_CMDLINE_MAX - 1);
    current_proc = k;
}

proc_t* proc_current(void) {
    return current_proc;
}

void proc_set_current(proc_t* p) {
    current_proc = p;
}

proc_t* proc_find(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    }
    return NULL;
}

proc_t* proc_get_index(int idx) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!procs[i].used) continue;
        if (n == idx) return &procs[i];
        n++;
    }
    return NULL;
}

int proc_count(void) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) if (procs[i].used) n++;
    return n;
}

proc_t* proc_alloc(const char* name, uint32_t ppid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (procs[i].used) continue;
        memset(&procs[i], 0, sizeof(procs[i]));
        procs[i].used = true;
        procs[i].pid = next_pid++;
        procs[i].ppid = ppid;
        procs[i].state = PROC_READY;
        procs[i].start_ticks = pit_get_ticks();
        procs[i].mem_pages = USER_SIZE / PAGE_SIZE;
        procs[i].fs_sel = 0x23;
        procs[i].gs_sel = 0x23;
        strncpy(procs[i].name, name ? name : "app", PROC_NAME_MAX - 1);
        strncpy(procs[i].cmdline, name ? name : "app", PROC_CMDLINE_MAX - 1);

        void* ks = mm_alloc_pages(kernel_stack_pages());
        if (!ks) {
            procs[i].used = false;
            return NULL;
        }
        procs[i].kstack = ks;
        procs[i].kstack_top = (uint32_t)ks + TSS_KERNEL_STACK_PAGES * 4096u;

        /* Private 8MB physical block + page directory mapping it at USER_BASE. */
        void* block = mm_alloc_pages(USER_SIZE / PAGE_SIZE);
        if (!block) {
            mm_free_pages(ks, kernel_stack_pages());
            procs[i].used = false;
            return NULL;
        }
        uint32_t pd = mm_create_user_pd((uint32_t)block);
        if (!pd) {
            mm_free_pages(block, USER_SIZE / PAGE_SIZE);
            mm_free_pages(ks, kernel_stack_pages());
            procs[i].used = false;
            return NULL;
        }
        procs[i].mem = block;
        procs[i].mem_pages = USER_SIZE / PAGE_SIZE;
        procs[i].pd_phys = pd;
        return &procs[i];
    }
    return NULL;
}

bool_t proc_snapshot_alloc(proc_t* p) {
    if (!p) return false;
    if (p->mem && p->pd_phys) return true;
    if (!p->mem) {
        p->mem = mm_alloc_pages(USER_SIZE / PAGE_SIZE);
        if (!p->mem) return false;
        p->mem_pages = USER_SIZE / PAGE_SIZE;
    }
    if (!p->pd_phys) {
        p->pd_phys = mm_create_user_pd((uint32_t)p->mem);
        if (!p->pd_phys) {
            mm_free_pages(p->mem, p->mem_pages);
            p->mem = NULL;
            p->mem_pages = 0;
            return false;
        }
    }
    return true;
}

void proc_snapshot_free(proc_t* p) {
    if (!p) return;
    if (p->pd_phys) {
        mm_free_user_pd(p->pd_phys);
        p->pd_phys = 0;
    }
    if (p->mem) {
        mm_free_pages(p->mem, p->mem_pages);
        p->mem = NULL;
        p->mem_pages = 0;
    }
}

void proc_switch_space(proc_t* p) {
    if (p && p->pd_phys) {
        mm_switch_pd(p->pd_phys);
        tss_set_esp0(p->kstack_top);
    } else {
        mm_switch_pd(0);                       /* kernel page directory */
        tss_set_esp0(tss_get_kernel_stack_top());
    }
}

/* ---- preemptive round-robin scheduler ----
 * A switch never copies process memory: each process has its own page
 * directory and 8MB block. The interrupt/syscall frame is rewritten in
 * place, so the normal assembly epilogue iret's into the next process.
 * Kernel (pid 1, the shell) is never scheduled from Ring3. */

static bool_t frame_switched = false;

bool_t proc_frame_switched(void) { return frame_switched; }
void   proc_clear_frame_switched(void) { frame_switched = false; }

static proc_t* sched_pick_next(proc_t* cur) {
    int start = 0;
    if (cur) {
        for (int i = 0; i < PROC_MAX; i++) {
            if (&procs[i] == cur) { start = i + 1; break; }
        }
    }
    for (int n = 0; n < PROC_MAX; n++) {
        int i = (start + n) % PROC_MAX;
        if (procs[i].used && procs[i].pid != 1 &&
            procs[i].state == PROC_READY && procs[i].has_ctx)
            return &procs[i];
    }
    return NULL;
}

static void proc_save_ctx(proc_t* p, const registers_t* regs) {
    p->eip = regs->eip;
    p->esp = regs->useresp;
    p->eflags = regs->eflags;
    p->eax = regs->eax;
    p->ebx = regs->ebx;
    p->ecx = regs->ecx;
    p->edx = regs->edx;
    p->esi = regs->esi;
    p->edi = regs->edi;
    p->ebp = regs->ebp;
    p->fs_sel = regs->fs & 0xFFFF;
    p->gs_sel = regs->gs & 0xFFFF;
    p->has_ctx = true;
}

static void proc_save_state(proc_t* p) {
    if (p->fd_state) user_fd_state_save(p->fd_state);
    strncpy(p->cwd, user_cwd_get(), USER_FD_PATH_MAX - 1);
    p->cwd[USER_FD_PATH_MAX - 1] = '\0';
    user_get_brk_state(&p->brk_base, &p->brk_cur);
    gdt_get_user_tls(&p->tls_base, &p->tls_limit, &p->tls_flags);
}

static void sched_load(proc_t* p, registers_t* regs) {
    proc_switch_space(p);
    if (p->fd_state) user_fd_state_restore(p->fd_state);
    user_cwd_set(p->cwd);
    user_restore_brk_state(p->brk_base, p->brk_cur);
    gdt_set_user_tls(p->tls_base, p->tls_limit, p->tls_flags);
    proc_set_current(p);
    p->state = PROC_RUNNING;
    user_set_active(true);

    regs->eip = p->eip;
    regs->useresp = p->esp;
    regs->eflags = (p->eflags | 0x200u) & ~0x3000u;
    regs->eax = p->eax;
    regs->ebx = p->ebx;
    regs->ecx = p->ecx;
    regs->edx = p->edx;
    regs->esi = p->esi;
    regs->edi = p->edi;
    regs->ebp = p->ebp;
    regs->cs = 0x1B;
    regs->ss = 0x23;
    regs->ds = 0x23;
    regs->es = 0x23;
    regs->fs = p->fs_sel ? p->fs_sel : 0x23;
    regs->gs = p->gs_sel ? p->gs_sel : 0x23;
    frame_switched = true;
}

/* Write a 32-bit value into a (possibly blocked) process's user memory
 * through its physical block, since its page directory is not active. */
static void proc_write_user_u32(proc_t* p, uint32_t uptr, uint32_t value) {
    if (!p || !p->mem) return;
    if (uptr < USER_BASE || uptr > USER_END - 4) return;
    uint32_t phys = (uint32_t)p->mem + (uptr - USER_BASE);
    *(uint32_t*)phys = value;
}

static void proc_wake_parent(proc_t* child) {
    proc_t* parent = proc_find(child->ppid);
    if (!parent || parent->state != PROC_BLOCKED) return;
    if (parent->wait_pid > 0 && (uint32_t)parent->wait_pid != child->pid) return;

    if (parent->wait_kind == PROC_WAIT_WAIT4) {
        if (parent->wait_status_ptr)
            proc_write_user_u32(parent, parent->wait_status_ptr,
                                (uint32_t)(child->exit_code & 0xFF) << 8);
        parent->eax = child->pid;
    } else if (parent->wait_kind == PROC_WAIT_WAITID) {
        uint32_t base = parent->wait_infop;
        proc_write_user_u32(parent, base + 0, 17);                 /* SIGCHLD */
        proc_write_user_u32(parent, base + 4, 0);
        proc_write_user_u32(parent, base + 8, 1);                  /* CLD_EXITED */
        proc_write_user_u32(parent, base + 12, child->pid);
        proc_write_user_u32(parent, base + 16, 0);
        proc_write_user_u32(parent, base + 20,
                            (uint32_t)(child->exit_code & 0xFF));
        parent->eax = 0;
    }
    parent->wait_kind = PROC_WAIT_NONE;
    parent->state = PROC_READY;
}

void proc_sched_tick(registers_t* regs) {
    proc_t* cur = proc_current();
    if (!cur || cur->pid == 1) return;
    if ((regs->cs & 3) != 3) return;
    if (cur->state != PROC_RUNNING) return;
    if (!proc_fdstate_alloc(cur)) return;

    proc_save_ctx(cur, regs);
    proc_save_state(cur);
    cur->state = PROC_READY;

    proc_t* next = sched_pick_next(cur);
    if (!next || next == cur) {
        cur->state = PROC_RUNNING;
        return;
    }
    sched_load(next, regs);
}

bool_t proc_block_current(registers_t* regs, proc_wait_t kind,
                          int32_t pid, uint32_t status_ptr) {
    proc_t* cur = proc_current();
    if (!cur || cur->pid == 1) return false;
    if (!proc_fdstate_alloc(cur)) return false;

    proc_save_ctx(cur, regs);
    proc_save_state(cur);
    cur->wait_kind = kind;
    cur->wait_pid = pid;
    cur->wait_status_ptr = status_ptr;
    cur->wait_infop = status_ptr;      /* waitid passes the infop here */
    cur->state = PROC_BLOCKED;
    user_set_active(false);

    proc_t* next = sched_pick_next(cur);
    if (!next) {
        cur->state = PROC_RUNNING;
        user_set_active(true);
        return false;
    }
    sched_load(next, regs);
    return true;
}

void proc_exit_current(registers_t* regs, int code, bool_t faulted) {
    proc_t* cur = proc_current();
    if (!cur || cur->pid == 1) {
        user_set_active(false);
        user_return_to_kernel();
    }

    if (cur->fd_state) user_fd_state_save(cur->fd_state);
    cur->exit_code = code;
    cur->faulted = faulted;
    cur->state = PROC_ZOMBIE;
    cur->has_ctx = false;
    user_set_active(false);
    proc_wake_parent(cur);

    proc_t* next = sched_pick_next(NULL);
    if (next) {
        sched_load(next, regs);
        return;                 /* asm epilogue iret's into next */
    }

    /* Nothing runnable: resume the shell that called user_enter(). */
    proc_switch_space(NULL);
    proc_set_current(proc_find(1));
    user_kernel_esp_save = user_kernel_esp_launch;
    user_return_to_kernel();
    for (;;) { /* not reached */ }
}

bool_t proc_fdstate_alloc(proc_t* p) {
    if (!p) return false;
    if (p->fd_state) return true;
    p->fd_state = kmalloc(user_fd_state_bytes());
    return p->fd_state ? true : false;
}

void proc_reap(proc_t* p) {
    if (!p || !p->used) return;
    proc_snapshot_free(p);
    if (p->fds_retained && p->fd_state) user_fd_state_release(p->fd_state);
    if (p->fd_state) kfree(p->fd_state);
    if (p->kstack) mm_free_pages(p->kstack, kernel_stack_pages());
    if (current_proc == p) current_proc = proc_find(1);
    memset(p, 0, sizeof(*p));
}

void proc_cleanup_children(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (!procs[i].used || procs[i].pid == pid) continue;
        if (procs[i].ppid == pid) {
            proc_cleanup_children(procs[i].pid);
            proc_reap(&procs[i]);
        }
    }
}

char proc_state_char(const proc_t* p) {
    if (!p) return '?';
    switch (p->state) {
        case PROC_RUNNING: return 'R';
        case PROC_READY:   return 'S';
        case PROC_ZOMBIE:  return 'Z';
        default:           return '?';
    }
}
