# GameCube homebrew corpus (XBA-225). Runs inside the xba-gc-homebrew image (tools/docker/gc-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/gc/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.dol (elf2dol: what the runtime loads) and <prog>.elf (symbols, for debugging
# only).

PROGS := hello padtest
OUT ?= out
BUILD ?= build

ifeq ($(strip $(DEVKITPPC)),)
$(error DEVKITPPC is not set: run inside the xba-gc-homebrew image)
endif

CC := $(DEVKITPPC)/bin/powerpc-eabi-gcc
LIBOGC := $(DEVKITPRO)/libogc

MACHDEP := -DGEKKO -mogc -mcpu=750 -meabi -mhard-float
CFLAGS := $(MACHDEP) -O2 -g -Wall -Wextra -Werror -std=gnu11 -I$(LIBOGC)/include
LDFLAGS := $(MACHDEP) -g -L$(LIBOGC)/lib/cube
LIBS := -logc -lm

all: $(addprefix $(OUT)/,$(addsuffix .dol,$(PROGS)))

define prog_rules
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c))

$(OUT)/$(1).elf: $$($(1)_OBJS) | $(OUT)
	$$(CC) $$(LDFLAGS) -Wl,-Map,$(BUILD)/$(1).map -o $$@ $$^ $$(LIBS)
endef
$(foreach p,$(PROGS),$(eval $(call prog_rules,$(p))))

$(OUT)/%.dol: $(OUT)/%.elf
	elf2dol $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
.SECONDARY:
