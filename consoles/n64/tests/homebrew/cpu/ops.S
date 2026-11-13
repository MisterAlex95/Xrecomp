# N64 cpu test: hand-written MIPS III (VR4300) sequences (XBA-149). Our own code.
# void mips64_tests(uint64_t *out) writes MIPS64_COUNT doublewords; expected values in main.c,
# each line below names its index. .set noreorder: every delay slot is ours. Big-endian.

    .set noreorder
    .set noat
    .set gp=64
    .set fp=64
    .text

    .macro OUT reg
    sd      \reg, 0($a0)
    daddiu  $a0, $a0, 8
    .endm

    .global mips64_tests
    .type mips64_tests, @function
mips64_tests:
    # --- 32-bit ops sign-extend into 64-bit registers
    lui     $t0, 0x8000
    OUT     $t0                         # 0  0xFFFFFFFF80000000
    li      $t0, 0x7FFFFFFF
    addiu   $t1, $t0, 1
    OUT     $t1                         # 1  0xFFFFFFFF80000000
    dli     $t0, 0x0000000080000000
    sll     $t1, $t0, 0
    OUT     $t1                         # 2  0xFFFFFFFF80000000

    # --- 64-bit add / sub (no trap)
    dli     $t0, 0x7FFFFFFFFFFFFFFF
    daddiu  $t1, $t0, 1
    OUT     $t1                         # 3  0x8000000000000000
    dsubu   $t1, $zero, $t0
    OUT     $t1                         # 4  0x8000000000000001
    dli     $t0, 0x00000000FFFFFFFF
    daddu   $t1, $t0, $t0
    OUT     $t1                         # 5  0x00000001FFFFFFFE

    # --- 64-bit shifts
    li      $t0, 0x12345678
    dsll32  $t1, $t0, 4
    OUT     $t1                         # 6  0x2345678000000000
    dsll    $t1, $t0, 12
    OUT     $t1                         # 7  0x0000012345678000
    dli     $t0, 0x8000000000000000
    dsra32  $t1, $t0, 4
    OUT     $t1                         # 8  0xFFFFFFFFF8000000
    dsrl32  $t1, $t0, 4
    OUT     $t1                         # 9  0x0000000008000000
    dsra    $t1, $t0, 8
    OUT     $t1                         # 10 0xFF80000000000000
    li      $t2, 68                     # DSxxV mask the amount to 6 bits: 4
    dsrav   $t1, $t0, $t2
    OUT     $t1                         # 11 0xF800000000000000
    li      $t0, 0x12345678
    dsllv   $t1, $t0, $t2
    OUT     $t1                         # 12 0x0000000123456780

    # --- 64-bit multiply / divide
    dli     $t0, -1
    li      $t1, 2
    dmultu  $t0, $t1
    mflo    $t2
    OUT     $t2                         # 13 0xFFFFFFFFFFFFFFFE
    mfhi    $t2
    OUT     $t2                         # 14 0x0000000000000001
    li      $t0, -3
    li      $t1, 5
    dmult   $t0, $t1
    mflo    $t2
    OUT     $t2                         # 15 0xFFFFFFFFFFFFFFF1
    mfhi    $t2
    OUT     $t2                         # 16 0xFFFFFFFFFFFFFFFF
    dli     $t0, -100
    li      $t1, 7
    ddiv    $zero, $t0, $t1
    mflo    $t2
    OUT     $t2                         # 17 -14
    mfhi    $t2
    OUT     $t2                         # 18 -2
    dli     $t0, -1
    li      $t1, 10
    ddivu   $zero, $t0, $t1
    mflo    $t2
    OUT     $t2                         # 19 0x1999999999999999
    mfhi    $t2
    OUT     $t2                         # 20 5
    li      $t0, 0x12345678             # 32-bit MULT: HI/LO sign-extended
    li      $t1, -0x65432110            # 0x9ABCDEF0, sign-extended (MULT needs it)
    mult    $t0, $t1
    mflo    $t2
    OUT     $t2                         # 21 0x00000000242D2080
    mfhi    $t2
    OUT     $t2                         # 22 0xFFFFFFFFF8CC93D6

    # --- loads / stores (big-endian)
    la      $t3, bytes_a                # 00 11 22 .. FF, 8-aligned
    ld      $t1, 0($t3)
    OUT     $t1                         # 23 0x0011223344556677
    lw      $t1, 8($t3)
    OUT     $t1                         # 24 0xFFFFFFFF8899AABB
    lwu     $t1, 8($t3)
    OUT     $t1                         # 25 0x000000008899AABB
    ldl     $t1, 1($t3)
    ldr     $t1, 8($t3)
    OUT     $t1                         # 26 0x1122334455667788
    lwl     $t1, 1($t3)
    lwr     $t1, 4($t3)
    OUT     $t1                         # 27 0x0000000011223344
    lb      $t1, 9($t3)
    OUT     $t1                         # 28 0xFFFFFFFFFFFFFF99
    lhu     $t1, 10($t3)
    OUT     $t1                         # 29 0x000000000000AABB
    la      $t3, scratch
    sd      $zero, 0($t3)
    sd      $zero, 8($t3)
    dli     $t0, 0x0102030405060708
    sdl     $t0, 3($t3)
    sdr     $t0, 10($t3)
    ld      $t1, 0($t3)
    OUT     $t1                         # 30 0x0000000102030405
    ld      $t1, 8($t3)
    OUT     $t1                         # 31 0x0607080000000000

    # --- 64-bit compares
    dli     $t0, 0x8000000000000000
    li      $t1, 1
    sltu    $t2, $t1, $t0
    OUT     $t2                         # 32 1
    slt     $t2, $t0, $t1
    OUT     $t2                         # 33 1

    # --- branch-likely: the delay slot is nullified when the branch is not taken
    move    $t0, $zero
    li      $t1, 1
    beql    $zero, $t1, 1f
    daddiu  $t0, $t0, 1                 # nullified
    daddiu  $t0, $t0, 0x10
