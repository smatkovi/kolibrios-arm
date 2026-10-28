/* KolibriOS for ARM, AArch64: the F(x)tec Pro1X (Qualcomm SM6115) */
#include "kernel.h"
#include "fdt.h"

struct platform platform_pro1x = {
	.name = "F(x)tec Pro1X (SM6115)",
};

bool pro1x_probe(const void *fdt)
{
	(void)fdt;
	return false;
}
