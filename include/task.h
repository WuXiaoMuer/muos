#ifndef MUOS_TASK_H
#define MUOS_TASK_H
#include "types.h"

/* Assembly-visible scheduler flag (defined in isr_stubs.s) */
extern volatile uint8_t need_reschedule;
extern uint32_t current_task_ptr;

#define TASK_NAME_MAX   32
#define TASK_STACK_SIZE 4096

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint32_t        esp;            /* Saved stack pointer */
    uint32_t        pid;            /* Process ID */
    char            name[TASK_NAME_MAX];
    task_state_t    state;
    uint32_t*       stack_base;     /* Stack page for freeing */
    struct task*    next;
} task_t;

void task_init(void);
task_t* task_create(void (*entry)(void), const char* name);
void task_exit(void);
void task_yield(void);

task_t* task_get_current(void);
uint32_t task_get_count(void);

void task_list(void);

/* Scheduler - called from timer ISR */
void task_schedule(void);

/* Assembly helper */
extern uint32_t task_switch(uint32_t current_esp);

#endif
