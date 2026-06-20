#include "task.h"
#include "vga.h"
#include "mm.h"

static task_t* task_list_head = NULL;
static task_t* task_list_tail = NULL;
static task_t* current_task = NULL;
static uint32_t next_pid = 1;
uint32_t task_count = 0;  /* exported for kernel.c */

void task_init(void) {
    task_list_head = NULL;
    task_list_tail = NULL;
    current_task = NULL;
    next_pid = 1;
    task_count = 0;
}

/* Register a task that is already running (e.g. shell on kernel
 * stack). Used to keep `tasks` / `ps` working without actually
 * context-switching. */
task_t* task_register(const char* name) {
    task_t* task = (task_t*)mm_alloc_page();
    if (!task) return NULL;
    task->pid   = next_pid++;
    task->state = TASK_RUNNING;
    task->stack_base = NULL;
    task->next  = NULL;
    int i;
    for (i = 0; name[i] && i < TASK_NAME_MAX - 1; i++) task->name[i] = name[i];
    task->name[i] = '\0';

    if (task_list_tail) {
        task_list_tail->next = task;
        task_list_tail = task;
    } else {
        task_list_head = task;
        task_list_tail = task;
    }
    task_count++;
    current_task = task;
    return task;
}

task_t* task_create(void (*entry)(void), const char* name) {
    /* Allocate contiguous kernel stack — 4 pages (16KB) for deep GUI call chains */
    uint32_t* stack = (uint32_t*)mm_alloc_pages(4);
    if (!stack) return NULL;

    /* Allocate task struct page */
    task_t* task = (task_t*)mm_alloc_page();
    if (!task) {
        mm_free_pages(stack, 4);
        return NULL;
    }

    task->pid   = next_pid++;
    task->state = TASK_READY;
    task->stack_base = stack;
    task->next  = NULL;

    /* Copy name */
    int i;
    for (i = 0; i < TASK_NAME_MAX - 1 && name[i] != '\0'; i++) {
        task->name[i] = name[i];
    }
    task->name[i] = '\0';

    /* Build an interrupt frame that matches isr_stubs.s irq_common exactly.
     * Stack layout (top to bottom, as restored by irq_common):
     *   edi, esi, ebp, esp, ebx, edx, ecx, eax   <- popa
     *   gs, fs, es, ds                           <- segment pops
     *   int_no, err_code                         <- skipped by add $8
     *   eip, cs, eflags, return_addr             <- iret
     *
     * We push in reverse order so that the last pushed value becomes the top.
     */
    uint32_t* sp = (uint32_t*)((uint32_t)stack + TASK_STACK_SIZE);

    /* iret frame */
    *(--sp) = (uint32_t)task_exit;  /* return address (unused, tasks loop forever) */
    *(--sp) = 0x202;                /* EFLAGS */
    *(--sp) = 0x08;                 /* CS */
    *(--sp) = (uint32_t)entry;      /* EIP */
    *(--sp) = 0;                    /* err_code */
    *(--sp) = 0x20;                 /* int_no */

    /* Segment registers: irq_common pops gs, fs, es, ds, so push ds, es, fs, gs */
    *(--sp) = 0x10; /* ds */
    *(--sp) = 0x10; /* es */
    *(--sp) = 0x10; /* fs */
    *(--sp) = 0x10; /* gs */

    /* General registers: pusha pops edi, esi, ebp, esp, ebx, edx, ecx, eax,
     * so push eax, ecx, edx, ebx, esp, ebp, esi, edi.
     */
    *(--sp) = 0; /* eax */
    *(--sp) = 0; /* ecx */
    *(--sp) = 0; /* edx */
    *(--sp) = 0; /* ebx */
    *(--sp) = 0; /* esp */
    *(--sp) = 0; /* ebp */
    *(--sp) = 0; /* esi */
    *(--sp) = 0; /* edi */

    task->esp = (uint32_t)sp;

    /* Add to task list */
    if (task_list_tail) {
        task_list_tail->next = task;
        task_list_tail = task;
    } else {
        task_list_head = task;
        task_list_tail = task;
    }
    task_count++;

    /* First real task becomes current */
    if (!current_task) {
        current_task = task;
        current_task->state = TASK_RUNNING;
        current_task_ptr = (uint32_t)&current_task->esp;
    }

    return task;
}

void task_exit(void) {
    if (current_task) {
        current_task->state = TASK_DEAD;

        /* Unlink from list */
        task_t* prev = NULL;
        task_t* t = task_list_head;
        while (t) {
            if (t == current_task) {
                if (prev) prev->next = t->next;
                else task_list_head = t->next;
                if (task_list_tail == t) task_list_tail = prev;
                task_count--;
                break;
            }
            prev = t;
            t = t->next;
        }

        uint32_t* sb = current_task->stack_base;
        mm_free_page(current_task);
        mm_free_pages(sb, 4);
        current_task = NULL;
    }

    need_reschedule = 1;
    for (;;) { __asm__ volatile ("hlt"); }
}

task_t* task_get_current(void) { return current_task; }
uint32_t task_get_count(void)  { return task_count; }

void task_list(void) {
    task_t* t = task_list_head;
    vga_print("PID  Name                State\n");
    vga_print("---- ------------------- --------\n");
    while (t) {
        vga_print_dec(t->pid);
        if (t->pid < 10) vga_putchar(' ');
        vga_print("    ");
        vga_print(t->name);

        /* Pad name to 20 chars */
        int len = 0;
        for (const char* p = t->name; *p; p++) len++;
        for (int i = len; i < 20; i++) vga_putchar(' ');

        switch (t->state) {
            case TASK_READY:   vga_print(" READY\n");   break;
            case TASK_RUNNING: vga_print(" RUNNING\n"); break;
            case TASK_BLOCKED: vga_print(" BLOCKED\n"); break;
            default:           vga_print(" DEAD\n");    break;
        }
        t = t->next;
    }
}

/* Called from assembly irq_common after saving current ESP */
void task_schedule(void) {
    if (!current_task || !task_list_head) return;
    /* Single task — no switching needed, just keep running */
    if (task_count <= 1) return;

    /* Move current to ready */
    if (current_task->state == TASK_RUNNING) {
        current_task->state = TASK_READY;
    }

    /* Find next runnable task (round-robin) */
    task_t* next = current_task->next;
    for (uint32_t tries = 0; tries < task_count; tries++) {
        if (!next) next = task_list_head;
        if (next->state == TASK_READY || next->state == TASK_RUNNING) {
            current_task = next;
            current_task->state = TASK_RUNNING;
            current_task_ptr = (uint32_t)&current_task->esp;
            return;
        }
        next = next->next;
    }

    /* Fallback to head */
    current_task = task_list_head;
    current_task->state = TASK_RUNNING;
    current_task_ptr = (uint32_t)&current_task->esp;
}

void task_yield(void) {
    if (task_count > 1) {
        need_reschedule = 1;
    }
}
