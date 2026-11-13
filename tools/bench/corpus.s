# Benchmark corpus for the lifted code (XBA-37, XBA-38): synthetic x86 of our
# own, shaped like the hot code of a title -- flag-heavy integer loops, memory
# copies through guest memory, x87 and SSE float loops, indirect calls through
# the dispatch, a call-heavy recursion, byte code with partial registers.
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

# ── x87: 4x4 matrix times 256 vec4, fld/fmul/faddp/fstp, fistp result ──
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
    fadd dword ptr [esi]
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
