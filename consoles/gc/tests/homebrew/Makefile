# GameCube homebrew corpus (XBA-225). Runs inside the xba-gc-homebrew image (tools/docker/gc-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/gc/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.dol (elf2dol: what the runtime loads) and <prog>.elf (symbols, for debugging
# only).

PROGS := hello padtest asnd gxtri gxtex gxtev gxcopy gxind gxfog
# Programs with data made at build time: <prog>/mktex.py OUTDIR writes OUTDIR/textures.c / .h (gxtex: our pictures
# through gxtexconv, XBA-262).
GEN_PROGS := gxtex
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
asnd_LIBS := -lasnd   # libogc's ASND (XBA-263), ahead of -logc

all: $(addprefix $(OUT)/,$(addsuffix .dol,$(PROGS)))

define prog_rules
$(1)_OBJS := $$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c))
ifneq ($$(filter $(1),$(GEN_PROGS)),)
$(1)_OBJS += $(BUILD)/$(1)/gen/textures.o
$(BUILD)/$(1)/gen/textures.c: $(1)/mktex.py
	python3 $(1)/mktex.py $(BUILD)/$(1)/gen
$(BUILD)/$(1)/gen/textures.o: $(BUILD)/$(1)/gen/textures.c
	$$(CC) $$(CFLAGS) -c -o $$@ $$<
$$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c)): $(BUILD)/$(1)/gen/textures.c
$$(patsubst %.c,$(BUILD)/%.o,$$(wildcard $(1)/*.c)): CFLAGS += -I$(BUILD)/$(1)/gen
endif

$(OUT)/$(1).elf: $$($(1)_OBJS) | $(OUT)
	$$(CC) $$(LDFLAGS) -Wl,-Map,$(BUILD)/$(1).map -o $$@ $$^ $$($(1)_LIBS) $$(LIBS)
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
