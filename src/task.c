#include "task.h"
#include "vga.h"
#include "kheap.h"
#include "string.h"

static task_t* task_list_head = NULL;
static task_t* task_list_tail = NULL;
static task_t* current_task = NULL;
static uint32_t next_pid = 1;
uint32_t task_count = 0;  /* exported for introspection */

void task_init(void) {
    task_list_head = NULL;
    task_list_tail = NULL;
    current_task = NULL;
    next_pid = 1;
    task_count = 0;
}

/* Register the entity that is already running (shell on the kernel
 * stack). Exists so `tasks` / `ps` / self-tests have something to
 * show; no context switching happens on top of it. */
task_t* task_register(const char* name) {
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) return NULL;
    task->pid   = next_pid++;
    task->state = TASK_RUNNING;
    task->next  = NULL;
    strncpy(task->name, name, TASK_NAME_MAX - 1);
    task->name[TASK_NAME_MAX - 1] = '\0';

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
        int len = (int)strlen(t->name);
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
