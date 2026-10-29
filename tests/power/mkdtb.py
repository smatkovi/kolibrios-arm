#!/usr/bin/env python3
"""A device tree for QEMU's virt that takes the AArch64 kernel down the
Pro1X's code paths (docs/pro1x.md, "In QEMU"):

    tests/power/mkdtb.py OUT.dtb [--psci] [--virt] [--bootargs "..."]

QEMU's own tree (dumped by QEMU: GICv3, 8 GiB, a Cortex-A53) with the
Pro1X's compatible string, /chosen's simple-framebuffer at 0x5c000000 and
the splash memory reserved (the RAM console). --psci adds the SM6115's CPU
idle states as the phone's tree has them (arm/qcom sm6115.dtsi: the cores'
and the cluster's power domains, cpu@0 in cpu_pd0; not below /psci, which
QEMU replaces), with
suspend parameters in PSCI 0.2's original format, the only one QEMU's
CPU_SUSPEND takes (bit 16 power down, a 16-bit state id); without
local-timer-stop, as QEMU's wfi keeps the timer running. --virt keeps
QEMU's compatible string (its platform, the network) with those states.
Needs dtc."""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--psci', action='store_true')
ap.add_argument('--bootargs', default='')
ap.add_argument('--virt', action='store_true', help="QEMU's virt still (its devices: the network), not a Pro1X")
a = ap.parse_args()

with tempfile.TemporaryDirectory() as tmp:
    raw = os.path.join(tmp, 'virt.dtb')
    subprocess.run(['qemu-system-aarch64', '-M', f'virt,gic-version=3,dumpdtb={raw}', '-cpu', 'cortex-a53',
                    '-m', '8G', '-display', 'none'], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', raw], check=True,
                         capture_output=True, text=True).stdout

if not a.virt:
    dts = dts.replace('compatible = "linux,dummy-virt";', 'compatible = "fxtec,pro1x", "qcom,sm6115";', 1)
chosen = f'''chosen {{
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		ranges;
		bootargs = "{a.bootargs}";

		framebuffer@5c000000 {{
			compatible = "simple-framebuffer";
			reg = <0x00 0x5c000000 0x00 0x8e6200>;
			width = <1080>;
			height = <2160>;
			stride = <4320>;
			format = "a8r8g8b8";
		}};
	}};

	reserved-memory {{
		#address-cells = <0x02>;
		#size-cells = <0x02>;
		ranges;

		splash@5c000000 {{
			reg = <0x00 0x5c000000 0x00 0x1000000>;
			no-map;
		}};
	}};
'''
dts, n = re.subn(r'chosen \{[^{}]*\};\n', chosen, dts, count=1)
if not n:
    sys.exit('mkdtb: no /chosen in QEMU\'s tree')

if a.psci:
    # the phone's hierarchy; phandles of our own, well above dtc's
    states = '''
		idle-states {
			entry-method = "psci";

			little_cpu_sleep_0: cpu-sleep-0-0 {
				compatible = "arm,idle-state";
				idle-state-name = "silver-rail-power-collapse";
				arm,psci-suspend-param = <0x10003>;
				entry-latency-us = <290>;
				exit-latency-us = <376>;
				min-residency-us = <1182>;
			};
		};

		domain-idle-states {
			cluster_0_sleep_0: cluster-sleep-0-0 {
				compatible = "domain-idle-state";
				arm,psci-suspend-param = <0x10022>;
				entry-latency-us = <360>;
				exit-latency-us = <421>;
				min-residency-us = <782>;
			};

			cluster_0_sleep_1: cluster-sleep-0-1 {
				compatible = "domain-idle-state";
				arm,psci-suspend-param = <0x10044>;
				entry-latency-us = <800>;
				exit-latency-us = <2118>;
				min-residency-us = <7376>;
			};
		};
'''
    domains = '''
		cpu_pd0: power-domain-cpu0 {
			#power-domain-cells = <0>;
			power-domains = <&cluster_0_pd>;
			domain-idle-states = <&little_cpu_sleep_0>;
		};

		cluster_0_pd: power-domain-cpu-cluster0 {
			#power-domain-cells = <0>;
			domain-idle-states = <&cluster_0_sleep_0>, <&cluster_0_sleep_1>;
		};
'''
    # before /cpus's first child (its properties come first)
    c = dts.find('\tcpus {\n')
    m = re.compile(r'\n\t\t[\w,.-]+(@[0-9a-f]+)? \{\n').search(dts, c) if c >= 0 else None
    if not m:
        sys.exit('mkdtb: no /cpus')
    dts = dts[:m.start()] + '\n' + states + dts[m.start():]
    dts, n = re.subn(r'(cpu@0 \{\n)', r'\1\t\t\tpower-domains = <&cpu_pd0>;\n\t\t\tpower-domain-names = "psci";\n',
                     dts, count=1)
    if not n:
        sys.exit('mkdtb: no cpu@0')
    # QEMU replaces /psci when it loads a tree (hw/arm/boot.c,
    # fdt_add_psci_node), its children gone: the domains in a node of
    # their own (the kernel follows the phandles, not the place)
    dts, n = re.subn(r'(\n\tpsci \{\n)', '\n\tcpu-power-domains {' + domains + '\t};\n' + r'\1', dts, count=1)
    if not n:
        sys.exit('mkdtb: no /psci')

# MKDTB_DTS=FILE: the source as well
if os.environ.get('MKDTB_DTS'):
    open(os.environ['MKDTB_DTS'], 'w').write(dts)
r = subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', a.out, '-'], input=dts, text=True)
sys.exit(r.returncode)