1:  OUT     $t0                         # 34 0x10
    move    $t0, $zero
    beql    $zero, $zero, 1f
    daddiu  $t0, $t0, 1                 # executed
    daddiu  $t0, $t0, 0x10
1:  OUT     $t0                         # 35 0x1
    move    $t0, $zero
    bnel    $zero, $zero, 1f
    daddiu  $t0, $t0, 2                 # nullified
    daddiu  $t0, $t0, 0x20
1:  OUT     $t0                         # 36 0x20

    # --- FPU, double precision (constants from fpdata)
    la      $t3, fpdata
    ctc1    $zero, $31                  # round to nearest, flags clear
    ldc1    $f0, 0($t3)                 # 1.5
    ldc1    $f2, 8($t3)                 # 2.25
    add.d   $f4, $f0, $f2
    dmfc1   $t1, $f4
    OUT     $t1                         # 37 3.75
    ldc1    $f0, 16($t3)                # 3.0
    ldc1    $f2, 24($t3)                # -0.5
    mul.d   $f4, $f0, $f2
    dmfc1   $t1, $f4
    OUT     $t1                         # 38 -1.5
    ctc1    $zero, $31
    ldc1    $f0, 32($t3)                # 1.0
    div.d   $f4, $f0, $f0               # exact: no flag
    div.d   $f4, $f0, $f2               # 1.0 / -0.5 = -2 exact
    ldc1    $f2, 16($t3)                # 3.0
    div.d   $f4, $f0, $f2               # 1/3: inexact
    dmfc1   $t1, $f4
    OUT     $t1                         # 39 0x3FD5555555555555
    cfc1    $t1, $31
    OUT     $t1                         # 40 FCSR = 0x1004 (cause I, flag I)
    ctc1    $zero, $31
    ldc1    $f0, 40($t3)                # 2.0
    sqrt.d  $f4, $f0
    dmfc1   $t1, $f4
    OUT     $t1                         # 41 0x3FF6A09E667F3BCD

    # --- conversions and rounding modes
    ldc1    $f0, 48($t3)                # 2.5
    cvt.w.d $f4, $f0                    # RM = nearest even: 2
    mfc1    $t1, $f4
    OUT     $t1                         # 42 2
    li      $t2, 2                      # RM = toward +inf
    ctc1    $t2, $31
    cvt.w.d $f4, $f0
    mfc1    $t1, $f4
    OUT     $t1                         # 43 3
    li      $t2, 3                      # RM = toward -inf
    ctc1    $t2, $31
    ldc1    $f2, 56($t3)                # -2.5
    cvt.w.d $f4, $f2
    mfc1    $t1, $f4
    OUT     $t1                         # 44 -3
    ctc1    $zero, $31
    ldc1    $f0, 64($t3)                # -2.7
    trunc.w.d $f4, $f0
    mfc1    $t1, $f4
    OUT     $t1                         # 45 -2
    ldc1    $f0, 72($t3)                # 3.5
    round.w.d $f4, $f0
    mfc1    $t1, $f4
    OUT     $t1                         # 46 4
    ldc1    $f0, 24($t3)                # -0.5
    floor.w.d $f4, $f0
    mfc1    $t1, $f4
    OUT     $t1                         # 47 -1
    ldc1    $f0, 80($t3)                # 1.1
    ceil.w.d $f4, $f0
    mfc1    $t1, $f4
    OUT     $t1                         # 48 2
    dli     $t0, -123456789012
    dmtc1   $t0, $f0
    cvt.d.l $f4, $f0
    dmfc1   $t1, $f4
    OUT     $t1                         # 49 double(-123456789012)
    ldc1    $f0, 32($t3)                # 1.0
    ldc1    $f2, 40($t3)                # 2.0
    trunc.l.d $f4, $f2
    dmfc1   $t1, $f4
    OUT     $t1                         # 50 2

    # --- single precision and compares
    lwc1    $f0, 88($t3)                # 1.5f
    lwc1    $f2, 92($t3)                # 4.0f
    mul.s   $f4, $f0, $f2
    mfc1    $t1, $f4
    OUT     $t1                         # 51 6.0f = 0x40C00000
    cvt.d.s $f6, $f4
    dmfc1   $t1, $f6
    OUT     $t1                         # 52 6.0 = 0x4018000000000000
    ldc1    $f0, 32($t3)                # 1.0
    ldc1    $f2, 40($t3)                # 2.0
    li      $t1, 0
    c.lt.d  $f0, $f2
    nop
    bc1t    1f
    nop
    li      $t1, 0x55
1:  OUT     $t1                         # 53 0 (branch taken)
    li      $t1, 0
    c.lt.d  $f2, $f0
    nop
    bc1fl   1f                          # likely: taken, slot runs
    ori     $t1, $t1, 1
    ori     $t1, $t1, 0x10
1:  OUT     $t1                         # 54 1

    # --- counted loop with a 64-bit accumulator
    move    $t2, $zero
    dli     $t1, 100
2:  dsll32  $t0, $t1, 0                 # t1 << 32
    daddu   $t2, $t2, $t0
    daddiu  $t1, $t1, -1
    bnez    $t1, 2b
    nop
    OUT     $t2                         # 55 5050 << 32

    ctc1    $zero, $31
    jr      $ra
    nop

    .data
    .align 3
bytes_a:
    .byte   0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77
    .byte   0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
fpdata:
    .double 1.5, 2.25, 3.0, -0.5, 1.0, 2.0, 2.5, -2.5, -2.7, 3.5, 1.1
    .float  1.5, 4.0

    .bss
    .align 3
scratch:
    .space  16
