/* x360_seh.c -- structured exceptions in lifted code (XBA-282): RtlRaiseException's dispatch, RtlUnwind and the
 * kernel's __C_specific_handler, done on the guest's own state.
 *
 * Lifted code keeps no host stack (a call, a return or a jump sets PC and hands control back to the dispatcher,
 * xenon_rt.h), so an exception is unwound entirely in guest state. Frames are walked as RtlVirtualUnwind does on
 * PowerPC: the function of a PC from .pdata (IMAGE_CE_RUNTIME_FUNCTION_ENTRY: start, prolog length, function length,
 * the exception flag), its prolog read back (mflr r12, std / stfd / stw to r1, the __savegprlr / __savefpr helpers it
 * calls, stwu / stwux r1), and the caller's state rebuilt from the slots it saved and the back chain. A function with
 * the exception flag has its handler and handler data in the two words before its start (as on Windows CE / NT
 * PowerPC); both are checked (the handler a lifted function or an import thunk, the data inside the image) before a
 * handler is called, so a frame whose flag means something else is only walked through.
 *
 * Handlers are guest code (or the kernel's __C_specific_handler, done here): they are called with their LR set to
 * X360_SEH_RETURN, and the dispatcher picks up where it was when one returns there (x360_seh_return). The search
 * phase calls each frame's handler until one takes the exception; RtlUnwind (from a handler, or the scope table's
 * __except through __C_specific_handler) then runs the frames' handlers again with EXCEPTION_UNWINDING up to the
 * target frame (termination handlers: __finally) and resumes there, r1 = the target frame, r3 = the return value,
 * the non-volatile registers as the frames between saved them. A raise inside a handler nests one level more.
 *
 * Layouts that follow Windows CE / NT PowerPC and Xenia's notes, not checked on a retail title yet: the exception
 * flag (bit 31 of the .pdata info word), DISPATCHER_CONTEXT {ControlPc, FunctionEntry, EstablisherFrame,
 * ContextRecord}, the expanded RUNTIME_FUNCTION {Begin, End, Handler, HandlerData, PrologEnd}, the scope table
 * {Count, {Begin, End, Handler (filter, 1, or the __finally), JumpTarget (0: a __finally)}}, and the CONTEXT record
 * (x360_seh_write_context). VMX registers are not unwound (no __savevmx in a prolog is read). */
#include <string.h>

#include "x360_internal.h"

enum { PH_SEARCH = 1, PH_UNWIND = 2 };
enum { W_HANDLER = 1, W_FILTER = 2, W_FINALLY = 3 };
enum { EXC_NONCONTINUABLE = 1, EXC_UNWINDING = 2, EXC_EXIT_UNWIND = 4 };

#define AREA_CONTEXT 0x000
#define AREA_DISPATCH 0x400
#define AREA_FUNCTION 0x410
#define AREA_POINTERS 0x430
#define AREA_RECORD 0x440   /* RtlUnwind without a record: one made here */
#define AREA_SIZE 0x1000    /* below the raising frame's r1 */
#define HANDLER_STACK 0x200 /* the handlers' frames start this far below the area */

typedef x360_seh_fn pfn;

/* ---- .pdata ---- */

static void find_pdata(x360_machine *x)
{
    x->pdata_scanned = 1;
    x360_seh_bind(x);
    uint32_t b = x->image_base;
    if (!b || x360_r16(x, b) != 0x4D5A) /* "MZ" read big-endian */
        return;
    uint8_t *p = x360_p(x, b);
    uint32_t pe = (uint32_t)p[0x3C] | (uint32_t)p[0x3D] << 8 | (uint32_t)p[0x3E] << 16 | (uint32_t)p[0x3F] << 24;
    if (pe > 0x1000 || memcmp(p + pe, "PE\0\0", 4))
        return;
    unsigned nsec = p[pe + 6] | p[pe + 7] << 8, opt = p[pe + 20] | p[pe + 21] << 8;
    const uint8_t *s = p + pe + 24 + opt;
    for (unsigned k = 0; k < nsec && k < 96; k++, s += 40) {
        if (memcmp(s, ".pdata", 6))
            continue;
        uint32_t vsize = s[8] | s[9] << 8 | s[10] << 16 | (uint32_t)s[11] << 24;
        uint32_t va = s[12] | s[13] << 8 | s[14] << 16 | (uint32_t)s[15] << 24;
        if (va >= x->image_size || vsize > x->image_size - va)
            return;
        x->pdata_va = b + va;
        uint32_t n = vsize / 8;
        while (n && !x360_r32(x, x->pdata_va + 8 * (n - 1)))
            n--; /* trailing zero entries */
        x->pdata_n = n;
        return;
    }
}

