# PSP homebrew corpus (XBA-224). Runs inside the xba-psp-homebrew image (tools/docker/psp-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/psp/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.prx (the relocatable module an EBOOT.PBP carries: what the runtime loads),
# <prog>.pbp (the EBOOT.PBP itself, the runtime loads it too) and <prog>.elf (symbols and relocations, for debugging
# only), built by PSPSDK's own build.mak (BUILD_PRX=1, prog.mak), one sub-make per program in a copy of its sources.

PROGS := hello
OUT ?= out
BUILD ?= build

ifeq ($(strip $(PSPDEV)),)
$(error PSPDEV is not set: run inside the xba-psp-homebrew image)
endif

SRC := $(CURDIR)
OUT := $(abspath $(OUT))
BUILD := $(abspath $(BUILD))

all: $(addprefix $(OUT)/,$(addsuffix .prx,$(PROGS)))

$(OUT)/%.prx: %/main.c prog.mak | $(OUT)
	rm -rf $(BUILD)/$*
	mkdir -p $(BUILD)/$*
	cp $(SRC)/$*/*.c $(BUILD)/$*/
	$(MAKE) -C $(BUILD)/$* -f $(SRC)/prog.mak TARGET=$* PSP_EBOOT_TITLE="XBA $*"
	cp $(BUILD)/$*/$*.prx $@
	cp $(BUILD)/$*/$*.elf $(OUT)/$*.elf
	cp $(BUILD)/$*/EBOOT.PBP $(OUT)/$*.pbp

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
