/* acpi.c - tiny ACPI table walker + S5 shutdown and reset.
 *
 * Only the pieces needed for power control are implemented: RSDP search,
 * RSDT/XSDT walk to the FADT, and the PM1a control block. SLP_TYPa is
 * assumed to be 5 (the near-universal S5 value; QEMU uses 5 as well);
 * when no FADT is found the common QEMU ports 0x604 / 0xB004 are used.
 */
#include "acpi.h"
#include "string.h"
#include "io.h"
#include "serial.h"
#include "mm.h"

static bool_t   present;
static uint16_t pm1a_cnt;
static uint32_t fadt_addr;

typedef struct {
    char     sig[8];
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;
} __attribute__((packed)) rsdt_t;

typedef struct {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    uint8_t  oem_id[6];
    uint8_t  oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
} __attribute__((packed)) fadt_prefix_t;

/* Only dereference pointers that lie inside the identity-mapped RAM;
 * SeaBIOS places some ACPI tables in the reserved tail above ram_top. */
static bool_t ram_ok(uint32_t p, uint32_t need) {
    uint32_t top = mm_get_total_pages() * PAGE_SIZE;
    if (!top) top = 0x2000000u;
    return p >= 0x1000 && p + need <= top;
}

static bool_t sum_ok(const uint8_t* p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

static uint32_t rsdp_search(void) {
    uint16_t ebda_seg = *(volatile uint16_t*)0x40E;
    uint32_t ebda = (uint32_t)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        for (uint32_t p = ebda; p < ebda + 1024; p += 16) {
            if (memcmp((const void*)p, "RSD PTR ", 8) == 0 &&
                sum_ok((const uint8_t*)p, 20))
                return p;
        }
    }
    for (uint32_t p = 0xE0000; p < 0x100000; p += 16) {
        if (memcmp((const void*)p, "RSD PTR ", 8) == 0 &&
            sum_ok((const uint8_t*)p, 20))
            return p;
    }
    return 0;
}

bool_t acpi_init(void) {
    present = false;
    pm1a_cnt = 0;
    fadt_addr = 0;

    uint32_t rsdp = rsdp_search();
    if (!rsdp || !ram_ok(rsdp, 36)) return false;

    uint8_t rev = *(volatile uint8_t*)(rsdp + 15);
    uint32_t rsdt_p = *(volatile uint32_t*)(rsdp + 16);
    if (rev >= 2) {
        uint64_t xsdt = *(volatile uint64_t*)(rsdp + 24);
        if (xsdt && xsdt < 0x100000000ULL)
            rsdt_p = (uint32_t)xsdt;
    }
    if (!rsdt_p || (rsdt_p & 0xFu) || !ram_ok(rsdt_p, 36)) return false;

    const rsdt_t* rsdt = (const rsdt_t*)rsdt_p;
    if (memcmp(rsdt->sig, "RSDT", 4) != 0 && memcmp(rsdt->sig, "XSDT", 4) != 0)
        return false;
    if (rsdt->length < sizeof(rsdt_t) || rsdt->length > 0x10000) return false;
    if (!ram_ok(rsdt_p, rsdt->length)) return false;

    uint32_t n = (rsdt->length - sizeof(rsdt_t)) / 4;
    const uint32_t* ent = (const uint32_t*)(rsdt_p + sizeof(rsdt_t));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = ent[i];
        if (!t || (t & 0xFu) || !ram_ok(t, 64)) continue;
        if (memcmp((const void*)t, "FACP", 4) == 0) {
            fadt_addr = t;
            break;
        }
    }
    if (!fadt_addr) return false;

    const fadt_prefix_t* f = (const fadt_prefix_t*)fadt_addr;
    if (f->length < 64 || !ram_ok(fadt_addr, f->length)) return false;
    pm1a_cnt = (uint16_t)f->pm1a_cnt_blk;
    present = true;
    serial_write("[acpi] FADT found, PM1a_CNT=");
    { char b[12]; u32_to_dec(b, pm1a_cnt); serial_write(b); }
    serial_write("\n");
    return true;
}

bool_t acpi_present(void) { return present; }
uint16_t acpi_pm1a_control(void) { return pm1a_cnt; }

void acpi_poweroff(void) {
    /* SLP_TYPa = 5, SLP_EN = bit13. */
    uint16_t val = (uint16_t)((5u << 10) | (1u << 13));
    uint16_t ports[2] = { pm1a_cnt, 0x604 };
    for (int i = 0; i < 2; i++) {
        uint32_t port = ports[i];
        if (!port) port = 0xB004;
        outw((uint16_t)port, val);
    }
    /* QEMU falls back to this port on newer machine types. */
    outw(0xB004, val);
    for (;;) __asm__ volatile ("cli; hlt");
}

void acpi_reboot(void) {
    while (inb(0x64) & 0x02) { }
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("cli; hlt");
}
