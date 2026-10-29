#ifndef MUOS_PIPE_H
#define MUOS_PIPE_H
#include "types.h"

#define PIPE_MAX       8
#define PIPE_BUF_SIZE  4096

void pipe_init(void);
int  pipe_create(void);
int  pipe_read(int id, char* buf, uint32_t len);
int  pipe_write(int id, const char* buf, uint32_t len);
bool_t pipe_valid(int id);
uint32_t pipe_fill(int id);

#endif
