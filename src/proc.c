#include "proc.h"
#include "mm.h"
#include "kheap.h"
#include "pit.h"
#include "tss.h"
#include "gdt.h"
#include "string.h"
#include "pipe.h"
#include "keyboard.h"

static proc_t procs[PROC_MAX];
static proc_t* current_proc = NULL;
static uint32_t next_pid = 2;   /* pid 1 is the kernel task */

static uint32_t kernel_stack_pages(void) {
    return (TSS_KERNEL_STACK_PAGES * 4096u) / PAGE_SIZE;
}

/* ---- refcounted address space ---- */

static void proc_space_unref(proc_space_t* s) {
    if (!s) return;
    if (s->refs > 1) { s->refs--; return; }
    if (s->pd_phys)   mm_free_user_pd(s->pd_phys);
    if (s->mem)       mm_free_pages(s->mem, s->mem_pages);
    if (s->tramp_phys) mm_free_page((void*)s->tramp_phys);
    kfree(s);
}

/* Fill the sigreturn trampoline page: mov eax,173 (rt_sigreturn); int 0x80 */
static void write_sig_trampoline(uint32_t phys) {
    uint8_t* t = (uint8_t*)phys;
    memset(t, 0, PAGE_SIZE);
    t[0] = 0xB8;              /* mov eax, imm32 */
    t[1] = 173; t[2] = 0; t[3] = 0; t[4] = 0;
    t[5] = 0xCD; t[6] = 0x80;
}

static bool_t proc_space_alloc(proc_t* p) {
    if (!p) return false;
    if (p->space) return true;
    proc_space_t* s = kmalloc(sizeof(*s));
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    s->mem_pages = USER_SIZE / PAGE_SIZE;
    s->mem = mm_alloc_pages(s->mem_pages);
    if (!s->mem) { kfree(s); return false; }
    s->tramp_phys = (uint32_t)mm_alloc_page();
    if (!s->tramp_phys) {
        mm_free_pages(s->mem, s->mem_pages);
        kfree(s);
        return false;
    }
    write_sig_trampoline(s->tramp_phys);
    s->pd_phys = mm_create_user_pd((uint32_t)s->mem, s->tramp_phys);
    if (!s->pd_phys) {
        mm_free_page((void*)s->tramp_phys);
        mm_free_pages(s->mem, s->mem_pages);
        kfree(s);
        return false;
    }
    s->refs = 1;
    p->space = s;
    p->mem = s->mem;
    p->mem_pages = s->mem_pages;
    p->pd_phys = s->pd_phys;
    return true;
}

bool_t proc_space_share(proc_t* child, proc_t* parent) {
    if (!child || !parent || !parent->space) return false;
    child->space = parent->space;
    parent->space->refs++;
    child->mem = parent->mem;
    child->mem_pages = parent->mem_pages;
    child->pd_phys = parent->pd_phys;
    return true;
}

/* ---- refcounted fd table ---- */

static proc_fdstate_t* fdstate_hdr(void* state) {
    return ((proc_fdstate_t*)state) - 1;
}

bool_t proc_fdstate_alloc(proc_t* p) {
    if (!p) return false;
    if (p->fd_state) return true;
    uint32_t bytes = user_fd_state_bytes();
    if (bytes == 0) return false;
    proc_fdstate_t* h = kmalloc(sizeof(proc_fdstate_t) + bytes);
    if (!h) return false;
    h->refs = 1;
    p->fd_state = (void*)(h + 1);
    return true;
}

bool_t proc_fdstate_share(proc_t* child, proc_t* parent) {
    if (!child || !parent || !parent->fd_state) return false;
    child->fd_state = parent->fd_state;
    fdstate_hdr(parent->fd_state)->refs++;
    child->fds_retained = parent->fds_retained;
    return true;
}

void proc_fdstate_free(proc_t* p) {
    if (!p || !p->fd_state) return;
    proc_fdstate_t* h = fdstate_hdr(p->fd_state);
    if (h->refs > 1) {
        h->refs--;
    } else {
        if (p->fds_retained) user_fd_state_release(p->fd_state);
        kfree(h);
    }
    p->fd_state = NULL;
}

void proc_init(void) {
    memset(procs, 0, sizeof(procs));
    current_proc = NULL;
    next_pid = 2;

    proc_t* k = &procs[0];
    k->used = true;
    k->pid = 1;
    k->tgid = 1;
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
        procs[i].tgid = procs[i].pid;
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
        return &procs[i];
    }
    return NULL;
}

