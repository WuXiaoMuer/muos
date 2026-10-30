#ifndef MUOS_PCI_H
#define MUOS_PCI_H
#include "types.h"

/* Minimal PCI configuration-space access (mechanism #1: 0xCF8/0xCFC).
 * Only what the network driver needs: locate a device and read its BARs. */

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_VENDOR_REALTEK 0x10EC
#define PCI_DEVICE_RTL8139 0x8139

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off,
                     uint32_t value);

/* Find the first device with this vendor/device id. Returns true and the
 * location on success. */
bool_t pci_find(uint16_t vendor, uint16_t device,
                uint8_t* bus, uint8_t* slot, uint8_t* func);

/* Enable I/O decoding and bus mastering in the command register. */
void pci_enable_io_busmaster(uint8_t bus, uint8_t slot, uint8_t func);

#endif
