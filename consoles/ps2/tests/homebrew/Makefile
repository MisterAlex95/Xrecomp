# PS2 homebrew corpus (XBA-226). Runs inside the xba-ps2-homebrew image (tools/docker/ps2-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh:
#   make -C consoles/ps2/tests/homebrew OUT=/out BUILD=/tmp/build
# Each program directory gives <prog>.elf: the EE executable as a PS2 boots it (PS2SDK's crt0 and linkfile, loaded at
# 0x00100000, newlib and libkernel linked statically). The flags are PS2SDK's (samples/Makefile.eeglobal): -O2 -G0.

PROGS := hello
OUT ?= out
BUILD ?= build

ifeq ($(strip $(PS2SDK)),)
$(error PS2SDK is not set: run inside the xba-ps2-homebrew image)
endif

EE_PREFIX := mips64r5900el-ps2-elf-
CC := $(EE_PREFIX)gcc
CFLAGS := -D_EE -G0 -O2 -Wall -I$(PS2SDK)/ee/include -I$(PS2SDK)/common/include
LDFLAGS := -T$(PS2SDK)/ee/startup/linkfile -L$(PS2SDK)/ee/lib -Wl,-zmax-page-size=128

OUT := $(abspath $(OUT))
BUILD := $(abspath $(BUILD))

all: $(addprefix $(OUT)/,$(addsuffix .elf,$(PROGS)))

$(BUILD)/%.o: %/main.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/%.elf: $(BUILD)/%.o | $(OUT)
	$(CC) $(LDFLAGS) -O2 -o $@ $<

$(OUT) $(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT)

.PHONY: all clean
