#ifndef MUOS_PIT_H
#define MUOS_PIT_H
#include "types.h"

#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL1    0x41
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43
#define PIT_FREQUENCY   1193180

void pit_init(uint32_t frequency);
void pit_tick(void);
uint32_t pit_get_ticks(void);
void pit_sleep(uint32_t ms);

#endif
