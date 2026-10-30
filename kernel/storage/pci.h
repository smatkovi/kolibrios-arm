/* KolibriOS for ARM, AArch64: QEMU virt's PCI Express, for the storage
 * tests (pci.c) */
#ifndef PCI_H
#define PCI_H

#define PCI_CLASS_UFS 0x0109
#define PCI_CLASS_SDHCI 0x0805

void pci_init(addr_t ecam_va, u64 window, u64 size);
u64 pci_find(u32 class, u32 *bar_size);

#endif
