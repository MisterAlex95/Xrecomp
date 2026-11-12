# Synthetic SEH title for XBA-112: our own x86, shaped the way MSVC 7.x lays
# out exception frames, with our own minimal frame handlers (my_handler3 does
# what _except_handler3 does, my_cxx_handler what __CxxFrameHandler does for
# a catch-all). No CRT or XDK code.
#
# Built by tools/recomp/seh_fixture.py (as --32, ld -m elf_i386) into
# fixture.json; the runtime side (RAISE, UNWIND) is C in the test harness.
#
#   .text  0x00010000      code
#   .rdata 0x00020000      scope tables, FuncInfo
#   data   0x00030000      COUNTER, FILTER_RESULT, SEEN_CODE (absolute)
#   RAISE  0x0001F000      host: RtlRaiseException(record), stdcall
#   UNWIND 0x0001F010      host: RtlUnwind(frame, ip, record, value), stdcall

    .intel_syntax noprefix
    .code32

    .set COUNTER,       0x30000
    .set FILTER_RESULT, 0x30004
    .set SEEN_CODE,     0x30008
    .set RAISE,         0x1F000
    .set UNWIND,        0x1F010

    .text

# ---- raisers --------------------------------------------------------------

# g_raises: raise 0xE0000001 (continuable); return 0x333 if the raise returns.
    .globl g_raises
g_raises:
    push ebp
    mov ebp, esp
    sub esp, 0x50
    mov dword ptr [esp], 0xE0000001
    mov dword ptr [esp+4], 0
    mov dword ptr [esp+8], 0
    mov dword ptr [esp+0xC], 0
    mov dword ptr [esp+0x10], 0
    push esp
    call RAISE
    mov eax, 0x333
    mov esp, ebp
    pop ebp
    ret

# g_throw: raise 0xE06D7363 (a C++ throw, noncontinuable).
    .globl g_throw
g_throw:
    push ebp
    mov ebp, esp
    sub esp, 0x50
    mov dword ptr [esp], 0xE06D7363
    mov dword ptr [esp+4], 1
    mov dword ptr [esp+8], 0
    mov dword ptr [esp+0xC], 0
    mov dword ptr [esp+0x10], 0
    push esp
    call RAISE
    mov eax, 0x666
    mov esp, ebp
    pop ebp
    ret

# ---- EH3: __try/__except around a call to a __try/__finally ---------------

# f_outer: __try { f_inner(); eax = 0x111 } __except (filter) { eax = 0x222 }
    .globl f_outer
f_outer:
    push ebp
    mov ebp, esp
    push -1
    push offset scope_outer
    push offset my_handler3
    mov eax, dword ptr fs:0
    push eax
    mov dword ptr fs:0, esp
    sub esp, 8
    push ebx
    push esi
    push edi
    mov dword ptr [ebp-0x18], esp
    mov dword ptr [ebp-4], 0
    call f_inner
    mov eax, 0x111
    jmp fo_out
fo_except:
    mov esp, dword ptr [ebp-0x18]
    mov eax, 0x222
fo_out:
    mov dword ptr [ebp-4], -1
    mov ecx, dword ptr [ebp-0x10]
    mov dword ptr fs:0, ecx
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret
fo_filter:
    mov ecx, dword ptr [ebp-0x14]
    mov ecx, dword ptr [ecx]
    mov ecx, dword ptr [ecx]
    mov dword ptr ds:SEEN_CODE, ecx
    mov eax, dword ptr ds:FILTER_RESULT
    ret

# f_inner: __try { g_raises(); } __finally { COUNTER += 1 }
    .globl f_inner
f_inner:
    push ebp
    mov ebp, esp
    push -1
    push offset scope_inner
    push offset my_handler3
    mov eax, dword ptr fs:0
    push eax
    mov dword ptr fs:0, esp
    sub esp, 8
    push ebx
    push esi
    push edi
    mov dword ptr [ebp-0x18], esp
    mov dword ptr [ebp-4], 0
    call g_raises
    mov dword ptr [ebp-4], -1
    call fi_finally
    mov ecx, dword ptr [ebp-0x10]
    mov dword ptr fs:0, ecx
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret
fi_finally:
    add dword ptr ds:COUNTER, 1
    ret

