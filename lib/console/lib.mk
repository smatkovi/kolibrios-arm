# the console library's font and scroll bar, from KolibriOS's console_coff
CONSOLE_SRC := $(KOS)/programs/develop/libraries/console_coff
$(A)/gen/console_font.h: $(CONSOLE_SRC)/font8x16.bmp tools/kos-bmp.py | dirs
	@mkdir -p $(dir $@)
	python3 -I tools/kos-bmp.py font $< $@ console_font
$(A)/gen/console_scroll.h: $(CONSOLE_SRC)/conscrlv.bmp tools/kos-bmp.py | dirs
	@mkdir -p $(dir $@)
	python3 -I tools/kos-bmp.py image $< $@ console_scroll
$(A)/lib/console/console.o: $(A)/gen/console_font.h $(A)/gen/console_scroll.h
