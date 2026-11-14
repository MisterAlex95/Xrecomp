@ GBA cpu test: hand-written ARM and Thumb sequences (XBA-149). Our own code.
@ arm_tests(u32 *out) writes ARM_COUNT words, thumb_tests(u32 *out) THUMB_COUNT words.
@ The expected values live in main.c; each line below names its table index.
@ Both functions are called from Thumb C code (linker interworking veneers), and each one
@ calls a leaf in the other instruction set with BX, so every ARM<->Thumb path is used.

    .syntax unified
    .cpu arm7tdmi
    .section .rodata
    .align 2
testdata:
    .word 0x8899AABB, 0xCCDDEEFF

    .bss
    .align 2
scratch:
    .space 64

@ ---------------------------------------------------------------- ARM -------------
    .text
    .arm
    .align 2
    .global arm_tests
    .type arm_tests, %function
arm_tests:
    push    {r4-r11, lr}

    @ barrel shifter, immediate amounts
    ldr     r1, =0x80000001
    mov     r2, r1, lsl #1          @ 0  0x00000002
    str     r2, [r0], #4
    mov     r2, r1, lsr #1          @ 1  0x40000000
    str     r2, [r0], #4
    mov     r2, r1, asr #1          @ 2  0xC0000000
    str     r2, [r0], #4
    mov     r2, r1, ror #4          @ 3  0x18000000
    str     r2, [r0], #4
    movs    r2, r1, lsr #1          @ C = 1
    mov     r2, r1, rrx             @ 4  0xC0000000
    str     r2, [r0], #4

    @ barrel shifter, register amounts (>= 32 is legal on ARM)
    mov     r3, #36
    mov     r2, r1, lsl r3          @ 5  0
    str     r2, [r0], #4
    mov     r3, #40
    mov     r2, r1, asr r3          @ 6  0xFFFFFFFF
    str     r2, [r0], #4
    mov     r3, #36
    mov     r2, r1, ror r3          @ 7  0x18000000
    str     r2, [r0], #4

    @ flags
    msr     cpsr_f, #0
    mvn     r1, #0
    adds    r2, r1, #1              @ 8  0
    str     r2, [r0], #4
    mrs     r2, cpsr
    and     r2, r2, #0xF0000000     @ 9  0x60000000 (Z C)
    str     r2, [r0], #4
    ldr     r1, =0x7FFFFFFF
    adds    r2, r1, #1              @ 10 0x80000000
    str     r2, [r0], #4
    mrs     r2, cpsr
    and     r2, r2, #0xF0000000     @ 11 0x90000000 (N V)
    str     r2, [r0], #4
    mov     r1, #0
    subs    r2, r1, #1              @ 12 0xFFFFFFFF
    str     r2, [r0], #4
    mrs     r2, cpsr
    and     r2, r2, #0xF0000000     @ 13 0x80000000 (N, borrow so C = 0)
    str     r2, [r0], #4

    @ carry-in arithmetic
    subs    r3, r3, r3              @ C = 1
    mov     r1, #0x10
    adc     r2, r1, #0x20           @ 14 0x31
    str     r2, [r0], #4
    sbc     r2, r1, #1              @ 15 0x0F (C = 1: no extra borrow)
    str     r2, [r0], #4
    mov     r3, #0
    cmp     r3, #1                  @ C = 0
    sbc     r2, r1, #1              @ 16 0x0E
    str     r2, [r0], #4
    rsc     r2, r1, #0x100          @ 17 0xEF
    str     r2, [r0], #4

    @ multiplies
    ldr     r1, =0x12345678
    ldr     r2, =0x9ABCDEF0
    mul     r3, r1, r2              @ 18
    str     r3, [r0], #4
    umull   r4, r5, r1, r2          @ 19 lo, 20 hi
    str     r4, [r0], #4
    str     r5, [r0], #4
    smull   r6, r7, r1, r2          @ 21 lo, 22 hi
    str     r6, [r0], #4
    str     r7, [r0], #4
    mla     r3, r1, r2, r1          @ 23
    str     r3, [r0], #4
    umlal   r4, r5, r1, r2          @ 24 lo, 25 hi (2 * unsigned product)
    str     r4, [r0], #4
    str     r5, [r0], #4
    smlal   r6, r7, r1, r2          @ 26 lo, 27 hi (2 * signed product)
    str     r6, [r0], #4
    str     r7, [r0], #4

    @ loads: sign extension, halfwords, unaligned word rotation (ARMv4)
    ldr     r3, =testdata
    mov     r4, r3
    ldrb    r2, [r3, #1]            @ 28 0xAA
    str     r2, [r0], #4
    ldrsb   r2, [r3, #1]            @ 29 0xFFFFFFAA
    str     r2, [r0], #4
    ldrh    r2, [r3, #2]            @ 30 0x8899
    str     r2, [r0], #4
    ldrsh   r2, [r3, #2]            @ 31 0xFFFF8899
    str     r2, [r0], #4
    ldr     r2, [r3, #1]            @ 32 0xBB8899AA (word at +0 rotated right by 8)
    str     r2, [r0], #4
    ldr     r2, [r3, #4]!           @ 33 0xCCDDEEFF, pre-index writeback
    str     r2, [r0], #4
    sub     r2, r3, r4              @ 34 4
    str     r2, [r0], #4

    @ block transfers
    ldr     r8, =scratch
    mov     r9, r8
    mov     r4, #1
    mov     r5, #2
    mov     r6, #3
    mov     r7, #4
    stmia   r8!, {r4-r7}
    sub     r2, r8, r9              @ 35 16
    str     r2, [r0], #4
    ldmdb   r8!, {r1, r2}           @ r1 = 3, r2 = 4
    orr     r2, r1, r2, lsl #8      @ 36 0x403
    str     r2, [r0], #4
    sub     r2, r8, r9              @ 37 8
    str     r2, [r0], #4

    @ swap
    mov     r1, #0x55
    swp     r2, r1, [r9]            @ r2 = 1, mem = 0x55
    ldr     r3, [r9]
    orr     r2, r2, r3, lsl #8      @ 38 0x5501
    str     r2, [r0], #4

    @ conditional execution
    mov     r1, #5
    cmp     r1, #5
    moveq   r2, #1
    movne   r2, #2
    addgt   r2, r2, #0x10
    addge   r2, r2, #0x100          @ 39 0x101
    str     r2, [r0], #4
    mvn     r1, #2                  @ -3
    cmp     r1, #2
    movlt   r2, #7
    movge   r2, #8
    orrhi   r2, r2, #0x70           @ 40 0x77 (signed less, unsigned higher)
    str     r2, [r0], #4

    @ MSR / MRS flag field
    msr     cpsr_f, #0xF0000000
    mrs     r2, cpsr
    and     r2, r2, #0xF0000000     @ 41 0xF0000000
    str     r2, [r0], #4

    @ ARM -> Thumb leaf -> ARM
    mov     r2, #0x40
    ldr     r1, =thumb_leaf         @ Thumb bit set by the linker
    mov     lr, pc
    bx      r1
    str     r2, [r0], #4            @ 42 0x43

    @ shifter carry-out on a logical op
    msr     cpsr_f, #0
    mov     r1, #0x80000000
    movs    r2, r1, lsl #1
    mrs     r2, cpsr
    and     r2, r2, #0xF0000000     @ 43 0x60000000 (Z C)
    str     r2, [r0], #4

    @ counted loop
    mov     r2, #0
    mov     r3, #100
1:  add     r2, r2, r3
    subs    r3, r3, #1
    bne     1b                      @ 44 5050 = 0x13BA
    str     r2, [r0], #4

    pop     {r4-r11, lr}
    bx      lr
    .ltorg

    .arm
    .align 2
    .type arm_leaf, %function
arm_leaf:                          @ called from Thumb with BX
    add     r2, r2, #5
    bx      lr

@ ---------------------------------------------------------------- Thumb -----------
    .thumb
    .align 2
    .type thumb_leaf, %function
    .thumb_func
thumb_leaf:                        @ called from ARM with BX
    adds    r2, r2, #3
    bx      lr

    .thumb_func
call_via_r3:
    bx      r3

    .align 2
    .global thumb_tests
    .type thumb_tests, %function
    .thumb_func
thumb_tests:
    push    {r4-r7, lr}
    mov     r4, r8
    push    {r4}

    @ STMIA r0!, {rN} stores without touching the flags.
    ldr     r1, =0x80000001
    lsls    r2, r1, #1              @ 0  0x00000002
    stmia   r0!, {r2}
    lsrs    r2, r1, #1              @ 1  0x40000000
    stmia   r0!, {r2}
    asrs    r2, r1, #1              @ 2  0xC0000000
    stmia   r0!, {r2}
    movs    r3, #4
    movs    r2, r1
    rors    r2, r3                  @ 3  0x18000000
    stmia   r0!, {r2}

    @ carry chains
    movs    r2, #0
    mvns    r2, r2
    adds    r2, #1                  @ 0, C = 1
    movs    r3, #5                  @ MOVS imm leaves C alone
    adcs    r3, r3                  @ 4  11 (C = 0 after)
    stmia   r0!, {r3}
    movs    r2, #20
    movs    r3, #3
    sbcs    r2, r3                  @ 5  16 (20 - 3 - 1)
    stmia   r0!, {r2}
    movs    r3, #5
    negs    r2, r3                  @ 6  0xFFFFFFFB
    stmia   r0!, {r2}

    ldr     r2, =0x12345678
    ldr     r3, =0x9ABCDEF0
    muls    r2, r3                  @ 7  same as ARM 18
    stmia   r0!, {r2}

    @ loads
    ldr     r4, =testdata
    movs    r5, #1
    ldrsb   r2, [r4, r5]            @ 8  0xFFFFFFAA
    stmia   r0!, {r2}
    movs    r5, #2
    ldrsh   r2, [r4, r5]            @ 9  0xFFFF8899
    stmia   r0!, {r2}
    ldrh    r2, [r4, #2]            @ 10 0x8899
    stmia   r0!, {r2}
    ldrb    r2, [r4, #7]            @ 11 0xCC
    stmia   r0!, {r2}

    @ stack and block transfers
    movs    r2, #1
    movs    r3, #2
    push    {r2, r3}
    pop     {r5, r6}
    lsls    r6, r6, #8
    orrs    r5, r6                  @ 12 0x201
    stmia   r0!, {r5}
    ldr     r4, =scratch
    movs    r5, #7
    movs    r6, #9
    stmia   r4!, {r5, r6}
    subs    r4, #8
    ldmia   r4!, {r2, r3}
    adds    r2, r3                  @ 13 16
    stmia   r0!, {r2}

    @ branches
    movs    r2, #0
    movs    r3, #10
1:  adds    r2, r3
    subs    r3, #1
    bne     1b                      @ 14 55
    stmia   r0!, {r2}
    movs    r3, #0
    subs    r3, #3                  @ -3
    cmp     r3, #2
    blt     2f
    movs    r2, #1
    b       3f
2:  movs    r2, #2                  @ 15 2
3:  stmia   r0!, {r2}
    cmp     r3, #2
    bhi     4f
    movs    r2, #4
    b       5f
4:  movs    r2, #3                  @ 16 3
5:  stmia   r0!, {r2}

    @ high registers
    mov     r8, r1
    add     r8, r8                  @ 0x80000001 * 2
    mov     r2, r8                  @ 17 2
    stmia   r0!, {r2}

    @ logic
    ldr     r2, =0xF0F0F0F0
    ldr     r3, =0xFF00FF00
    movs    r5, r2
    ands    r5, r3                  @ 18 0xF000F000
    stmia   r0!, {r5}
    movs    r5, r2
    eors    r5, r3                  @ 19 0x0FF00FF0
    stmia   r0!, {r5}
    movs    r5, r2
    bics    r5, r3                  @ 20 0x00F000F0
    stmia   r0!, {r5}
    mvns    r5, r2                  @ 21 0x0F0F0F0F
    stmia   r0!, {r5}

    @ Thumb -> ARM leaf -> Thumb
    ldr     r3, =arm_leaf
    movs    r2, #0x40
    bl      call_via_r3
    stmia   r0!, {r2}               @ 22 0x45

    @ register shifts >= 32
    movs    r3, #33
    movs    r2, r1
    lsls    r2, r3                  @ 23 0
    stmia   r0!, {r2}
    movs    r3, #40
    movs    r2, r1
    asrs    r2, r3                  @ 24 0xFFFFFFFF
    stmia   r0!, {r2}

    pop     {r4}
    mov     r8, r4
    pop     {r4-r7}
    pop     {r3}                    @ ARMv4T: POP {PC} does not interwork, BX does
    bx      r3
    .ltorg
