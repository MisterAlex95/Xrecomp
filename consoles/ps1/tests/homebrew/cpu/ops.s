# PS1 cpu test: hand-written MIPS R3000A sequences (XBA-149). Our own code.
# void mips_tests(uint32_t *out) writes MIPS_COUNT words; expected values in main.c, each line
# below names its table index. .set noreorder: the assembler adds no nops, every delay slot is ours.

    .set noreorder
    .set noat
    .text

    .macro OUT reg
    sw      \reg, 0($a0)
    addiu   $a0, $a0, 4
    .endm

    .global mips_tests
    .type mips_tests, @function
mips_tests:
    addiu   $sp, $sp, -8
    sw      $ra, 0($sp)

    # --- branch delay slots
    li      $t0, 0
    beq     $zero, $zero, 1f
    addiu   $t0, $t0, 1             # delay slot: runs
    addiu   $t0, $t0, 0x10          # skipped
1:  OUT     $t0                     # 0  0x1
    li      $t0, 0
    bne     $zero, $zero, 1f
    addiu   $t0, $t0, 2             # delay slot of a not-taken branch: runs too
    addiu   $t0, $t0, 0x20
1:  OUT     $t0                     # 1  0x22
    li      $t0, 0
    jal     leaf_add5
    addiu   $t0, $t0, 0x100         # delay slot of JAL runs before the callee
    OUT     $t0                     # 2  0x105

    # --- load delay slot (R3000A: the next instruction still sees the old value)
    la      $t1, word_a
    li      $t0, 0x55
    lw      $t0, 0($t1)
    move    $t2, $t0                # reads the old $t0
    OUT     $t2                     # 3  0x55
    OUT     $t0                     # 4  0x11223344

    # --- multiply / divide
    li      $t0, 0x12345678
    li      $t1, 0x9ABCDEF0
    mult    $t0, $t1
    mflo    $t2
    OUT     $t2                     # 5  0x242D2080
    mfhi    $t2
    OUT     $t2                     # 6  0xF8CC93D6
    multu   $t0, $t1
    mflo    $t2
    OUT     $t2                     # 7  0x242D2080
    mfhi    $t2
    OUT     $t2                     # 8  0x0B00EA4E
    li      $t0, -100
    li      $t1, 7
    div     $zero, $t0, $t1         # raw DIV: no assembler divide-by-zero trap
    mflo    $t2
    OUT     $t2                     # 9  -14
    mfhi    $t2
    OUT     $t2                     # 10 -2
    li      $t0, 0xFFFFFFF0
    divu    $zero, $t0, $t1
    mflo    $t2
    OUT     $t2                     # 11 613566754
    mfhi    $t2
    OUT     $t2                     # 12 2
    li      $t0, 5
    div     $zero, $t0, $zero       # divide by zero, positive: LO = -1, HI = dividend
    mflo    $t2
    OUT     $t2                     # 13 0xFFFFFFFF
    mfhi    $t2
    OUT     $t2                     # 14 5
    li      $t0, -5
    div     $zero, $t0, $zero       # divide by zero, negative: LO = 1, HI = dividend
    mflo    $t2
    OUT     $t2                     # 15 1
    mfhi    $t2
    OUT     $t2                     # 16 -5
    li      $t0, 5
    divu    $zero, $t0, $zero       # unsigned divide by zero: LO = 0xFFFFFFFF, HI = dividend
    mflo    $t2
    OUT     $t2                     # 17 0xFFFFFFFF
    mfhi    $t2
    OUT     $t2                     # 18 5
    li      $t0, 0x80000000
    li      $t1, -1
    div     $zero, $t0, $t1         # overflow: LO = 0x80000000, HI = 0
    mflo    $t2
    OUT     $t2                     # 19 0x80000000
    mfhi    $t2
    OUT     $t2                     # 20 0

    # --- unaligned LWL / LWR / SWL / SWR (little-endian)
    la      $t1, bytes_a            # 00 11 22 33 44 55 66 77
    lwr     $t0, 1($t1)
    lwl     $t0, 4($t1)             # back-to-back pair, as compilers emit it
    nop
    OUT     $t0                     # 21 0x44332211
    li      $t0, 0xAABBCCDD
    lwr     $t0, 1($t1)
    nop
    OUT     $t0                     # 22 0xAA332211
    li      $t0, 0xAABBCCDD
    lwl     $t0, 1($t1)
    nop
    OUT     $t0                     # 23 0x1100CCDD
    la      $t3, scratch
    li      $t0, 0xDDCCBBAA
    swr     $t0, 1($t3)
    swl     $t0, 4($t3)
    lw      $t2, 0($t3)
    nop
    OUT     $t2                     # 24 0xCCBBAA00
    lw      $t2, 4($t3)
    nop
    OUT     $t2                     # 25 0x000000DD

    # --- byte / halfword loads
    la      $t1, word_b             # 0x8899AABB
    lb      $t2, 1($t1)
    nop
    OUT     $t2                     # 26 0xFFFFFFAA
    lbu     $t2, 1($t1)
    nop
    OUT     $t2                     # 27 0xAA
    lh      $t2, 2($t1)
    nop
    OUT     $t2                     # 28 0xFFFF8899
    lhu     $t2, 2($t1)
    nop
    OUT     $t2                     # 29 0x8899

    # --- set-on-less-than
    li      $t0, -1
    li      $t1, 1
    slt     $t2, $t0, $t1
    OUT     $t2                     # 30 1
    sltu    $t2, $t0, $t1
    OUT     $t2                     # 31 0
    sltiu   $t2, $t1, -1            # immediate sign-extended, then compared unsigned
    OUT     $t2                     # 32 1
    slti    $t2, $t0, -2
    OUT     $t2                     # 33 0

    # --- shifts
    li      $t0, 0x80000001
    sra     $t2, $t0, 4
    OUT     $t2                     # 34 0xF8000000
    srl     $t2, $t0, 4
    OUT     $t2                     # 35 0x08000000
    sll     $t2, $t0, 31
    OUT     $t2                     # 36 0x80000000
    li      $t1, 33
    sllv    $t2, $t0, $t1           # amount masked to 5 bits: 1
    OUT     $t2                     # 37 0x00000002
    li      $t1, 36
    srav    $t2, $t0, $t1           # 4
    OUT     $t2                     # 38 0xF8000000

    # --- logic and immediates
    li      $t0, 0xF0F0F0F0
    li      $t1, 0xFF00FF00
    nor     $t2, $t0, $t1
    OUT     $t2                     # 39 0x000F000F
    xor     $t2, $t0, $t1
    OUT     $t2                     # 40 0x0FF00FF0
    andi    $t2, $t0, 0xFFFF        # zero-extended immediate
    OUT     $t2                     # 41 0xF0F0
    ori     $t2, $zero, 0x8000
    OUT     $t2                     # 42 0x8000
    lui     $t2, 0x8000
    OUT     $t2                     # 43 0x80000000

    # --- wrapping arithmetic (no overflow trap)
    li      $t0, 0x7FFFFFFF
    addiu   $t2, $t0, 1
    OUT     $t2                     # 44 0x80000000
    subu    $t2, $zero, $t0
    OUT     $t2                     # 45 0x80000001

    # --- BLTZAL links even when not taken
    li      $t0, 1
    bltzal  $t0, 9f
    nop
