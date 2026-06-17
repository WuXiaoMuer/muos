#ifndef MUOS_SERIAL_H
#define MUOS_SERIAL_H
#include "types.h"

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char* str);

#endif
