# the Hildon shell's launcher shows KolibriOS's own icons of the programs
# ported from it: icons32.png's icons (data/common/icons32.png, as
# KolibriOS's desktop uses them: shell 1, kfar 16; a text editor 9 for
# t_edit, a terminal with its cable 33 for terminal), on the launcher's
# tile colour, as /sys/icons/NAME.raw
HILDON_ICONS := shell:1 terminal:33 kfar:16 t_edit:9
RD_hildon := $(foreach i,$(HILDON_ICONS),icons/$(firstword $(subst :, ,$(i))).raw=$(A)/icons/$(firstword $(subst :, ,$(i))).raw)
define HILDON_ICON
$(A)/icons/$(1).raw: $(KOS)/data/common/icons32.png tools/kos-png.py | dirs
	@mkdir -p $$(dir $$@)
	python3 -I tools/kos-png.py icon $$< $$@ $(2) 32 0x30343c
endef
$(foreach i,$(HILDON_ICONS),$(eval $(call HILDON_ICON,$(firstword $(subst :, ,$(i))),$(lastword $(subst :, ,$(i))))))
