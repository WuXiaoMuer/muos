#ifndef MUOS_ACPI_H
#define MUOS_ACPI_H
#include "types.h"

/* Minimal ACPI FADT lookup for power management. Finds the RSDP in the
 * EBDA or the 0xE0000-0xFFFFF BIOS area, walks the RSDT/XSDT for FACP and
 * exposes the PM1a control block. */
bool_t acpi_init(void);
bool_t acpi_present(void);
uint16_t acpi_pm1a_control(void);

/* Power off (S5) and reset, with legacy port fallbacks for QEMU. */
void acpi_poweroff(void);
void acpi_reboot(void);

#endif