# my_handler3: EXCEPTION_DISPOSITION (record, frame, context, dispatcher).
# The frame is an EH3 registration: [frame+8] scope table, [frame+0xC] try
# level, frame ebp = frame + 0x10, EXCEPTION_POINTERS* at [ebp-0x14].
    .globl my_handler3
my_handler3:
    push ebp
    mov ebp, esp
    sub esp, 8
    push ebx
    push esi
    push edi
    mov eax, dword ptr [ebp+8]
    test dword ptr [eax+4], 6
    jnz h_unwind
    mov dword ptr [ebp-8], eax
    mov ecx, dword ptr [ebp+0x10]
    mov dword ptr [ebp-4], ecx
    mov ebx, dword ptr [ebp+0xC]
    lea ecx, [ebp-8]
    mov dword ptr [ebx-4], ecx
    mov esi, dword ptr [ebx+0xC]
    mov edi, dword ptr [ebx+8]
h_loop:
    cmp esi, -1
    je h_search
    lea ecx, [esi+esi*2]
    mov eax, dword ptr [edi+ecx*4+4]
    test eax, eax
    jz h_next
    push ebp
    lea ebp, [ebx+0x10]
    call eax
    pop ebp
    cmp eax, 0
    jl h_continue
    je h_next
    push 0
    push 0
    push offset h_after_unwind
    push ebx
    call UNWIND
h_after_unwind:
    lea ecx, [esi+esi*2]
    mov eax, dword ptr [edi+ecx*4]
    mov dword ptr [ebx+0xC], eax
    mov eax, dword ptr [edi+ecx*4+8]
    lea ebp, [ebx+0x10]
    jmp eax
h_next:
    lea ecx, [esi+esi*2]
    mov esi, dword ptr [edi+ecx*4]
    jmp h_loop
h_search:
    mov eax, 1
    jmp h_ret
h_continue:
    xor eax, eax
    jmp h_ret
h_unwind:
    mov ebx, dword ptr [ebp+0xC]
    mov esi, dword ptr [ebx+0xC]
    mov edi, dword ptr [ebx+8]
hu_loop:
    cmp esi, -1
    je hu_done
    lea ecx, [esi+esi*2]
    mov eax, dword ptr [edi+ecx*4]
    mov dword ptr [ebx+0xC], eax
    mov edx, dword ptr [edi+ecx*4+4]
    test edx, edx
    jnz hu_skip
    mov edx, dword ptr [edi+ecx*4+8]
    push esi
    push ebp
    lea ebp, [ebx+0x10]
    call edx
    pop ebp
    pop esi
hu_skip:
    lea ecx, [esi+esi*2]
    mov esi, dword ptr [edi+ecx*4]
    jmp hu_loop
hu_done:
    mov eax, 1
h_ret:
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret

# ---- C++ EH: try { f_cxx_inner(); } catch (...) { } ----------------------

# f_cxx: returns 0x444 from the catch continuation.
    .globl f_cxx
f_cxx:
    push ebp
    mov ebp, esp
    push -1
    push offset ehstub_cxx
    mov eax, dword ptr fs:0
    push eax
    mov dword ptr fs:0, esp
    push ecx
    push ebx
    push esi
    push edi
    mov dword ptr [ebp-0x10], esp
    mov dword ptr [ebp-4], 0
    call f_cxx_inner
    mov eax, 0x555
cx_join:
    mov dword ptr [ebp-4], -1
    mov ecx, dword ptr [ebp-0xC]
    mov dword ptr fs:0, ecx
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret
cx_catch:
    mov eax, offset cx_cont
    ret
cx_cont:
    mov eax, 0x444
    jmp cx_join

