# Benchmark corpus for the lifted code (XBA-37, XBA-38): synthetic x86 of our
# own, shaped like the hot code of a title -- flag-heavy integer loops, memory
# copies through guest memory, x87 and SSE float loops, indirect calls through
# the dispatch, a call-heavy recursion, byte code with partial registers,
# direct calls into small functions that call through a table (a C++ method
# making a virtual call: each may reach a kernel call, XBA-247).
#
# Every workload is cdecl: `uint32_t w_xxx(uint32_t n)`, n = iterations, the
# result in eax (a checksum the bench compares across builds). Data are
# globals at absolute addresses, as a title's are. _start calls each workload
# once so the static analysis finds them all as functions.
#
# Assembled by tools/bench/__main__.py with `as --32` and linked at the
# synthetic XBE's text address (ld -Ttext); nothing here comes from a game.

    .intel_syntax noprefix
    .code32
    .text
    .globl _start
_start:
    push ebp
    mov ebp, esp
    push 1
# @copies: each copy's _start_<k> is called here
    call w_int
    call w_mem
    call w_rep
    call w_bytes
    call w_x87
    call w_sse
    call w_icall
    call w_fib
    call w_vcall
    call w_lock
    call w_stack
    call w_stkmix
    call w_leaf0
    call w_leaf1
    call w_leaf2
    call w_leaf3
    add esp, 4
spin:
    jmp spin

# ── integer: LCG, compare/branch, adc/sbb chain, setcc, cmov, dec/jnz ──
    .globl w_int
w_int:
    push ebx
    push esi
    push edi
    mov ecx, [esp+16]
    xor eax, eax
    mov edx, 0x12345678
    xor esi, esi
    xor edi, edi
1:
    imul edx, edx, 1103515245
    add edx, 12345
    mov ebx, edx
    shr ebx, 16
    cmp ebx, 0x4000
    jb 2f
    test bl, 1
    jz 3f
    add eax, ebx
    adc esi, 0
    jmp 4f
2:
    sub eax, ebx
    sbb esi, 0
    jmp 4f
3:
    xor eax, ebx
    cmp eax, edx
    setl bl
    movzx ebx, bl
    add edi, ebx
4:
    cmp eax, 0x40000000
    cmovg eax, edi
    test eax, eax
    jns 5f
    neg eax
5:
    dec ecx
    jnz 1b
    add eax, esi
    add eax, edi
    pop edi
    pop esi
    pop ebx
    ret

# ── memory: dword copy loop + running sum, guest memory both ways ──
    .globl w_mem
w_mem:
    push ebx
    push esi
    push edi
    mov ebx, [esp+16]
    xor eax, eax
1:
    mov esi, offset src_buf
    mov edi, offset dst_buf
    mov ecx, 1024
2:
    mov edx, [esi]
    add edx, ebx
    mov [edi], edx
    add eax, edx
    add esi, 4
    add edi, 4
    dec ecx
    jnz 2b
    mov edx, [dst_buf+64]
    mov [src_buf+128], edx
    dec ebx
    jnz 1b
    pop edi
    pop esi
    pop ebx
    ret

# ── string ops: rep movsd / rep stosd / rep movsb (DF clear) ──
    .globl w_rep
w_rep:
    push ebx
    push esi
    push edi
    mov ebx, [esp+16]
    xor edx, edx
    cld
1:
    mov esi, offset src_buf
    mov edi, offset dst_buf
    mov ecx, 1024
    rep movsd
    mov edi, offset out_buf
    mov eax, ebx
    mov ecx, 1024
    rep stosd
    mov esi, offset src_buf+3
    mov edi, offset out_buf+1
    mov ecx, 777
    rep movsb
    add edx, [dst_buf+400]
    add edx, [out_buf+800]
    add edx, [out_buf+4]
    dec ebx
    jnz 1b
    mov eax, edx
    pop edi
    pop esi
    pop ebx
    ret

