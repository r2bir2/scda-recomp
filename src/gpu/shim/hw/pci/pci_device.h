/* Local stand-in for QEMU's PCI device (see src/gpu/PROVENANCE.md).
 *
 * The NV2A's PBUS block mirrors three dwords of its PCI configuration space;
 * that is all of PCI the imported files touch. nv2a_recomp.c fills in the
 * vendor/device/class values the hardware reports. */
#pragma once

#include <stdint.h>
#include <string.h>

typedef struct PCIBus PCIBus;
typedef struct PCIDevice { uint8_t config[256]; } PCIDevice;

/* NV2AState starts with its PCIDevice (parent_obj), as in QEMU. */
#define PCI_DEVICE(obj) ((PCIDevice *)(obj))

#define PCI_VENDOR_ID      0x00
#define PCI_DEVICE_ID      0x02
#define PCI_COMMAND        0x04
#define PCI_CLASS_REVISION 0x08

static inline uint32_t pci_get_long(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void pci_set_long(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void pci_set_word(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
