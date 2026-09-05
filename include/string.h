#ifndef MUOS_STRING_H
#define MUOS_STRING_H
#include "types.h"

/* Freestanding libc-style string/memory helpers. Standard names are
 * load-bearing: GCC emits implicit calls to memcpy/memset for struct
 * copies and initializations even under -ffreestanding. */

void*    memset(void* dst, int value, uint32_t count);
void*    memcpy(void* dst, const void* src, uint32_t count);
void*    memmove(void* dst, const void* src, uint32_t count);
int      memcmp(const void* a, const void* b, uint32_t count);

uint32_t strlen(const char* s);
int      strcmp(const char* a, const char* b);
int      strncmp(const char* a, const char* b, uint32_t n);
char*    strcpy(char* dst, const char* src);
char*    strncpy(char* dst, const char* src, uint32_t n);

/* uint32_t → decimal string (NUL-terminated). Returns length. */
uint32_t u32_to_dec(char* buf, uint32_t value);

#endif