# ── bytes: a hash over a byte buffer, al/ah/ax partial registers ──
    .globl w_bytes
w_bytes:
    push ebx
    push esi
    push edi
    mov edi, [esp+16]
    mov edx, 0x811C9DC5
1:
    mov esi, offset byte_buf
    mov ecx, 4096
2:
    xor eax, eax
    mov al, [esi]
    mov ah, al
    xor ah, dl
    add al, ah
    xor dl, al
    movzx ebx, ax
    imul edx, edx, 16777619
    xor edx, ebx
    rol edx, 5
    inc esi
    dec ecx
    jnz 2b
    dec edi
    jnz 1b
    mov eax, edx
    pop edi
    pop esi
    pop ebx
    ret

# ── x87: 4x4 matrix times 256 vec4, fld/fmul/faddp/fstp, sum of sqrt|v| (fabs/fsqrt), fistp ──
    .globl w_x87
w_x87:
    push ebx
    push esi
    push edi
    push ebp
    sub esp, 8
    mov ebp, [esp+28]
1:
    mov esi, offset vec_in
    mov edi, offset vec_out
    mov ecx, 256
2:
    xor ebx, ebx
3:
    fld dword ptr [esi]
    fmul dword ptr [matrix+ebx*4]
    fld dword ptr [esi+4]
    fmul dword ptr [matrix+ebx*4+16]
    faddp st(1), st
    fld dword ptr [esi+8]
    fmul dword ptr [matrix+ebx*4+32]
    faddp st(1), st
    fld dword ptr [esi+12]
    fmul dword ptr [matrix+ebx*4+48]
    faddp st(1), st
    fstp dword ptr [edi+ebx*4]
    inc ebx
    cmp ebx, 4
    jl 3b
    add esi, 16
    add edi, 16
    dec ecx
    jnz 2b
    dec ebp
    jnz 1b
    fldz
    mov esi, offset vec_out
    mov ecx, 1024
4:
    fld dword ptr [esi]
    fabs
    fsqrt
    faddp st(1), st
    add esi, 4
    dec ecx
    jnz 4b
    fistp dword ptr [esp]
    mov eax, [esp]
    add esp, 8
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

# ── SSE: the same transform with movss/mulss/addss, then mulps/addps ──
    .globl w_sse
w_sse:
    push esi
    push edi
    push ebx
    mov ebx, [esp+16]
1:
    mov esi, offset vec_in
    mov edi, offset vec_out
    mov ecx, 256
    movups xmm4, [matrix]
    movups xmm5, [matrix+16]
    movups xmm6, [matrix+32]
    movups xmm7, [matrix+48]
2:
    movss xmm0, [esi]
    shufps xmm0, xmm0, 0
    mulps xmm0, xmm4
    movss xmm1, [esi+4]
    shufps xmm1, xmm1, 0
    mulps xmm1, xmm5
    addps xmm0, xmm1
    movss xmm2, [esi+8]
    shufps xmm2, xmm2, 0
    mulps xmm2, xmm6
    addps xmm0, xmm2
    movss xmm3, [esi+12]
    shufps xmm3, xmm3, 0
    mulps xmm3, xmm7
    addps xmm0, xmm3
    movups [edi], xmm0
    add esi, 16
    add edi, 16
    dec ecx
    jnz 2b
    dec ebx
    jnz 1b
    xorps xmm0, xmm0
    mov esi, offset vec_out
    mov ecx, 1024
3:
    addss xmm0, [esi]
    add esi, 4
    dec ecx
    jnz 3b
    cvttss2si eax, xmm0
    pop ebx
    pop edi
    pop esi
    ret

# ── indirect calls through a function table (the dispatch) ──
    .globl w_icall
w_icall:
    push ebx
    push esi
    mov esi, [esp+12]
    xor ebx, ebx
    xor eax, eax
