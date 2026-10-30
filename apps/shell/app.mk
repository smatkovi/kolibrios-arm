# shell: KolibriOS's command line shell (programs/system/shell, in C),
# built from its own sources: copied from third_party/kolibrios with LF
# line ends, apps/shell/arm.patch applied, system/kolibri.c replaced by
# apps/shell/kolibri.c; the console window from lib/console
CAPPS += shell
SHELL_DIR := $(KOS)/programs/system/shell
SHELL_B := $(A)/shell-src
SRC_shell := $(SHELL_B)/shell.c apps/shell/kolibri.c
CFLAGS_shell := -fpermissive -fsigned-char -DAUTOBUILD -DLANG_ENG=1 -Iapps/shell/include -I$(SHELL_B) -w
LIBS_shell := console
MEM_shell := 0x400000
RD_shell := settings/.shell=$(SHELL_DIR)/bin/eng/.shell

$(SHELL_B)/shell.c: $(SHELL_B)/.patched
$(SHELL_B)/.patched: $(SHELL_DIR)/shell.c $(SHELL_DIR)/bin/eng/.shell apps/shell/arm.patch
	rm -rf $(SHELL_B)
	mkdir -p $(SHELL_B)
	cp -r $(SHELL_DIR)/. $(SHELL_B)/
	find $(SHELL_B) -type f \( -name '*.c' -o -name '*.h' \) -exec sed -i 's/\r$$//' {} +
	patch -s -d $(SHELL_B) -p1 < apps/shell/arm.patch
	touch $@
