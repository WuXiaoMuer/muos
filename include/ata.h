#ifndef MUOS_ATA_H
#define MUOS_ATA_H
#include "types.h"

#define ATA_SECTOR_SIZE 512

/* Primary-bus master drive (QEMU's default IDE disk). */
bool_t      ata_init(void);
bool_t      ata_present(void);
uint32_t    ata_sector_count(void);
const char* ata_model(void);

/* 28-bit LBA PIO. Return bytes transferred, negative errno-style on
 * failure. Partial sectors use read-modify-write. */
int ata_read_bytes(uint32_t offset, void* buf, uint32_t count);
int ata_write_bytes(uint32_t offset, const void* buf, uint32_t count);

#endif
