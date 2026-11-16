# NDS homebrew corpus (XBA-223). Runs inside the xba-nds-homebrew image (tools/docker/nds-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/nds/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.nds (ndstool: ARM9 = our code with libnds + calico, ARM7 = calico's stock
# ds7_maine.elf) and <prog>.elf (the ARM9 symbols, for debugging only: the runtime gets the .nds).

PROGS := hello
OUT ?= out
BUILD ?= build

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set: run inside the xba-nds-homebrew image)
endif

CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
CALICO := $(DEVKITPRO)/calico
LIBNDS := $(DEVKITPRO)/libnds

ARCH := -march=armv5te -mtune=arm946e-s -mthumb
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -std=gnu11 -ffunction-sections -fdata-sections \
          -D__NDS__ -DARM9 -I$(CALICO)/include -I$(LIBNDS)/include
LDFLAGS := -specs=$(CALICO)/share/ds9.specs $(ARCH) -g -L$(CALICO)/lib -L$(LIBNDS)/lib
LIBS := -lnds9 -lcalico_ds9

all: $(addprefix $(OUT)/,$(addsuffix .nds,$(PROGS)))

define prog_rules
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c))

$(OUT)/$(1).elf: $$($(1)_OBJS) | $(OUT)
	$$(CC) $$(LDFLAGS) -Wl,-Map,$(BUILD)/$(1).map -o $$@ $$^ $$(LIBS)
endef
$(foreach p,$(PROGS),$(eval $(call prog_rules,$(p))))

# Fixed banner text: the same bytes on every build.
$(OUT)/%.nds: $(OUT)/%.elf
	ndstool -c $@ -9 $< -7 $(CALICO)/bin/ds7_maine.elf -b $(CALICO)/share/nds-icon.bmp "XBA $*;XRecomp test;homebrew" >/dev/null

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
.SECONDARY:
