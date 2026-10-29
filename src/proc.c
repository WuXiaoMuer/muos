#include "proc.h"
#include "mm.h"
#include "kheap.h"
#include "pit.h"
#include "tss.h"
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
    if (!p) return false;
    if (p->mem) return true;
    p->mem = mm_alloc_pages(USER_SIZE / PAGE_SIZE);
    if (!p->mem) return false;
    p->mem_pages = USER_SIZE / PAGE_SIZE;
    return true;
}

void proc_snapshot_free(proc_t* p) {
    if (!p || !p->mem) return;
    mm_free_pages(p->mem, p->mem_pages);
    p->mem = NULL;
    p->mem_pages = 0;
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
