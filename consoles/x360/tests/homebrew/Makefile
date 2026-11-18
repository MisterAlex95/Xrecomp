# The Xbox 360 test titles (XBA-270), built by tools/consoles/build_homebrew.sh x360 in the xba-x360-homebrew image
# (devkitPPC): one ELF per program, linked by x360.ld; elf2xex.py (at test time, outside the image) packs each
# into a synthetic XEX2. No C library, no SDK: kernel.h declares the xboxkrnl exports the programs call.
OUT ?= out
BUILD ?= build
PROGRAMS := hello_c atomics kobjects
CC := powerpc-eabi-gcc
CFLAGS := -O2 -mcpu=970 -mpowerpc64 -maltivec -mabi=altivec -msdata=none -G 0 -ffreestanding -fno-builtin \
          -fno-stack-protector -fno-common -fno-asynchronous-unwind-tables -mno-eabi -Wall -Wextra
LDFLAGS := -nostdlib -nostartfiles -T x360.ld -Wl,--gc-sections -Wl,-e,_start

all: $(PROGRAMS:%=$(OUT)/%.elf)

$(OUT)/%.elf: %/main.c kernel.h x360.ld
	@mkdir -p $(OUT) $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $(BUILD)/$*.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/$*.o -lgcc

clean:
	rm -rf $(OUT) $(BUILD)
