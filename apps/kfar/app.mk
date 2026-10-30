# kfar (Kolibri Far), translated to C from KolibriOS's programs/fs/kfar
# (diamond and the KolibriOS team, GPL-2.0): apps/kfar/kfar.h.
#
# KFAR_FONT: one of kfar's fonts (font6x9, font7x12, font8x9, font8x16,
# font10x20), chosen when building, as the original's font.inc does
CAPPS += kfar
SRC_kfar := $(wildcard apps/kfar/*.c)
LIBS_kfar := libini
MEM_kfar := 0x1000000
# kfar.ini beside the program (app_path with .ini): KolibriOS's
# data/common/File Managers/kfar.ini
RD_kfar := kfar.ini=apps/kfar/kfar.ini arc.zip=$(A)/kfar-gen/arc.zip arc.7z=$(A)/kfar-gen/arc.7z
KFAR_FONT ?= font8x16
KFAR_GEN := $(A)/kfar-gen/$(KFAR_FONT)
KFAR_SRC := third_party/kolibrios/programs/fs/kfar
CFLAGS_kfar := -Iapps/kfar -I$(KFAR_GEN)

# the font: font.inc's table from the BMP, at build time
$(KFAR_GEN)/font.h: apps/kfar/mkfont.py $(KFAR_SRC)/$(KFAR_FONT).bmp
	@mkdir -p $(dir $@)
	python3 apps/kfar/mkfont.py $(KFAR_SRC)/$(KFAR_FONT).bmp $(KFAR_GEN)
$(KFAR_GEN)/font_data.h: $(KFAR_GEN)/font.h
$(patsubst %.c,$(A)/kfar/%.o,$(SRC_kfar)): $(KFAR_GEN)/font.h $(KFAR_GEN)/font_data.h

# archives for kfar_arc (kfar's plugin, lib/kfar_arc) to open: made by
# apps/kfar/mkarc.py with Python's zipfile and lzma (no tools, no
# downloads)
$(A)/kfar-gen/arc.zip: apps/kfar/mkarc.py
	@mkdir -p $(dir $@)
	python3 -I apps/kfar/mkarc.py zip $@
$(A)/kfar-gen/arc.7z: apps/kfar/mkarc.py
	@mkdir -p $(dir $@)
	python3 -I apps/kfar/mkarc.py 7z $@