1:
    mov edx, ebx
    and edx, 3
    push eax
    call dword ptr [fn_table+edx*4]
    add esp, 4
    inc ebx
    cmp ebx, esi
    jne 1b
    pop esi
    pop ebx
    ret

    .globl w_leaf0
w_leaf0:
    mov eax, [esp+4]
    add eax, 3
    ret
    .globl w_leaf1
w_leaf1:
    mov eax, [esp+4]
    xor eax, 0x5A5A
    ret
    .globl w_leaf2
w_leaf2:
    mov eax, [esp+4]
    lea eax, [eax+eax*2+1]
    ret
    .globl w_leaf3
w_leaf3:
    mov eax, [esp+4]
    ror eax, 3
    ret

# ── call-heavy recursion: fib(n), direct calls and returns ──
    .globl w_fib
w_fib:
    push ebx
    mov ebx, [esp+8]
    cmp ebx, 2
    jae 1f
    mov eax, ebx
    pop ebx
    ret
1:
    lea eax, [ebx-1]
    push eax
    call w_fib
    add esp, 4
    push eax
    lea eax, [ebx-2]
    push eax
    call w_fib
    add esp, 4
    pop edx
    add eax, edx
    pop ebx
    ret

# ── direct calls into a function that calls through the table (XBA-247:
#    every call of w_vthunk records a frame, it may park) ──
    .globl w_vcall
w_vcall:
    push ebx
    push esi
    mov esi, [esp+12]
    xor ebx, ebx
    xor eax, eax
1:
    push eax
    push ebx
    call w_vthunk
    add esp, 8
    inc ebx
    cmp ebx, esi
    jne 1b
    pop esi
    pop ebx
    ret

    .globl w_vthunk
w_vthunk:
    mov edx, [esp+4]
    and edx, 3
    push dword ptr [esp+8]
    call dword ptr [fn_table+edx*4]
    add esp, 4
    ret

# ── stack-heavy code (XBA-307): locals and saved registers in [esp+N], an
#    x87 temporary spilled to a stack slot and read back as two dwords,
#    pushed arguments, and a frame function (push ebp; mov ebp, esp) whose
#    locals live in [ebp-N] -- what MSVC /O2 code does between its loops ──
    .globl w_stack
w_stack:
    push ebx
    push esi
    push edi
    sub esp, 16                 # [esp] acc, [esp+4] i, [esp+8] double tmp
    mov ecx, [esp+32]
    mov dword ptr [esp], 0x9E3779B9
    mov [esp+4], ecx
1:
    mov eax, [esp+4]
    push eax
    push dword ptr [esp+4]      # acc (esp moved by the push above)
    call stk_mix
    add esp, 8
    add [esp], eax
    fild dword ptr [esp+4]
    fadd st(0), st(0)
    fstp qword ptr [esp+8]
    mov edx, [esp+12]           # high dword of the double
    xor [esp], edx
    mov edx, [esp+8]
    add [esp], edx
    push dword ptr [esp+12]     # the double as two dword pushes: high...
    push dword ptr [esp+12]     # ...then low (esp moved by 4)
    call stk_dbl                # reads them as one fld qword
    add esp, 8
    xor [esp], eax
    dec dword ptr [esp+4]
    jnz 1b
    mov eax, [esp]
    add esp, 16
    pop edi
    pop esi
    pop ebx
    ret

    .globl stk_mix
stk_mix:
    push ebp
    mov ebp, esp
    sub esp, 12
    push esi
    mov eax, [ebp+8]
    mov ecx, [ebp+12]
    mov [ebp-4], eax
    mov [ebp-8], ecx
    imul eax, eax, 3
    add eax, [ebp-8]
    mov [ebp-12], eax
    mov esi, [ebp-4]
    shr esi, 2
    mov eax, [ebp-12]
    xor eax, esi
    rol eax, 5
    add eax, [ebp-8]
    pop esi
    mov esp, ebp
    pop ebp
    ret

