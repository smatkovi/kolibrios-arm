# t_edit: KolibriOS's text editor with syntax colouring, translated to C
# from programs/other/t_edit (apps/t_edit/t_edit.h). /sys/t_edit with
# its t_edit.ini and the syntax files in /sys/info, as KolibriOS's image
# has them in one folder.
#
# Built here from KolibriOS's sources: the pictures (te_icon.png: the
# toolbar, tl_nod_16.png and log_el's tl_sys_16.png: the syntax list's
# icons) into C arrays of 24-bit pixels by tools/png2bgr.py, as libimg
# decodes them at the start of the original; the syntax files
# (info/*_syn.asm: data and FASM macros) assembled by FASMARM, not packed
# (KolibriOS packs them with kpack), named as info/build.bat names them
# (t_edit.ini asks for cpp_dar.syn and ini.syn). FASMARM sizes dw and dd
# as ARM does (32 and 64 bits) and knows f0-f7 as registers, so
# apps/t_edit/fasm2arm.py makes x86's dw, dd ... ARM's dh, dw ... and the
# label f1 of the help texts te_f1 first.
CAPPS += t_edit
TE_SRC := $(KOS)/programs/other/t_edit
TE_GEN := $(A)/t_edit-gen
TE_PICS := te_icon tl_nod_16 tl_sys_16
SRC_t_edit := $(wildcard apps/t_edit/*.c) $(foreach p,$(TE_PICS),$(TE_GEN)/$(p).c)
LIBS_t_edit := boxlib libini msgbox proc_lib kmenu
# the text (100002 characters of 18 bytes), the syntax file (410000),
# the save buffer, the heap's free list
MEM_t_edit := 0x1000000
# info/NAME.syn from info/SRC_syn.asm
TE_SYN := asm:asm cpp_cla:cpp_kol_cla cpp_dar:cpp_kol_dar cpp_def:cpp_kol_def default:default \
	html:html ini:ini_files voc_eng_rus:voc_eng_rus win_const:win_const
RD_t_edit := t_edit.ini=$(TE_SRC)/t_edit.ini \
	$(foreach s,$(TE_SYN),info/$(firstword $(subst :, ,$(s))).syn=$(TE_GEN)/$(firstword $(subst :, ,$(s))).syn)

$(TE_GEN)/te_icon.c: $(TE_SRC)/te_icon.png tools/png2bgr.py
	@mkdir -p $(dir $@)
	python3 tools/png2bgr.py $< $@ te_icon
$(TE_GEN)/tl_nod_16.c: $(TE_SRC)/tl_nod_16.png tools/png2bgr.py
	@mkdir -p $(dir $@)
	python3 tools/png2bgr.py $< $@ tl_nod_16
$(TE_GEN)/tl_sys_16.c: $(KOS)/programs/media/log_el/tl_sys_16.png tools/png2bgr.py
	@mkdir -p $(dir $@)
	python3 tools/png2bgr.py $< $@ tl_sys_16

# the syntax files' sources for FASMARM, beside asm.inc (included)
$(TE_GEN)/src/.renamed: $(wildcard $(TE_SRC)/info/*_syn.asm) $(TE_SRC)/info/asm.inc apps/t_edit/fasm2arm.py
	@mkdir -p $(dir $@)
	for f in $(filter-out %.py,$^); do python3 apps/t_edit/fasm2arm.py $$f $(dir $@)$$(basename $$f); done
	touch $@
define TE_SYN_RULE
$(TE_GEN)/$(1).syn: $(TE_GEN)/src/.renamed
	@[ -x $$(FASMARM) ] || tools/fetch-fasmarm.sh
	$$(FASMARM) -m 16384 $(TE_GEN)/src/$(2)_syn.asm $$@ > /dev/null
endef
$(foreach s,$(TE_SYN),$(eval $(call TE_SYN_RULE,$(firstword $(subst :, ,$(s))),$(lastword $(subst :, ,$(s))))))
