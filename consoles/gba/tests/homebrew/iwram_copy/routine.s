@ GBA iwram_copy (XBA-149): a position-independent ARM routine that main.c copies from ROM into
@ IWRAM and EWRAM, patches, then calls. Our own code.
@   u32 pic_routine(u32 x) = ((x * 3) + IMM) ^ 0x5A5A0000, IMM = low byte of the word at patch_site.

    .syntax unified
    .cpu arm7tdmi
    .text
    .arm
    .align 2
    .global pic_routine, pic_routine_end, pic_patch_offset
    .type pic_routine, %function
pic_routine:
    add     r0, r0, r0, lsl #1
patch_site:
    add     r0, r0, #0x11           @ imm8 = low byte of this word, rewritten at run time
    ldr     r1, 1f                  @ PC-relative: copied along with the code
    eor     r0, r0, r1
    bx      lr
1:  .word   0x5A5A0000
pic_routine_end:

    .section .rodata
    .align 2
pic_patch_offset:
    .word   patch_site - pic_routine