# ── fast and kept functions calling each other (XBA-307): a caller that
#    keeps the MEM forms (a lea of one of its slots) writes a double argument
#    with fstp qword for a fast callee that reads it as two dwords, and a
#    fast caller pushes two dwords for a kept callee that reads them with
#    fld qword. Inlined, only the fences keep their accesses apart ──
    .globl w_stkmix
w_stkmix:
    push ebx
    push esi
    sub esp, 16
    mov esi, [esp+28]
    xor ebx, ebx
1:
    lea eax, [esp+8]            # kept: a slot's address in a register
    mov [eax], esi
    fild dword ptr [eax]
    sub esp, 8
    fstp qword ptr [esp]
    call mix_lo_hi
    add esp, 8
    add ebx, eax
    push esi
    call stk_push2
    add esp, 4
    xor ebx, eax
    rol ebx, 1
    dec esi
    jnz 1b
    mov eax, ebx
    add esp, 16
    pop esi
    pop ebx
    ret

    .globl mix_lo_hi
mix_lo_hi:
    mov eax, [esp+4]
    xor eax, [esp+8]
    ret

    .globl stk_push2
stk_push2:
    mov eax, [esp+4]
    and eax, 0xFFFFF
    or eax, 0x41200000          # high dword: 2^19..2^20
    push eax
    push dword ptr [esp+8]      # low dword: the argument
    call fld_esc
    add esp, 8
    ret

    .globl fld_esc
fld_esc:
    lea ecx, [esp+4]            # kept
    fld qword ptr [esp+4]
    fistp dword ptr [ecx]
    mov eax, [ecx]
    ret

# A slot written with one width and read with another: the stack accesses of
# one function must stay ordered whatever their C types (XBA-307).
    .globl stk_dbl
stk_dbl:
    fld qword ptr [esp+4]
    fadd st(0), st(0)
    sub esp, 8
    fistp dword ptr [esp]
    mov ecx, [esp]
    mov word ptr [esp+6], cx    # halfword store, dword read below
    mov byte ptr [esp+5], 0x5A  # byte store, dword read below
    mov byte ptr [esp+4], cl
    mov eax, [esp+4]
    add eax, ecx
    add esp, 8
    ret

# ── lock-prefixed read-modify-writes on shared words (XBA-277): what
#    InterlockedIncrement / Decrement / ExchangeAdd / CompareExchange and a
#    spin lock's xchg compile to: reference counts and lock-free lists ──
    .globl w_lock
w_lock:
    push ebx
    push esi
    mov ecx, [esp+12]
    xor esi, esi
1:
    mov edx, ecx
    and edx, 63
    lea edx, [src_buf+edx*4]
    lock inc dword ptr [edx]
    mov eax, ecx
    lock xadd [edx+256], eax
    add esi, eax
    lock dec dword ptr [edx+512]
    adc esi, 0
    mov eax, [edx+768]
    lea ebx, [eax+ecx]
    lock cmpxchg [edx+768], ebx
    add esi, eax
    mov eax, ecx
    xchg [edx+1024], eax
    xor esi, eax
    lock or dword ptr [edx+1280], ecx
    lock add dword ptr [edx+1536], 3
    dec ecx
    jnz 1b
    mov eax, esi
    add eax, [src_buf]
    add eax, [src_buf+1280]
    add eax, [src_buf+1536]
    pop esi
    pop ebx
    ret

# @data
# ── data (zero in the image; the bench host fills src_buf, byte_buf,
#    matrix and vec_in before it runs anything) ──
    .p2align 4
    .globl fn_table
fn_table:
    .long w_leaf0, w_leaf1, w_leaf2, w_leaf3
    .p2align 4
    .globl matrix
matrix:   .space 64
    .globl src_buf
src_buf:  .space 4096
    .globl dst_buf
dst_buf:  .space 4096
    .globl out_buf
out_buf:  .space 4096
    .globl byte_buf
byte_buf: .space 4096
    .globl vec_in
vec_in:   .space 4096
    .globl vec_out
vec_out:  .space 4096
