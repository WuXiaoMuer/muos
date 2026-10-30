#include "ata.h"
#include "io.h"
#include "string.h"

/* Primary IDE bus, master drive, 28-bit LBA, polled PIO. */
#define ATA_IO      0x1F0
#define ATA_CTRL    0x3F6
#define ATA_DATA    (ATA_IO + 0)
#define ATA_ERROR   (ATA_IO + 1)
#define ATA_FEAT    (ATA_IO + 1)
#define ATA_SECCNT  (ATA_IO + 2)
#define ATA_LBA0    (ATA_IO + 3)
#define ATA_LBA1    (ATA_IO + 4)
#define ATA_LBA2    (ATA_IO + 5)
#define ATA_DRIVE   (ATA_IO + 6)
#define ATA_STATUS  (ATA_IO + 7)
#define ATA_CMD     (ATA_IO + 7)

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DF   0x20
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_IDENTIFY 0xEC
#define CMD_READ     0x20
#define CMD_WRITE    0x30

static bool_t   present = false;
static uint32_t sectors = 0;
static char     model[41];

static uint8_t ata_status(void) {
    return inb(ATA_STATUS);
}

/* ~400ns delay: four alternate-status reads. */
static void ata_delay(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_CTRL);
}

/* Wait until BSY clears; optionally wait for DRQ. Returns 0 on success. */
static int ata_wait(uint8_t want_drq) {
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint8_t st = ata_status();
        if (!(st & ST_BSY)) {
            if (st & ST_DF) return -5;      /* drive fault */
            if (st & ST_ERR) return -5;
            if (!want_drq || (st & ST_DRQ)) return 0;
        }
    }
    return -16;                             /* timeout: EBUSY-ish */
}

static int ata_read_sector(uint32_t lba, void* out) {
    if (lba >= sectors) return -22;
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay();
    outb(ATA_FEAT, 0);
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, CMD_READ);

    int rc = ata_wait(1);
    if (rc) return rc;

    uint16_t tmp[256];
    for (int i = 0; i < 256; i++) tmp[i] = inw(ATA_DATA);
    memcpy(out, tmp, ATA_SECTOR_SIZE);
    return 0;
}

static int ata_write_sector(uint32_t lba, const void* in) {
    if (lba >= sectors) return -22;
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay();
    outb(ATA_FEAT, 0);
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA0, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA1, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA2, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_CMD, CMD_WRITE);

    int rc = ata_wait(1);
    if (rc) return rc;

    const uint16_t* w = (const uint16_t*)in;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, w[i]);

    /* Flush the drive's write cache before reporting success. */
    for (int i = 0; i < 4; i++) (void)inb(ATA_CTRL);
    outb(ATA_CMD, 0xE7);                    /* CACHE FLUSH */
    return ata_wait(0);
}

bool_t ata_init(void) {
    present = false;
    sectors = 0;
    model[0] = '\0';

    /* Soft-reset the bus: nIEN (bit 1) + SRST (bit 2), then release. */
    outb(ATA_CTRL, 0x04);
    for (int i = 0; i < 8; i++) (void)inb(ATA_CTRL);
    outb(ATA_CTRL, 0x02);
    for (int i = 0; i < 200000; i++) {
        if (!(ata_status() & ST_BSY)) break;
    }
    ata_delay();

    outb(ATA_DRIVE, 0xA0);                  /* master, CHS for identify */
    ata_delay();
    outb(ATA_SECCNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, CMD_IDENTIFY);

    uint8_t st = ata_status();
    if (st == 0 || st == 0xFF) return false;       /* no drive / floating */
    if (ata_wait(1) != 0) return false;

    /* A non-ATA device reports LBA1/LBA2 != 0 after identify. */
    if (inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0) return false;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);

    /* Words 27..46: model string, ASCII, high byte first. */
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--)
        model[i] = '\0';

    /* Cap at 28-bit LBA (the driver uses 28-bit PIO). */
    sectors = ((uint32_t)id[61] << 16) | id[60];
    if (sectors > 0x0FFFFFFFu) sectors = 0x0FFFFFFFu;
    if (sectors == 0) return false;

    present = true;
    return true;
}

bool_t ata_present(void)     { return present; }
uint32_t ata_sector_count(void) { return sectors; }
const char* ata_model(void)  { return model; }

int ata_read_bytes(uint32_t offset, void* buf, uint32_t count) {
    if (!present) return -19;               /* ENODEV */
    if (count == 0) return 0;
    if (count > 0xFFFFFFFFu - offset) return -22;
    uint32_t end = offset + count;
    if (end > sectors * ATA_SECTOR_SIZE) return -22;

    uint8_t sec[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < count) {
        uint32_t pos = offset + done;
        uint32_t lba = pos / ATA_SECTOR_SIZE;
        uint32_t off = pos % ATA_SECTOR_SIZE;
        uint32_t n = ATA_SECTOR_SIZE - off;
        if (n > count - done) n = count - done;
        int rc = ata_read_sector(lba, sec);
        if (rc) return rc;
        memcpy((uint8_t*)buf + done, sec + off, n);
        done += n;
    }
    return (int)count;
}

int ata_write_bytes(uint32_t offset, const void* buf, uint32_t count) {
    if (!present) return -19;
    if (count == 0) return 0;
    if (count > 0xFFFFFFFFu - offset) return -22;
    uint32_t end = offset + count;
    if (end > sectors * ATA_SECTOR_SIZE) return -22;

    uint8_t sec[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < count) {
        uint32_t pos = offset + done;
        uint32_t lba = pos / ATA_SECTOR_SIZE;
        uint32_t off = pos % ATA_SECTOR_SIZE;
        uint32_t n = ATA_SECTOR_SIZE - off;
        if (n > count - done) n = count - done;

        if (n != ATA_SECTOR_SIZE) {
            int rc = ata_read_sector(lba, sec);      /* read-modify-write */
            if (rc) return rc;
        }
        memcpy(sec + off, (const uint8_t*)buf + done, n);
        int rc = ata_write_sector(lba, sec);
        if (rc) return rc;
        done += n;
    }
    return (int)count;
}
