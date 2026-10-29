#ifndef MUOS_MODULE_H
#define MUOS_MODULE_H
#include "types.h"
#include "multiboot.h"

/* Multiboot modules let QEMU/GRUB hand the kernel arbitrary files:
 *   qemu-system-i386 -kernel kernel.elf -initrd program.elf -m 256M
 * Each module is a contiguous RAM range; no filesystem needed. */

#define MODULE_MAX 8

void     modules_init(const multiboot_info_t* mb);
uint32_t modules_count(void);
bool_t   modules_get(uint32_t index, uint32_t* start, uint32_t* end);
const char* modules_name(uint32_t index);

#endif