int x360_seh_lookup(x360_machine *x, uint32_t pc, x360_seh_fn *f)
{
    if (!x->pdata_scanned)
        find_pdata(x);
    unsigned lo = 0, hi = x->pdata_n;
    while (lo < hi) { /* the last entry starting at or before pc */
        unsigned mid = (lo + hi) / 2;
        if (x360_r32(x, x->pdata_va + 8 * mid) <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return 0;
    uint32_t a = x->pdata_va + 8 * (lo - 1);
    f->entry = a;
    f->start = x360_r32(x, a);
    f->info = x360_r32(x, a + 4);
    f->prolog = f->info & 0xFF;
    f->len = ((f->info >> 8) & 0x3FFFFF) * 4;
    return pc >= f->start && pc - f->start < f->len;
}

int x360_seh_code_pc(x360_machine *x, uint32_t pc)
{
    return !(pc & 3) && ((pc >= x->code_lo && pc < x->code_hi && x->fn_tab[(pc - x->code_lo) >> 2]) ||
                         x360_thunk_at(x, pc));
}

/* The frame's handler and handler data (the two words before its start), when it has them and they look right. */
static int frame_handler(x360_machine *x, const pfn *f, uint32_t *handler, uint32_t *data)
{
    if (!(f->info & 0x80000000u) || f->start < x->image_base + 8)
        return 0;
    uint32_t h = x360_r32(x, f->start - 8), d = x360_r32(x, f->start - 4);
    if (!x360_seh_code_pc(x, h) || d < x->image_base || d - x->image_base >= x->image_size)
        return 0;
    *handler = h;
    *data = d;
    return 1;
}

/* ---- contexts ---- */

static void ctx_from_cpu(x360_seh_ctx *k, const xe_cpu *c)
{
    memcpy(k->gpr, c->gpr, sizeof k->gpr);
    memcpy(k->fpr, c->fpr, sizeof k->fpr);
    k->lr = c->lr, k->ctr = c->ctr, k->pc = c->pc, k->cr = c->cr;
    k->xer = c->xer_so << 31 | c->xer_ov << 30 | c->xer_ca << 29;
}

static void cpu_from_ctx(xe_cpu *c, const x360_seh_ctx *k)
{
    memcpy(c->gpr, k->gpr, sizeof k->gpr);
    memcpy(c->fpr, k->fpr, sizeof k->fpr);
    c->lr = k->lr, c->ctr = k->ctr, c->pc = k->pc, c->cr = k->cr;
    c->xer_so = k->xer >> 31 & 1, c->xer_ov = k->xer >> 30 & 1, c->xer_ca = k->xer >> 29 & 1;
}

/* The CONTEXT record a handler gets (Xenia's X_CONTEXT as described: flags, msr, iar, lr at 0x0-0xC, ctr at 0x10,
 * gpr[32] at 0x18, cr / xer at 0x118, fpr[32] at 0x120, fpscr at 0x220). */
void x360_seh_write_context(x360_machine *x, uint32_t a, const x360_seh_ctx *k)
{
    x360_w32(x, a, 0x0000000Fu); /* control, integer, floating point, (no) vector */
    x360_w32(x, a + 0x4, 0x9030);
    x360_w32(x, a + 0x8, k->pc);
    x360_w32(x, a + 0xC, (uint32_t)k->lr);
    x360_w64(x, a + 0x10, k->ctr);
    for (int r = 0; r < 32; r++)
        x360_w64(x, a + 0x18 + 8 * (uint32_t)r, k->gpr[r]);
    x360_w32(x, a + 0x118, k->cr);
    x360_w32(x, a + 0x11C, k->xer);
    for (int r = 0; r < 32; r++) {
        uint64_t v;
        memcpy(&v, &k->fpr[r], 8);
        x360_w64(x, a + 0x120 + 8 * (uint32_t)r, v);
    }
    x360_w64(x, a + 0x220, 0);
}

void x360_seh_read_context(x360_machine *x, uint32_t a, x360_seh_ctx *k)
{
    k->pc = x360_r32(x, a + 0x8);
    k->lr = x360_r32(x, a + 0xC);
    k->ctr = x360_r64(x, a + 0x10);
    for (int r = 0; r < 32; r++)
        k->gpr[r] = x360_r64(x, a + 0x18 + 8 * (uint32_t)r);
    k->cr = x360_r32(x, a + 0x118);
    k->xer = x360_r32(x, a + 0x11C);
    for (int r = 0; r < 32; r++) {
        uint64_t v = x360_r64(x, a + 0x120 + 8 * (uint32_t)r);
        memcpy(&k->fpr[r], &v, 8);
    }
}

/* ---- virtual unwind ---- */

typedef struct saves {
    int lr_in_r12;          /* mflr r12 ran */
    int lr_saved;           /* stw r12, d(r1) ran: LR at entry sp + lr_off */
    int32_t lr_off;
    int stwu;               /* the frame is allocated (stwu / stwux r1) */
    int32_t gpr_off[32], fpr_off[32]; /* entry-sp-relative slots (INT32_MIN: not saved) */
    int64_t r12_base;       /* addi r12, r1, imm: r12 = entry sp + imm (the FPR helpers' base), or -1 */
} saves;

#define NOT_SAVED INT32_MIN

/* One prolog store (or a helper's): rA 1 = r1 (the entry sp until stwu), 12 = r12 (r12_base). */
static void note_store(saves *s, uint32_t w)
{
    uint32_t op = w >> 26, rs = (w >> 21) & 31, ra = (w >> 16) & 31;
    int32_t d = (int16_t)(w & 0xFFFF);
    int64_t base;
    if (ra == 1 && !s->stwu)
        base = 0;
    else if (ra == 12 && s->r12_base != -1)
        base = s->r12_base;
    else
        return; /* after the stwu (a frame-relative save): not followed */
    if (op == 62 && (w & 3) == 0) /* std */
        s->gpr_off[rs] = (int32_t)(base + (d & ~3));
    else if (op == 54) /* stfd */
        s->fpr_off[rs] = (int32_t)(base + d);
    else if (op == 36 && rs == 12 && s->lr_in_r12) { /* stw r12 (= LR) */
        s->lr_saved = 1;
        s->lr_off = (int32_t)(base + d);
    } else if (op == 36 && rs >= 13) /* stw of a non-volatile (32-bit saves) */
        s->gpr_off[rs] = (int32_t)(base + d);
}

/* A save / restore helper called from the prolog (__savegprlr_N, __savefpr_N): its stores, up to its blr. */
static void note_helper(x360_machine *x, saves *s, uint32_t at)
{
    for (int k = 0; k < 64; k++, at += 4) {
        uint32_t w = x360_r32(x, at);
        if (w == 0x4E800020u) /* blr */
            return;
        note_store(s, w);
    }
}

static void read_prolog(x360_machine *x, const pfn *f, uint32_t until, saves *s)
{
    memset(s, 0, sizeof *s);
    s->r12_base = -1;
    for (int r = 0; r < 32; r++)
        s->gpr_off[r] = s->fpr_off[r] = NOT_SAVED;
    uint32_t end = f->start + 4 * f->prolog;
    if (until < end)
        end = until;
    for (uint32_t a = f->start; a < end; a += 4) {
        uint32_t w = x360_r32(x, a);
        uint32_t op = w >> 26;
        if (w == 0x7D8802A6u) { /* mflr r12 */
            s->lr_in_r12 = 1;
        } else if (op == 37 && ((w >> 21) & 31) == 1 && ((w >> 16) & 31) == 1) { /* stwu r1, d(r1) */
            s->stwu = 1;
        } else if (op == 31 && ((w >> 1) & 0x3FF) == 183 && ((w >> 21) & 31) == 1 && ((w >> 16) & 31) == 1) {
            s->stwu = 1; /* stwux r1, r1, rB */
        } else if (op == 62 && (w & 3) == 1 && ((w >> 21) & 31) == 1 && ((w >> 16) & 31) == 1) {
            s->stwu = 1; /* stdu r1, d(r1) */
        } else if (op == 14 && ((w >> 21) & 31) == 12 && ((w >> 16) & 31) == 1 && !s->stwu) { /* addi r12, r1, d */
            s->r12_base = (int16_t)(w & 0xFFFF);
            s->lr_in_r12 = 0;
        } else if (op == 18 && (w & 3) == 1) { /* bl helper */
            int32_t li = (int32_t)(w & 0x03FFFFFCu);
            if (li & 0x02000000)
                li -= 0x04000000;
            note_helper(x, s, a + (uint32_t)li);
        } else {
            note_store(s, w);
        }
    }
}

/* next = the caller's state of the frame cur is in (f: its function, or NULL for a leaf without .pdata). */
void x360_seh_virtual_unwind(x360_machine *x, const x360_seh_ctx *cur, const x360_seh_fn *f, x360_seh_ctx *next)
{
    *next = *cur;
    if (!f) {
        next->pc = (uint32_t)cur->lr & ~3u;
        return;
    }
    saves s;
    read_prolog(x, f, cur->pc - 4 < f->start ? f->start : cur->pc, &s);
    uint32_t sp = (uint32_t)cur->gpr[1];
    uint32_t entry = s.stwu ? x360_r32(x, sp) : sp; /* the back chain */
    for (int r = 0; r < 32; r++) {
        if (s.gpr_off[r] != NOT_SAVED)
            next->gpr[r] = x360_r64(x, entry + (uint32_t)s.gpr_off[r]);
        if (s.fpr_off[r] != NOT_SAVED) {
            uint64_t v = x360_r64(x, entry + (uint32_t)s.fpr_off[r]);
            memcpy(&next->fpr[r], &v, 8);
        }
    }
    uint32_t ra = s.lr_saved ? x360_r32(x, entry + (uint32_t)s.lr_off)
                  : s.lr_in_r12 ? (uint32_t)cur->gpr[12]
                                : (uint32_t)cur->lr;
    next->gpr[1] = entry;
    next->pc = ra & ~3u;
    next->lr = next->pc;
}

/* ---- the dispatcher ---- */

static x360_seh_level *top(x360_thread *t)
{
    return t->seh.depth > 0 ? &t->seh.lv[t->seh.depth - 1] : NULL;
}

static void pop(x360_thread *t)
{
    if (t->seh.depth > 0)
        t->seh.depth--;
}

static void call_guest(x360_machine *x, x360_thread *t, x360_seh_level *lv, int wait, uint32_t fn, uint32_t a0,
                       uint32_t a1, uint32_t a2, uint32_t a3)
{
    xe_cpu *c = &t->cpu;
    lv->wait = wait;
    c->gpr[3] = a0, c->gpr[4] = a1, c->gpr[5] = a2, c->gpr[6] = a3;
    c->gpr[1] = lv->area - HANDLER_STACK;
    x360_w32(x, (uint32_t)c->gpr[1], 0); /* the end of the back chain */
    c->lr = X360_SEH_RETURN;
    c->pc = fn;
    x->st.seh_handlers++;
}

/* The frame's dispatch records in the area: the exception context, RUNTIME_FUNCTION, DISPATCHER_CONTEXT. */
static void frame_records(x360_machine *x, x360_seh_level *lv)
{
    uint32_t a = lv->area;
    x360_seh_write_context(x, a + AREA_CONTEXT, &lv->exc); /* the exception context, as Windows hands it */
    x360_w32(x, a + AREA_FUNCTION, lv->fstart);
    x360_w32(x, a + AREA_FUNCTION + 4, lv->fstart + lv->flen);
    x360_w32(x, a + AREA_FUNCTION + 8, lv->handler);
    x360_w32(x, a + AREA_FUNCTION + 12, lv->hdata);
    x360_w32(x, a + AREA_FUNCTION + 16, lv->fstart + 4 * lv->fprolog);
    x360_w32(x, a + AREA_DISPATCH, lv->cur.pc);
    x360_w32(x, a + AREA_DISPATCH + 4, a + AREA_FUNCTION);
    x360_w32(x, a + AREA_DISPATCH + 8, lv->establisher);
    x360_w32(x, a + AREA_DISPATCH + 12, a + AREA_CONTEXT);
}

static void unhandled(x360_machine *x, x360_thread *t, x360_seh_level *lv, const char *why)
{
    uint32_t code = x360_r32(x, lv->rec), at = x360_r32(x, lv->rec + 12);
    x360_fail(x, "unhandled exception %08X at %08X (%s)", code, at, why);
    pop(t);
}

static void search(x360_machine *x, x360_thread *t, x360_seh_level *lv);
static void unwind_step(x360_machine *x, x360_thread *t, x360_seh_level *lv);

static void finish(x360_machine *x, x360_thread *t, x360_seh_level *lv)
{
    xe_cpu *c = &t->cpu;
    cpu_from_ctx(c, &lv->cur);
    c->pc = lv->target_ip;
    c->gpr[1] = lv->target_frame;
    c->gpr[3] = lv->retval;
    c->lr = lv->target_ip;
    x->st.seh_unwinds++;
    if (x->verbose)
        x360_logf(x, "x360: [t%u] unwound to %08X (frame %08X)\n", t->id, lv->target_ip, lv->target_frame);
    pop(t);
}

static void begin_unwind(x360_machine *x, x360_thread *t, x360_seh_level *lv, uint32_t frame, uint32_t ip,
                         uint32_t retval)
{
    lv->phase = PH_UNWIND;
    lv->target_frame = frame, lv->target_ip = ip, lv->retval = retval;
    x360_w32(x, lv->rec + 4, x360_r32(x, lv->rec + 4) | (frame ? EXC_UNWINDING : EXC_UNWINDING | EXC_EXIT_UNWIND));
    lv->cur = lv->exc;
    unwind_step(x, t, lv);
}

static void continue_execution(x360_machine *x, x360_thread *t, x360_seh_level *lv)
{
    if (x360_r32(x, lv->rec + 4) & EXC_NONCONTINUABLE) {
        unhandled(x, t, lv, "a handler continued a noncontinuable exception");
        return;
    }
    x360_seh_ctx k = lv->exc;
    x360_seh_read_context(x, lv->area + AREA_CONTEXT, &k); /* what the filter / handler left in the record */
    cpu_from_ctx(&t->cpu, &k);
    pop(t);
}

/* __C_specific_handler, search phase: the frame's scopes holding ControlPc with an __except. 1 when a guest call
 * or an unwind was started (or execution continues), 0 when no scope takes the exception (the next frame). */
static int cspec_search(x360_machine *x, x360_thread *t, x360_seh_level *lv)
{
    uint32_t n = x360_r32(x, lv->hdata), pc = lv->cur.pc - 4; /* the call (cur.pc is where it returns) */
    for (; lv->scope < n && lv->scope < 256; lv->scope++) {
        uint32_t e = lv->hdata + 4 + 16 * lv->scope;
        uint32_t b = x360_r32(x, e), en = x360_r32(x, e + 4), h = x360_r32(x, e + 8), j = x360_r32(x, e + 12);
        if (pc < b || pc >= en || !j)
            continue;
        if (h == 1) { /* EXCEPTION_EXECUTE_HANDLER as the filter */
            begin_unwind(x, t, lv, lv->establisher, j, x360_r32(x, lv->rec));
            return 1;
        }
        uint32_t ptrs = lv->area + AREA_POINTERS;
        frame_records(x, lv);
        x360_w32(x, ptrs, lv->rec);
        x360_w32(x, ptrs + 4, lv->area + AREA_CONTEXT);
        call_guest(x, t, lv, W_FILTER, h, ptrs, lv->establisher, 0, 0);
        return 1;
    }
    return 0;
}

/* __C_specific_handler, unwind phase: the __finally of each scope holding ControlPc (abnormal termination). */
static int cspec_unwind(x360_machine *x, x360_thread *t, x360_seh_level *lv)
{
    uint32_t n = x360_r32(x, lv->hdata), pc = lv->cur.pc - 4; /* the call (cur.pc is where it returns) */
    while (lv->scope < n && lv->scope < 256) {
        uint32_t e = lv->hdata + 4 + 16 * lv->scope++;
        uint32_t b = x360_r32(x, e), en = x360_r32(x, e + 4), h = x360_r32(x, e + 8), j = x360_r32(x, e + 12);
        if (pc < b || pc >= en || j || !x360_seh_code_pc(x, h))
            continue;
        call_guest(x, t, lv, W_FINALLY, h, 1, lv->establisher, 0, 0);
        return 1;
    }
    return 0;
}

/* The next frame with a handler, from lv->cur: 1 and lv's frame fields set, or 0 at the end of the chain. */
static int next_frame(x360_machine *x, x360_seh_level *lv, int *have_handler)
{
    *have_handler = 0;
    if (!x360_seh_code_pc(x, lv->cur.pc) || lv->cur.pc == X360_THREAD_EXIT)
        return 0;
    pfn f;
    int have = x360_seh_lookup(x, lv->cur.pc - 4, &f);
    x360_seh_virtual_unwind(x, &lv->cur, have ? &f : NULL, &lv->next);
    lv->establisher = (uint32_t)lv->cur.gpr[1];
    if (have && frame_handler(x, &f, &lv->handler, &lv->hdata)) {
        lv->fstart = f.start, lv->flen = f.len, lv->fprolog = f.prolog;
        *have_handler = 1;
    }
    if (x->verbose)
        x360_logf(x, "x360: seh frame pc %08X sp %08X (function %08X%s), caller pc %08X sp %08X\n", lv->cur.pc,
                  (uint32_t)lv->cur.gpr[1], have ? f.start : 0, *have_handler ? ", handler" : "", lv->next.pc,
                  (uint32_t)lv->next.gpr[1]);
    return 1;
}

static void search(x360_machine *x, x360_thread *t, x360_seh_level *lv)
{
    for (int guard = 0; guard < 4096; guard++) {
        int h;
        if (!next_frame(x, lv, &h)) {
            unhandled(x, t, lv, "no handler took it");
            return;
        }
        if (h) {
            if (lv->handler == x->cspec_thunk && x->cspec_thunk) {
                lv->scope = 0;
                if (cspec_search(x, t, lv))
                    return;
            } else {
                frame_records(x, lv);
                call_guest(x, t, lv, W_HANDLER, lv->handler, lv->rec, lv->establisher, lv->area + AREA_CONTEXT,
                           lv->area + AREA_DISPATCH);
                return;
            }
        }
        if (lv->next.gpr[1] <= lv->cur.gpr[1] && lv->next.pc == lv->cur.pc) {
            unhandled(x, t, lv, "the frame chain does not move");
            return;
        }
        lv->cur = lv->next;
    }
    unhandled(x, t, lv, "too many frames");
}

static void unwind_step(x360_machine *x, x360_thread *t, x360_seh_level *lv)
{
    for (int guard = 0; guard < 4096; guard++) {
        uint32_t sp = (uint32_t)lv->cur.gpr[1];
        if (lv->target_frame && sp == lv->target_frame) {
            finish(x, t, lv);
            return;
        }
        if (lv->target_frame && sp > lv->target_frame) {
            unhandled(x, t, lv, "the unwind passed its target frame");
            return;
        }
        int h;
        if (!next_frame(x, lv, &h)) {
            unhandled(x, t, lv, "the unwind did not find its target frame");
            return;
        }
        if (h) {
            if (lv->handler == x->cspec_thunk && x->cspec_thunk) {
                lv->scope = 0;
                if (cspec_unwind(x, t, lv))
                    return;
            } else {
                frame_records(x, lv);
                call_guest(x, t, lv, W_HANDLER, lv->handler, lv->rec, lv->establisher, lv->area + AREA_CONTEXT,
                           lv->area + AREA_DISPATCH);
                return;
            }
        }
        lv->cur = lv->next;
    }
    unhandled(x, t, lv, "too many frames");
}

static x360_seh_level *push(x360_machine *x, x360_thread *t, const xe_cpu *c, uint32_t rec)
{
    if (t->seh.depth >= X360_SEH_DEPTH) {
        x360_fail(x, "exception %08X raised %d levels deep in exception handlers", rec ? x360_r32(x, rec) : 0,
                  t->seh.depth);
        return NULL;
    }
    x360_seh_level *lv = &t->seh.lv[t->seh.depth++];
    memset(lv, 0, sizeof *lv);
    ctx_from_cpu(&lv->exc, c);
    lv->area = ((uint32_t)c->gpr[1] - AREA_SIZE) & ~15u;
    lv->rec = rec;
    return lv;
}

void x360_seh_raise(x360_machine *x, x360_thread *t, uint32_t rec)
{
    xe_cpu *c = &t->cpu; /* c->pc: the return address (the dispatcher set it before the HLE call) */
    if (!rec) {
        x360_fail(x, "RtlRaiseException without a record");
        return;
    }
    if (!x360_r32(x, rec + 12))
        x360_w32(x, rec + 12, c->pc); /* ExceptionAddress */
    x->st.seh_raised++;
    if (x->verbose)
        x360_logf(x, "x360: [t%u] exception %08X raised at %08X\n", t->id, x360_r32(x, rec), c->pc);
    x360_seh_level *lv = push(x, t, c, rec);
    if (!lv)
        return;
    lv->phase = PH_SEARCH;
    lv->cur = lv->exc;
    search(x, t, lv);
}

void x360_seh_unwind(x360_machine *x, x360_thread *t, uint32_t frame, uint32_t ip, uint32_t rec, uint32_t retval)
{
    x360_seh_level *lv = top(t);
    if (!lv || lv->phase != PH_SEARCH) { /* not from a handler: unwind from the caller of RtlUnwind */
        lv = push(x, t, &t->cpu, rec);
        if (!lv)
            return;
        if (!rec) {
            lv->rec = lv->area + AREA_RECORD;
            x360_w32(x, lv->rec, 0xC0000027u); /* STATUS_UNWIND */
            x360_w32(x, lv->rec + 4, 0);
            x360_w32(x, lv->rec + 8, 0);
            x360_w32(x, lv->rec + 12, t->cpu.pc);
            x360_w32(x, lv->rec + 16, 0);
        }
    } else if (rec) {
        lv->rec = rec;
    }
    begin_unwind(x, t, lv, frame, ip, retval);
}

void x360_seh_return(x360_machine *x, x360_thread *t)
{
    x360_seh_level *lv = top(t);
    if (!lv) {
        x360_fail(x, "a return to the exception dispatcher with no exception being dispatched");
        return;
    }
    int32_t r3 = (int32_t)(uint32_t)t->cpu.gpr[3];
    switch (lv->wait) {
    case W_FILTER:
        if (r3 > 0)
            begin_unwind(x, t, lv, lv->establisher, x360_r32(x, lv->hdata + 4 + 16 * lv->scope + 12),
                         x360_r32(x, lv->rec));
        else if (r3 < 0)
            continue_execution(x, t, lv);
        else {
            lv->scope++;
            if (!cspec_search(x, t, lv)) {
                lv->cur = lv->next;
                search(x, t, lv);
            }
        }
        return;
    case W_FINALLY:
        if (!cspec_unwind(x, t, lv)) {
            lv->cur = lv->next;
            unwind_step(x, t, lv);
        }
        return;
    default:
        if (lv->phase == PH_UNWIND) {
            lv->cur = lv->next;
            unwind_step(x, t, lv);
        } else if (r3 == 0) { /* ExceptionContinueExecution */
            continue_execution(x, t, lv);
        } else { /* ExceptionContinueSearch (1), a nested exception or a collided unwind (2, 3): on */
            lv->cur = lv->next;
            search(x, t, lv);
        }
        return;
    }
}

void x360_seh_bind(x360_machine *x)
{
    x->cspec_thunk = 0;
    for (unsigned k = 0; k < x->nthunks; k++)
        if (x->thunks[k].lib == X360_LIB_XBOXKRNL && x->thunks[k].ordinal == X360_xboxkrnl___C_specific_handler)
            x->cspec_thunk = x->thunks[k].addr;
}