bool_t proc_snapshot_alloc(proc_t* p) {
    return proc_space_alloc(p);
}

void proc_snapshot_free(proc_t* p) {
    if (!p || !p->space) return;
    proc_space_unref(p->space);
    p->space = NULL;
    p->mem = NULL;
    p->mem_pages = 0;
    p->pd_phys = 0;
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
 * directory and 8MB block (threads share both). The interrupt/syscall
 * frame is rewritten in place, so the normal assembly epilogue iret's
 * into the next process. Kernel (pid 1, the shell) is never scheduled
 * from Ring3. */

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
    if (!p->is_thread) user_get_brk_state(&p->brk_base, &p->brk_cur);
    gdt_get_user_tls(&p->tls_base, &p->tls_limit, &p->tls_flags);
}

static void sched_load(proc_t* p, registers_t* regs) {
    proc_switch_space(p);
    if (p->fd_state) user_fd_state_restore(p->fd_state);
    user_cwd_set(p->cwd);
    if (!p->is_thread) user_restore_brk_state(p->brk_base, p->brk_cur);
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

/* ---- signals ---- */

static bool_t proc_sig_default_terminates(int sig) {
    switch (sig) {
        case 1:  /* SIGHUP */
        case 2:  /* SIGINT */
        case 3:  /* SIGQUIT */
        case 4:  /* SIGILL */
        case 5:  /* SIGTRAP */
        case 6:  /* SIGABRT */
        case 7:  /* SIGBUS */
        case 8:  /* SIGFPE */
        case 9:  /* SIGKILL */
        case 10: /* SIGUSR1 */
        case 11: /* SIGSEGV */
        case 12: /* SIGUSR2 */
        case 13: /* SIGPIPE */
        case 14: /* SIGALRM */
        case 15: /* SIGTERM */
        case 16: /* SIGSTKFLT */
        case 24: /* SIGXCPU */
        case 25: /* SIGXFSZ */
        case 26: /* SIGVTALRM */
        case 27: /* SIGPROF */
        case 29: /* SIGIO */
        case 30: /* SIGPWR */
        case 31: /* SIGSYS */
            return true;
        default:
            return false;   /* SIGCHLD/SIGCONT/SIGTSTP/WINCH... ignored */
    }
}

void proc_signal_reset(proc_t* p) {
    if (!p) return;
    p->sig_pending = 0;
    p->sig_blocked = 0;
    p->sig_depth = 0;
    p->alarm_tick = 0;
    for (int i = 0; i < PROC_NSIG; i++) {
        p->sig_handler[i] = PROC_SIG_DFL;
        p->sig_flags[i] = 0;
        p->sig_restorer[i] = 0;
    }
}

void proc_signal_inherit(proc_t* child, proc_t* parent) {
    if (!child || !parent) return;
    proc_signal_reset(child);
    for (int i = 0; i < PROC_NSIG; i++) {
        child->sig_handler[i] = parent->sig_handler[i];
        child->sig_flags[i] = parent->sig_flags[i];
        child->sig_restorer[i] = parent->sig_restorer[i];
    }
    child->sig_blocked = parent->sig_blocked;
}

int proc_signal_send(uint32_t pid, int sig) {
    if (sig < 0 || sig >= PROC_NSIG) return -22;   /* EINVAL */

    if (sig == 0) {
        if (pid == 0 || pid == (uint32_t)-1) return 0;
        return proc_find(pid) ? 0 : -3;            /* ESRCH */
    }

    uint32_t bit = 1u << sig;
    if (pid == 0) {
        proc_t* p = proc_current();
        if (!p || p->pid == 1) return -3;
        p->sig_pending |= bit;
        return 0;
    }
    if (pid == (uint32_t)-1) {
        int hit = 0;
        for (int i = 0; i < PROC_MAX; i++) {
            proc_t* p = &procs[i];
            if (!p->used || p->pid == 1 || p->state == PROC_ZOMBIE) continue;
            p->sig_pending |= bit;
            if (p->state == PROC_BLOCKED) p->state = PROC_READY;
            hit = 1;
        }
        return hit ? 0 : -3;
    }

    proc_t* t = proc_find(pid);
    if (!t) return -3;
    if (t->pid == 1) return -1;                    /* EPERM */
    if (t->state == PROC_ZOMBIE) return 0;
    t->sig_pending |= bit;
    /* Every blocking point in MuOS is interruptible: a signal wakes it
     * with EINTR in eax; the handler runs at the next tick/syscall exit. */
    if (t->state == PROC_BLOCKED) {
        if (t->wait_kind != PROC_WAIT_NONE) {
            t->eax = (uint32_t)-4;              /* -EINTR */
            t->wait_kind = PROC_WAIT_NONE;
            t->blk_has_deadline = false;
        }
        t->state = PROC_READY;
    }
    return 0;
}

bool_t proc_signal_pending(void) {
    proc_t* p = proc_current();
    if (!p || p->pid == 1) return false;
    return (p->sig_pending & ~p->sig_blocked) ? true : false;
}

uint32_t proc_signal_set_action(proc_t* p, int sig, uint32_t handler,
                                uint32_t flags, uint32_t restorer) {
    if (!p || sig <= 0 || sig >= PROC_NSIG) return 22;      /* EINVAL */
    if (sig == 9 || sig == 19) return 22;                   /* cannot catch */
    uint32_t old = p->sig_handler[sig];
    p->sig_handler[sig] = handler;
    p->sig_flags[sig] = flags;
    p->sig_restorer[sig] = restorer;
    if (handler == PROC_SIG_DFL || handler == PROC_SIG_IGN)
        p->sig_pending &= ~(1u << sig);
    return old;
}

uint32_t proc_signal_get_action(proc_t* p, int sig, uint32_t* flags,
                                uint32_t* restorer) {
    if (!p || sig <= 0 || sig >= PROC_NSIG) return 22;
    if (flags) *flags = p->sig_flags[sig];
    if (restorer) *restorer = p->sig_restorer[sig];
    return p->sig_handler[sig];
}

/* Rewrite the interrupt frame so the asm epilogue returns to handler. */
bool_t proc_signal_deliver(registers_t* regs) {
    proc_t* p = proc_current();
    if (!p || p->pid == 1) return false;
    if ((regs->cs & 3) != 3) return false;

    for (;;) {
        /* Blocked pending signals must stay pending, not be dropped. */
        uint32_t ready = p->sig_pending & ~p->sig_blocked;
        int sig = 0;
        for (int i = 1; i < PROC_NSIG && ready; i++) {
            if (ready & (1u << i)) { sig = i; break; }
        }
        if (sig == 0) break;
        p->sig_pending &= ~(1u << sig);

        uint32_t h = p->sig_handler[sig];
        if (h == PROC_SIG_IGN) continue;
        if (h == PROC_SIG_DFL) {
            if (proc_sig_default_terminates(sig)) {
                proc_exit_current(regs, 128 + sig, false);
                return true;
            }
            continue;
        }
        if (p->sig_depth >= PROC_SIGFRAME_MAX) {
            proc_exit_current(regs, 128 + sig, true);
            return true;
        }

        uint32_t base = regs->useresp - 48;    /* 12 saved dwords */
        if (base < USER_BASE || base >= USER_SIGTRAMP) {
            proc_exit_current(regs, 139, true);
            return true;
        }
        uint32_t* f = (uint32_t*)base;
        f[0]  = regs->eax;
        f[1]  = regs->ebx;
        f[2]  = regs->ecx;
        f[3]  = regs->edx;
        f[4]  = regs->esi;
        f[5]  = regs->edi;
        f[6]  = regs->ebp;
        f[7]  = regs->useresp;
        f[8]  = regs->eflags;
        f[9]  = regs->fs;
        f[10] = regs->gs;
        f[11] = regs->eip;

        uint32_t restorer = 0;
        if ((p->sig_flags[sig] & 0x04000000u) && p->sig_restorer[sig])
            restorer = p->sig_restorer[sig];

        uint32_t arg_slot = base - 4;          /* handler's (int sig) */
        uint32_t ret_slot = base - 8;          /* return -> sigreturn */
        *(uint32_t*)arg_slot = (uint32_t)sig;
        *(uint32_t*)ret_slot = restorer ? restorer : USER_SIGTRAMP;

        p->sig_frame_sp[p->sig_depth++] = base;

        regs->eip = h;
        regs->useresp = ret_slot;
        regs->eax = (uint32_t)sig;
        return true;
    }
    return false;
}

bool_t proc_sigreturn(registers_t* regs) {
    proc_t* p = proc_current();
    if (!p || p->sig_depth <= 0) return false;
    uint32_t base = p->sig_frame_sp[--p->sig_depth];
    if (base < USER_BASE || base >= USER_SIGTRAMP) return false;

    const uint32_t* f = (const uint32_t*)base;
    regs->eax     = f[0];
    regs->ebx     = f[1];
    regs->ecx     = f[2];
    regs->edx     = f[3];
    regs->esi     = f[4];
    regs->edi     = f[5];
    regs->ebp     = f[6];
    regs->useresp = f[7];
    regs->eflags  = (f[8] | 0x200u) & ~0x3000u;
    regs->fs      = f[9] ? (f[9] & 0xFFFF) : 0x23;
    regs->gs      = f[10] ? (f[10] & 0xFFFF) : 0x23;
    regs->eip     = f[11];
    regs->cs = 0x1B;
    regs->ss = 0x23;
    regs->ds = 0x23;
    regs->es = 0x23;
    return true;
}

/* ---- blocking / scheduling ---- */

/* A blocked process's page directory is not active: user pointers are
 * translated through its physical block (identity-addressable). */
static uint8_t* proc_user_ptr(proc_t* p, uint32_t uptr) {
    if (!p || !p->mem || uptr < USER_BASE || uptr >= USER_END) return NULL;
    return (uint8_t*)((uint32_t)p->mem + (uptr - USER_BASE));
}

/* Evaluate a blocked poller's fd array through its saved fd table and
 * write the revents fields back into its user memory. */
static int proc_poll_scan(proc_t* p) {
    if (!p->fd_state || p->blk_len <= 0 || p->blk_len > 64) return 0;
    int ready = 0;
    for (int i = 0; i < p->blk_len; i++) {
        uint8_t* e = proc_user_ptr(p, p->blk_ptr + (uint32_t)i * 8);
        if (!e) continue;
        int fd;
        int16_t events, rev = 0;
        memcpy(&fd, e, 4);
        memcpy(&events, e + 4, 2);
        if (fd >= 0)
            rev = (int16_t)user_fd_state_events(p->fd_state, fd, (int)events);
        memcpy(e + 6, &rev, 2);
        if (rev) ready++;
    }
    return ready;
}

static void proc_poll_finish(proc_t* p, int ready) {
    p->eax = (uint32_t)ready;
    p->wait_kind = PROC_WAIT_NONE;
    p->blk_has_deadline = false;
    p->state = PROC_READY;
}

static void proc_poll_expire(proc_t* p) {
    for (int i = 0; i < p->blk_len && i < 64; i++) {
        uint8_t* e = proc_user_ptr(p, p->blk_ptr + (uint32_t)i * 8);
        if (!e) continue;
        int16_t zero = 0;
        memcpy(e + 6, &zero, 2);
    }
    proc_poll_finish(p, 0);
}

void proc_tick_all(void) {
    uint32_t now = pit_get_ticks();
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t* p = &procs[i];
        if (!p->used || p->pid == 1) continue;

        if (p->alarm_tick && (int32_t)(now - p->alarm_tick) >= 0) {
            p->alarm_tick = 0;
            p->sig_pending |= 1u << 14;              /* SIGALRM */
            if (p->state == PROC_BLOCKED && p->wait_kind != PROC_WAIT_NONE) {
                p->eax = (uint32_t)-4;               /* -EINTR */
                p->wait_kind = PROC_WAIT_NONE;
                p->blk_has_deadline = false;
                p->state = PROC_READY;
            }
        }
        if (p->state == PROC_BLOCKED && p->wait_kind == PROC_WAIT_POLL &&
            p->blk_has_deadline && (int32_t)(now - p->blk_deadline) >= 0) {
            proc_poll_expire(p);                     /* timeout: 0 ready */
        }
    }
}

