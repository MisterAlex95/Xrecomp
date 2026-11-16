# Dreamcast homebrew corpus (XBA-227). Runs inside the xba-dc-homebrew image (tools/docker/dc-homebrew.Dockerfile,
# KallistiOS's own toolchain image), normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/dc/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.elf (what the spike loads: entry and symbols) and <prog>.bin (the same program
# as a raw, unscrambled binary for 0x8C010000: what a disc's 1ST_READ.BIN holds before scrambling).

PROGS := hello
OUT ?= out
BUILD ?= build

ifeq ($(strip $(KOS_BASE)),)
$(error KOS_BASE is not set: run inside the xba-dc-homebrew image)
endif

all: $(addprefix $(OUT)/,$(addsuffix .elf,$(PROGS))) $(addprefix $(OUT)/,$(addsuffix .bin,$(PROGS)))

define prog_rules
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c))

$(OUT)/$(1).elf: $$($(1)_OBJS) | $(OUT)
	kos-cc -Wl,-Map,$(BUILD)/$(1).map -o $$@ $$^
endef
$(foreach p,$(PROGS),$(eval $(call prog_rules,$(p))))

$(OUT)/%.bin: $(OUT)/%.elf
	$(KOS_OBJCOPY) -O binary $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	kos-cc -Wextra -Werror -c -o $@ $<

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
.SECONDARY:
