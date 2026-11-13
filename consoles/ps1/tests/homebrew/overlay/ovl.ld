/* Overlays of the PS1 overlay test (XBA-149): flat binaries linked at 0x80100000, entry first. */
OUTPUT_FORMAT("elf32-littlemips")
ENTRY(ovl_entry)
SECTIONS
{
    . = 0x80100000;
    .text : { KEEP(*(.text.entry)) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    /DISCARD/ : { *(.MIPS.abiflags) *(.reginfo) *(.comment) *(.pdr) *(.gnu.attributes) }
}
