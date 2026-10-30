#include "pipe.h"
#include "string.h"
#include "proc.h"

typedef struct {
    bool_t used;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int writers;          /* open write-end descriptors (0 => read side HUP) */
    char buf[PIPE_BUF_SIZE];
} pipe_t;

static pipe_t pipes[PIPE_MAX];

void pipe_init(void) {
    memset(pipes, 0, sizeof(pipes));
}

int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].used) {
            memset(&pipes[i], 0, sizeof(pipes[i]));
            pipes[i].used = true;
            return i;
        }
    }
    return -1;
}

bool_t pipe_valid(int id) {
    return (id >= 0 && id < PIPE_MAX && pipes[id].used) ? true : false;
}

uint32_t pipe_fill(int id) {
    if (!pipe_valid(id)) return 0;
    return pipes[id].count;
}

void pipe_writer_add(int id, int delta) {
    if (!pipe_valid(id)) return;
    pipes[id].writers += delta;
    if (pipes[id].writers < 0) pipes[id].writers = 0;
    proc_notify_pipe(id);      /* last writer closed => POLLHUP, EOF */
}

int pipe_writers(int id) {
    if (!pipe_valid(id)) return 0;
    return pipes[id].writers;
}

int pipe_read(int id, char* buf, uint32_t len) {
    if (!pipe_valid(id)) return -1;
    pipe_t* p = &pipes[id];
    if (len > p->count) len = p->count;
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = p->buf[(p->read_pos + i) % PIPE_BUF_SIZE];
    }
    p->read_pos = (p->read_pos + len) % PIPE_BUF_SIZE;
    p->count -= len;
    proc_notify_pipe(id);      /* readers freed space: POLLOUT waiters */
    return (int)len;
}

int pipe_write(int id, const char* buf, uint32_t len) {
    if (!pipe_valid(id)) return -1;
    pipe_t* p = &pipes[id];
    uint32_t space = PIPE_BUF_SIZE - p->count;
    if (len > space) len = space;
    for (uint32_t i = 0; i < len; i++) {
        p->buf[(p->write_pos + i) % PIPE_BUF_SIZE] = buf[i];
    }
    p->write_pos = (p->write_pos + len) % PIPE_BUF_SIZE;
    p->count += len;
    proc_notify_pipe(id);      /* new data: POLLIN waiters */
    return (int)len;
}
