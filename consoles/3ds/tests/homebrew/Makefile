# 3DS homebrew corpus (XBA-228). Runs inside the xba-3ds-homebrew image (tools/docker/3ds-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/3ds/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.3dsx (3dsxtool: our code with libctru, what the Homebrew Launcher loads) and
# <prog>.elf (the symbols, for debugging only: the runtime gets the .3dsx). No SMDH: the icon and title do not matter.

PROGS := hello
OUT ?= out
BUILD ?= build

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set: run inside the xba-3ds-homebrew image)
endif

CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
CTRULIB := $(DEVKITPRO)/libctru

# libctru's own flags (devkitPro's 3DS template): ARM state, hard-float VFPv2, soft thread pointer.
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -std=gnu11 -ffunction-sections -fdata-sections \
          -D__3DS__ -I$(CTRULIB)/include
LDFLAGS := -specs=3dsx.specs $(ARCH) -g -L$(CTRULIB)/lib
LIBS := -lctru -lm

all: $(addprefix $(OUT)/,$(addsuffix .3dsx,$(PROGS)))

define prog_rules
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c))

$(OUT)/$(1).elf: $$($(1)_OBJS) | $(OUT)
	$$(CC) $$(LDFLAGS) -Wl,-Map,$(BUILD)/$(1).map -o $$@ $$^ $$(LIBS)
endef
$(foreach p,$(PROGS),$(eval $(call prog_rules,$(p))))

$(OUT)/%.3dsx: $(OUT)/%.elf
	3dsxtool $< $@ >/dev/null

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
.SECONDARY:
