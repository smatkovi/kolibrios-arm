/* KolibriOS for ARM, AArch64: the CPU's idle states through PSCI (to come) */
#include "kernel.h"
#include "power.h"

void psci_probe(const void *fdt) { (void)fdt; }
void psci_init(void) {}
bool psci_idle(u64 idle_ticks) { (void)idle_ticks; return false; }
const char *psci_summary(void) { return "wfi only"; }