void proc_sched_tick(registers_t* regs) {
    proc_t* cur = proc_current();
    if (!cur || cur->pid == 1) return;
    if ((regs->cs & 3) != 3) return;
    if (cur->state != PROC_RUNNING) return;
    if (!proc_fdstate_alloc(cur)) return;

    if (proc_signal_deliver(regs)) {
        if (proc_frame_switched() || proc_current() != cur) return;
    }

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
    return proc_block_current_ext(regs, kind, status_ptr, pid, 0, false, -1);
}

/* Wait with an optional absolute tick deadline (poll timeouts). ptr/len
 * describe the user-side request so wakers can inspect it. */
bool_t proc_block_current_ext(registers_t* regs, proc_wait_t kind,
                              uint32_t ptr, int32_t len,
                              uint32_t deadline, bool_t has_deadline,
                              int32_t fd) {
    proc_t* cur = proc_current();
    if (!cur || cur->pid == 1) return false;
    if (!proc_fdstate_alloc(cur)) return false;

    proc_save_ctx(cur, regs);
    proc_save_state(cur);
    cur->wait_kind = kind;
    cur->wait_pid = (kind == PROC_WAIT_WAIT4 || kind == PROC_WAIT_WAITID) ? len : 0;
    cur->wait_status_ptr = (kind == PROC_WAIT_WAIT4) ? ptr : 0;
    cur->wait_infop = (kind == PROC_WAIT_WAITID) ? ptr : 0;
    cur->blk_ptr = ptr;
    cur->blk_len = len;
    cur->blk_fd = fd;
    cur->blk_deadline = deadline;
    cur->blk_has_deadline = has_deadline;
    cur->state = PROC_BLOCKED;
    user_set_active(false);

    proc_t* next = sched_pick_next(cur);
    if (!next) {
        cur->state = PROC_RUNNING;
        cur->wait_kind = PROC_WAIT_NONE;
        cur->blk_has_deadline = false;
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
    cur->blk_has_deadline = false;
    cur->wait_kind = PROC_WAIT_NONE;
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

void proc_reap(proc_t* p) {
    if (!p || !p->used) return;
    if (p->space) {
        proc_space_unref(p->space);
        p->space = NULL;
    }
    p->mem = NULL;
    p->pd_phys = 0;
    proc_fdstate_free(p);
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

/* Re-evaluate every blocked poller; only those with a ready fd wake up,
 * with the revents fields already written into their user memory. */
static void proc_wake_pollers(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t* p = &procs[i];
        if (!p->used || p->pid == 1) continue;
        if (p->state != PROC_BLOCKED || p->wait_kind != PROC_WAIT_POLL) continue;
        int ready = proc_poll_scan(p);
        if (ready > 0) proc_poll_finish(p, ready);
    }
}

/* Complete a blocked read() on behalf of its process: copy pipe bytes or
 * one console character into its own block and leave the count in eax. */
static void proc_read_check(proc_t* p) {
    if (!p->fd_state || p->blk_ptr == 0 || p->blk_len <= 0 || p->blk_fd < 0)
        return;

    int kind = user_fd_state_kind(p->fd_state, p->blk_fd);
    if (kind == USER_FD_PIPE_READ) {
        int id = user_fd_state_pipe(p->fd_state, p->blk_fd);
        if (id < 0 || !pipe_valid(id)) {
            p->eax = (uint32_t)-9;              /* -EBADF */
        } else {
            uint32_t fill = pipe_fill(id);
            if (fill == 0 && pipe_writers(id) > 0) return;   /* still empty */
            uint32_t want = (uint32_t)p->blk_len;
            if (fill > want) fill = want;
            char tmp[128];
            uint32_t done = 0;
            while (done < fill) {
                uint32_t chunk = fill - done;
                if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
                int got = pipe_read(id, tmp, chunk);
                if (got <= 0) break;
                uint8_t* dst = proc_user_ptr(p, p->blk_ptr + done);
                if (dst) memcpy(dst, tmp, (uint32_t)got);
                done += (uint32_t)got;
            }
            p->eax = done;                      /* 0 = EOF (no writers) */
        }
    } else if (kind == USER_FD_STDIN || kind == USER_FD_DEV_CONSOLE) {
        if (!keyboard_haschar()) return;
        char c = keyboard_getchar();
        uint8_t* dst = proc_user_ptr(p, p->blk_ptr);
        if (dst) dst[0] = (uint8_t)c;
        p->eax = 1;
    } else {
        return;                                 /* never blocks on other kinds */
    }
    p->wait_kind = PROC_WAIT_NONE;
    p->state = PROC_READY;
}

static void proc_wake_readers(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t* p = &procs[i];
        if (!p->used || p->pid == 1) continue;
        if (p->state != PROC_BLOCKED || p->wait_kind != PROC_WAIT_READ) continue;
        proc_read_check(p);
    }
}

void proc_notify_console(void) {
    proc_wake_pollers();
    proc_wake_readers();
}

void proc_notify_pipe(int pipe_id) {
    (void)pipe_id;
    proc_wake_pollers();
    proc_wake_readers();
}
