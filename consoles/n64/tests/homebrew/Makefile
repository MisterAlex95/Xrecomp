# N64 homebrew corpus (XBA-149). Runs inside the xba-n64-homebrew image (tools/docker/n64-homebrew.Dockerfile),
# normally through tools/consoles/build_homebrew.sh, from a writable copy of this directory:
#   make -j4 BUILD_DIR=/tmp/build
# Each program gives <prog>.z64 (big-endian ROM: libdragon IPL3 + the program ELF) and
# $(BUILD_DIR)/<prog>.elf. N64_ROM_ELFCOMPRESS=0 keeps the ELF in the ROM uncompressed, so a parser
# can read the code straight from the .z64.

PROGS := hello cpu timer_irq gfx
BUILD_DIR ?= build
SOURCE_DIR := .
N64_ROM_ELFCOMPRESS := 0

include $(N64_INST)/include/n64.mk

N64_CFLAGS += -Werror -Icommon

all: $(addsuffix .z64,$(PROGS))

$(BUILD_DIR)/hello.elf: $(BUILD_DIR)/hello/main.o
$(BUILD_DIR)/cpu.elf: $(BUILD_DIR)/cpu/main.o $(BUILD_DIR)/cpu/ops.o
$(BUILD_DIR)/timer_irq.elf: $(BUILD_DIR)/timer_irq/main.o
$(BUILD_DIR)/gfx.elf: $(BUILD_DIR)/gfx/main.o

hello.z64: N64_ROM_TITLE = "XBA hello"
cpu.z64: N64_ROM_TITLE = "XBA cpu"
timer_irq.z64: N64_ROM_TITLE = "XBA timer_irq"
gfx.z64: N64_ROM_TITLE = "XBA gfx"

clean:
	rm -rf $(BUILD_DIR) *.z64

.PHONY: all clean
