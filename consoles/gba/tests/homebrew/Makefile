# GBA homebrew corpus (XBA-149). Runs inside the xba-gba-homebrew image (tools/docker/gba-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/gba/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.gba (cartridge image, header fixed by gbafix) and <prog>.elf
# (symbols, for debugging only: the runtime gets the .gba). *.arm.c files are compiled as ARM, the
# rest of the C as Thumb; *.s files choose their own state.

PROGS := hello cpu timer_irq gfx iwram_copy pad sound
OUT ?= out
BUILD ?= build

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set: run inside the xba-gba-homebrew image)
endif

CC := $(DEVKITARM)/bin/arm-none-eabi-gcc
OBJCOPY := $(DEVKITARM)/bin/arm-none-eabi-objcopy
TONC := $(DEVKITPRO)/libtonc

ARCH := -mcpu=arm7tdmi -mtune=arm7tdmi -mthumb-interwork
CFLAGS := $(ARCH) -O2 -g -Wall -Wextra -Werror -std=gnu11 -ffunction-sections -fdata-sections \
          -I$(TONC)/include -Icommon
ASFLAGS := $(ARCH) -g
LIBS := -ltonc

all: $(addprefix $(OUT)/,$(addsuffix .gba,$(PROGS)))

define prog_rules
$(1)_C := $$(filter-out %.arm.c,$$(wildcard $(1)/*.c))
$(1)_ARMC := $$(wildcard $(1)/*.arm.c)
$(1)_S := $$(wildcard $(1)/*.s)
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/%.o,$$($(1)_C)) \
             $$(patsubst %.arm.c,$(BUILD)/%.arm.o,$$($(1)_ARMC)) \
             $$(patsubst %.s,$(BUILD)/%.o,$$($(1)_S))

$(OUT)/$(1).elf: $$($(1)_OBJS) | $(OUT)
	$$(CC) $$(ARCH) -specs=gba.specs -L$$(TONC)/lib -Wl,-Map,$(BUILD)/$(1).map -o $$@ $$^ $$(LIBS)
endef
$(foreach p,$(PROGS),$(eval $(call prog_rules,$(p))))

$(OUT)/%.gba: $(OUT)/%.elf
	$(OBJCOPY) -O binary $< $@
	gbafix -t$(shell echo XBA$* | tr a-z A-Z | cut -c1-12) -cXBAT -m00 $@ >/dev/null

$(BUILD)/%.arm.o: %.arm.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -marm -c -o $@ $<

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -mthumb -c -o $@ $<

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -x assembler-with-cpp -c -o $@ $<

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
.SECONDARY:
