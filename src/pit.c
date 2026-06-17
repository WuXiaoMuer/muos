#include "pit.h"
#include "io.h"

static volatile uint32_t pit_ticks = 0;
static uint32_t pit_freq = 0;
static uint32_t pit_ms_per_tick = 0;

void pit_init(uint32_t frequency) {
    pit_freq = frequency;
    pit_ms_per_tick = 1000 / frequency;
    pit_ticks = 0;

    uint32_t divisor = PIT_FREQUENCY / frequency;

    /* Channel 0, lobyte/hibyte access, mode 2 (rate generator), binary */
    outb(PIT_COMMAND, 0x36);
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void pit_tick(void) {
    pit_ticks++;
}

uint32_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_sleep(uint32_t ms) {
    uint32_t target = pit_ticks + ms / pit_ms_per_tick;
    while (pit_ticks < target) {
        __asm__ volatile ("hlt");
    }
}