# f_cxx_inner: an object with a destructor (state 0) around a throw.
    .globl f_cxx_inner
f_cxx_inner:
    push ebp
    mov ebp, esp
    push -1
    push offset ehstub_inner
    mov eax, dword ptr fs:0
    push eax
    mov dword ptr fs:0, esp
    push ecx
    mov dword ptr [ebp-0x10], esp
    mov dword ptr [ebp-4], 0
    call g_throw
    mov dword ptr [ebp-4], -1
    add dword ptr ds:COUNTER, 0x1000
    mov ecx, dword ptr [ebp-0xC]
    mov dword ptr fs:0, ecx
    mov esp, ebp
    pop ebp
    ret
inner_unwind:
    add dword ptr ds:COUNTER, 0x10
    ret

# __ehhandler stubs, as MSVC emits them.
    .globl ehstub_cxx
ehstub_cxx:
    mov eax, offset funcinfo_cxx
    jmp my_cxx_handler
    .globl ehstub_inner
ehstub_inner:
    mov eax, offset funcinfo_inner
    jmp my_cxx_handler

# my_cxx_handler: eax = FuncInfo. Registration at frame; frame ebp =
# frame + 0xC; state at [frame+8]; saved esp at [ebp-0x10]. Catches anything
# thrown in the first try block's state range.
    .globl my_cxx_handler
my_cxx_handler:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov edi, eax
    mov ebx, dword ptr [ebp+0xC]
    mov eax, dword ptr [ebp+8]
    test dword ptr [eax+4], 6
    jnz cx_unwind
    mov esi, dword ptr [ebx+8]
    cmp dword ptr [edi+12], 0
    je cx_search
    mov ecx, dword ptr [edi+16]
    cmp esi, dword ptr [ecx]
    jl cx_search
    cmp esi, dword ptr [ecx+4]
    jg cx_search
    push 0
    push 0
    push offset cx_after
    push ebx
    call UNWIND
cx_after:
    mov ecx, dword ptr [edi+16]
    mov eax, dword ptr [ecx+8]
    mov dword ptr [ebx+8], eax
    mov edx, dword ptr [ecx+16]
    mov edx, dword ptr [edx+12]
    push ebx
    push ebp
    lea ebp, [ebx+0xC]
    call edx
    pop ebp
    pop ebx
    lea ebp, [ebx+0xC]
    mov esp, dword ptr [ebp-0x10]
    jmp eax
cx_search:
    mov eax, 1
    jmp cx_ret
cx_unwind:
    mov esi, dword ptr [ebx+8]
cxu_loop:
    cmp esi, -1
    je cxu_done
    mov ecx, dword ptr [edi+8]
    mov edx, dword ptr [ecx+esi*8+4]
    mov esi, dword ptr [ecx+esi*8]
    mov dword ptr [ebx+8], esi
    test edx, edx
    jz cxu_loop
    push esi
    push ebp
    lea ebp, [ebx+0xC]
    call edx
    pop ebp
    pop esi
    jmp cxu_loop
cxu_done:
    mov eax, 1
cx_ret:
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
    .globl text_end
text_end:

# ---- tables ---------------------------------------------------------------

    .section .rdata, "a"
    .p2align 2
scope_outer:                      # {enclosing, filter, handler}
    .long -1, fo_filter, fo_except
scope_inner:
    .long -1, 0, fi_finally
funcinfo_cxx:
    .long 0x19930520, 1, unwind_cxx, 1, tries_cxx, 0, 0
unwind_cxx:
    .long -1, 0
tries_cxx:                        # tryLow, tryHigh, catchHigh, nCatches, handlers
    .long 0, 0, 1, 1, handlers_cxx
handlers_cxx:                     # adjectives, pType, dispCatchObj, handler
    .long 0, 0, 0, cx_catch
funcinfo_inner:
    .long 0x19930520, 1, unwind_inner, 0, 0, 0, 0
unwind_inner:
    .long -1, inner_unwind
