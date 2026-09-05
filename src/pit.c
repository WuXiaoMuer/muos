#include "pit.h"
#include "io.h"

static volatile uint32_t pit_ticks = 0;
static uint32_t pit_freq = 0;
static uint32_t pit_ms_per_tick = 0;

void pit_init(uint32_t frequency) {
    /* Clamp so the divisor fits in 16 bits and ms_per_tick stays nonzero */
    if (frequency < 19) frequency = 19;          /* 1193180/65535 ≈ 18.2 Hz */
    if (frequency > PIT_FREQUENCY) frequency = PIT_FREQUENCY;

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
