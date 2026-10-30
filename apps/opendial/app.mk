# opendial: KolibriOS's open and save dialog, translated to C from
# programs/fs/opendial/opendial.asm (apps/opendial/opendial.h). proc_lib's
# OpenDialog_start (lib/proc_lib) starts it with the name of its caller's
# shared memory. On the ramdisk where proc_lib's callers look for it, as
# in KolibriOS's image: /sys/File managers/opendial, beside its
# icon2ext.ini (the files' icons by extension; KolibriOS's
# data/common/File Managers/icon2ext.ini). The icons, /sys/icons18.png
# there (libimg decodes it at the start), are built in as 24-bit pixels
# by tools/png2bgr.py, as t_edit's toolbar.
CAPPS += opendial
OD_GEN := $(A)/opendial-gen
SRC_opendial := $(wildcard apps/opendial/*.c) $(OD_GEN)/icons18.c
LIBS_opendial := boxlib
CFLAGS_opendial := -Iapps/opendial
RDNAME_opendial := File%20managers/opendial
RD_opendial := File%20managers/icon2ext.ini=$(OD_GEN)/icon2ext.ini

$(OD_GEN)/icons18.c: $(KOS)/data/common/icons18.png tools/png2bgr.py
	@mkdir -p $(dir $@)
	python3 tools/png2bgr.py $< $@ icons18
# a copy under a name without a space (make's words have none)
$(OD_GEN)/icon2ext.ini: $(KOS)/data/common/File\ Managers/icon2ext.ini
	@mkdir -p $(dir $@)
	cp "$<" $@
