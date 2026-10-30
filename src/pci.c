#include "pci.h"
#include "io.h"

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, off & 0xFC);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                 uint32_t value) {
    uint32_t addr = 0x80000000u |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) |
                    (off & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, value);
}

bool_t pci_find(uint16_t vendor, uint16_t device,
                uint8_t* bus, uint8_t* slot, uint8_t* func) {
    /* QEMU puts PCI devices on bus 0; scan a few buses anyway. */
    for (uint8_t b = 0; b < 8; b++) {
        for (uint8_t s = 0; s < 32; s++) {
            uint32_t id = pci_read32(b, s, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;   /* no device */
            if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                if (bus)  *bus  = b;
                if (slot) *slot = s;
                if (func) *func = 0;
                return true;
            }
        }
    }
    return false;
}

void pci_enable_io_busmaster(uint8_t bus, uint8_t slot, uint8_t func) {
    /* Command register at 0x04: bit0 = I/O space, bit2 = bus master. */
    uint16_t cmd = pci_read16(bus, slot, func, 0x04);
    cmd |= 0x0001 | 0x0004;
    uint32_t v = pci_read32(bus, slot, func, 0x04);
    v = (v & 0xFFFF0000u) | cmd;
    pci_write32(bus, slot, func, 0x04, v);
}
