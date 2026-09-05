#ifndef MUOS_TASK_H
#define MUOS_TASK_H
#include "types.h"

/* MuOS is currently a single-task system: the shell runs on the
 * kernel stack and no context switching happens (see kernel.c).
 * Task structs exist for introspection (`tasks` / `ps`) and the
 * self-test suite.
 *
 * Ring 3 groundwork already present: GDT slots 3/4 hold user code/
 * data selectors (see gdt.c). A real process model — per-task page
 * directories, TSS, separate kernel stacks, scheduling — is the
 * user-mode milestone in ROADMAP.md. */

#define TASK_NAME_MAX   32

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint32_t        pid;
    char            name[TASK_NAME_MAX];
    task_state_t    state;
    struct task*    next;
} task_t;

void task_init(void);
task_t* task_register(const char* name);  /* register the running entity */

task_t* task_get_current(void);
uint32_t task_get_count(void);
extern uint32_t task_count;

void task_list(void);

#endif