2:  la      $t1, 2b
    subu    $t2, $ra, $t1
    OUT     $t2                     # 46 0

    # --- JALR
    la      $t9, leaf_add5
    li      $t0, 0
    jalr    $t9
    addiu   $t0, $t0, 0x200
    OUT     $t0                     # 47 0x205

    # --- counted loop
    li      $t2, 0
    li      $t1, 100
3:  addu    $t2, $t2, $t1
    addiu   $t1, $t1, -1
    bnez    $t1, 3b
    nop
    OUT     $t2                     # 48 5050

    # --- HI / LO moves
    li      $t0, 0xABCD
    mthi    $t0
    li      $t1, 0x1234
    mtlo    $t1
    mfhi    $t2
    OUT     $t2                     # 49 0xABCD
    mflo    $t2
    OUT     $t2                     # 50 0x1234

    lw      $ra, 0($sp)
    nop
    jr      $ra
    addiu   $sp, $sp, 8

9:  break   0                       # never reached
    nop

    .type leaf_add5, @function
leaf_add5:
    jr      $ra
    addiu   $t0, $t0, 5             # delay slot of JR

    .data
    .align 2
word_a:
    .word   0x11223344
word_b:
    .word   0x8899AABB
bytes_a:
    .byte   0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77

    .bss
    .align 2
scratch:
    .space  16
