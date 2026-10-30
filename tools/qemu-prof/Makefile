# The profiling plugin for QEMU (prof.c): make -C tools/qemu-prof
# Needs QEMU's qemu-plugin.h (Arch: qemu-common) and GLib's headers.
B := ../../build/qemu-prof
$(B)/prof.so: prof.c
	@mkdir -p $(B)
	$(CC) -O2 -Wall -shared -fPIC $(shell pkg-config --cflags glib-2.0) $< -o $@
