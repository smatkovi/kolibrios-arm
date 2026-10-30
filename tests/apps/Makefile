# KolibriOS's ported programs in QEMU: fstest on both kernels, the shell,
# the terminal against a socket (run.py); IMG=docs/img renews the
# documentation's screenshots
IMG ?=
all:
	python3 run.py $(if $(IMG),--img $(IMG))
.PHONY: all
