/*
 * xbox_resume.c - the resume engine (XBA-250): a thread re-entering its saved guest call chain. See
 * xbox_resume.h; the lifted side is RECOMP_FRAME_ENTRY_R (recomp_types.h) and the resume blocks
 * tools/recomp/translator.py emits (_resume_block).
 */
#include "xbox_resume.h"
#include "kernel_park.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RECOMP_TLS uint32_t t_target;            /* the indirect call's target the next label takes */
static RECOMP_TLS XboxResumePlan *t_kernel;     /* re-entering its kernel call: the dispatch completes it */
static volatile int32_t s_completed;

XboxResumePlan *xbox_ResumePlanAlloc(uint32_t n)
{
    size_t bytes = sizeof(XboxResumePlan) + (size_t)(n ? n - 1 : 0) * sizeof(XboxResumeFrame);
    XboxResumePlan *p = (XboxResumePlan *)calloc(1, bytes);
    if (p)
        p->n = n;
    return p;
}

static const char *site_problem(uint32_t flags)
{
    if (flags & XBOX_CALL_SITE_STACK_SWITCH) return "in a function that switches stacks";
    if (flags & XBOX_CALL_SITE_JMPBUF) return "a setjmp / longjmp call";
    if (flags & XBOX_CALL_SITE_MANUAL) return "a call into hand-written code";
    if (flags & XBOX_CALL_SITE_SEH_ANCHOR) return "in a function with an exception handler (__try)";
    if (flags & XBOX_CALL_SITE_NO_RESUME) return "a site the lifter gave no resume entry";
    return NULL;
}

XboxResumePlan *xbox_ResumePlanBuild(const XboxResumeChainFrame *fr, uint32_t n, uint32_t site_kind,
                                     uint32_t site_va, uint32_t ordinal, uint32_t kernel_target, int returned,
                                     uint32_t call_esp, const RecompCpuRegs *regs, XboxResumeLookup lookup,
                                     char *why, size_t why_n)
{
    XboxResumePlan *p = NULL;
    uint32_t i, func = 0, flags = 0, key;
    const char *bad;
    const char *what = site_kind == XBOX_PARK_SITE_KERNEL ? "kernel call" : "host wait";

    if (why && why_n)
        why[0] = 0;
#define REFUSE(...) do { if (why && why_n) snprintf(why, why_n, __VA_ARGS__); free(p); return NULL; } while (0)
    if (!n || !fr || !regs || !lookup)
        REFUSE("no lifted frame (its routine is not lifted code)");
    if (n > RECOMP_FRAME_SLOTS)
        REFUSE("deeper than the frame shadow stack");
    if (fr[0].f_ret != 0)
        REFUSE("its first frame returns to 0x%08X: not a routine the runtime started", fr[0].f_ret);
    /* The innermost site, in the innermost frame's function. */
    key = site_kind == XBOX_PARK_SITE_KERNEL ? site_va : (XBOX_RESUME_HOST_WAIT | site_va);
    if (!site_va || !lookup(key, &func, &flags))
        REFUSE("its %s at 0x%08X is at no lifted site", what, site_va);
    if ((bad = site_problem(flags)) != NULL)
        REFUSE("its %s at 0x%08X is %s", what, site_va, bad);
    if (site_kind == XBOX_PARK_SITE_KERNEL && !kernel_target)
        REFUSE("kernel call %u has no thunk in this run", ordinal);
    p = xbox_ResumePlanAlloc(n);
    if (!p)
        REFUSE("out of memory");
    p->entry_esp = fr[0].f_esp;
    p->entry_ebp = fr[0].f_ebp;
    p->site_kind = site_kind;
    p->ordinal = ordinal;
    p->returned = returned ? 1u : 0u;
    p->call_esp = call_esp;
    p->regs = *regs;
    /* Frame i is the function frame i+1's call site is in (the table's), the innermost one the function of the
     * park site; it re-enters at that call, holding the ebp it published for it. */
    for (i = n; i-- > 0;) {
        XboxResumeFrame *f = &p->f[i];
        if (i + 1 == n) {
            f->func = func;
            f->site = key;
            f->ebp = regs->r_ebp;
            f->target = site_kind == XBOX_PARK_SITE_KERNEL ? kernel_target : 0;
            continue;
        }
        if (fr[i + 1].f_esp == fr[i].f_esp)
            REFUSE("frame %u was entered by a tail jump from frame %u", i + 1, i);
        if (!lookup(fr[i + 1].f_ret, &f->func, &flags))
            REFUSE("frame %u returns to 0x%08X, which no call site names", i + 1, fr[i + 1].f_ret);
        if ((bad = site_problem(flags)) != NULL)
            REFUSE("frame %u was called from 0x%08X, %s", i + 1, fr[i + 1].f_ret, bad);
        f->site = fr[i + 1].f_ret;
        f->ebp = fr[i + 1].f_ebp;
        f->next_esp = fr[i + 1].f_esp;
        f->next_ebp = fr[i + 1].f_ebp;
        f->target = (flags & XBOX_CALL_SITE_INDIRECT) ? p->f[i + 1].func : 0;
    }
#undef REFUSE
    return p;
}

void xbox_ResumeBegin(XboxResumePlan *p)
{
    p->at = 0;
    p->done = 0;
    t_kernel = NULL;
    g_cpu.r_resume = p;
    /* Every entry compares the frame top with the limit; at the base (or with no array yet, NULL against NULL)
     * the compare fails and RECOMP_FRAME_ENTRY_R goes to the slow path, which records the frame and jumps to the
     * function's resume block. */
    g_cpu.r_flimit = g_cpu.r_fbase;
    if (g_cpu.r_fbase)
        g_cpu.r_ftop = g_cpu.r_fbase;
    g_esp = p->entry_esp;
    g_ebp = p->entry_ebp;
    g_seh_ebp = p->entry_ebp;
}

int xbox_ResumeActive(void)
{
    return g_cpu.r_resume != NULL;
}

/* The innermost frame was reached: resume mode off, the frame limit back. */
static void resume_end(XboxResumePlan *p)
{
    g_cpu.r_resume = NULL;
    g_cpu.r_flimit = g_cpu.r_fbase ? g_cpu.r_fbase + RECOMP_FRAME_SLOTS : NULL;
    p->done = 1;
#if defined(__GNUC__) || defined(__clang__)
    __atomic_add_fetch(&s_completed, 1, __ATOMIC_SEQ_CST);
#else
    s_completed++;
#endif
}

static void resume_fatal(const char *what, uint32_t func, uint32_t site)
{
    XboxResumePlan *p = (XboxResumePlan *)g_cpu.r_resume;
    fprintf(stderr, "  [STATE] resume: %s (function 0x%08X, site 0x%08X, frame %u of %u): the saved chain does not"
                    " match this build's code\n", what, func, site, p ? p->at : 0, p ? p->n : 0);
    fflush(stderr);
    abort();
}

uint32_t recomp_resume_site(uint32_t func, uint32_t *ebp)
{
    XboxResumePlan *p = (XboxResumePlan *)g_cpu.r_resume;
    const XboxResumeFrame *f;

    if (!p || p->at >= p->n)
        resume_fatal("a function entered in resume mode past the end of the plan", func, 0);
    f = &p->f[p->at];
    if (f->func != func)
        resume_fatal("another function than the plan's was entered", func, f->func);
    *ebp = f->ebp;
    if (p->at + 1 < p->n) {
        /* An inner frame: the call is made again into the next function, with the esp it was entered with (the
         * guest stack, restored, holds its return address and arguments) and the ebp it recorded. */
        g_esp = f->next_esp;
        g_ebp = f->next_ebp;
        g_seh_ebp = f->next_ebp;
        t_target = f->target;
        p->at++;
        return f->site;
    }
    p->at++;
    resume_end(p);
    if (f->site & XBOX_RESUME_HOST_WAIT) {
        /* A host wait: the thread continues after the helper with the registers it had there (the frame
         * pointers stay: they are the ones the chain just rebuilt). */
        memcpy(&g_cpu, &p->regs, sizeof p->regs);
    } else {
        /* A kernel call: made again from its call site, esp at its return address; kernel_thunk_dispatch takes
         * the record's registers (xbox_ResumeKernelArrive). */
        g_esp = p->call_esp;
        t_target = f->target;
        t_kernel = p;
    }
    return f->site;
}

uint32_t recomp_resume_target(void)
{
    return t_target;
}

void recomp_resume_lost(uint32_t func, uint32_t site)
{
    resume_fatal("no resume entry for this site", func, site);
}

int xbox_ResumeKernelPending(void)
{
    return t_kernel != NULL;
}

int xbox_ResumeKernelArrive(uint32_t ordinal)
{
    XboxResumePlan *p = t_kernel;

    t_kernel = NULL;
    if (!p)
        return 0;
    if (ordinal != p->ordinal)
        fprintf(stderr, "  [STATE] resume: kernel call %u re-entered, %u in the state\n", ordinal, p->ordinal);
    memcpy(&g_cpu, &p->regs, sizeof p->regs);
    return p->returned ? 1 : 0;
}

uint32_t xbox_ResumeCompleted(void)
{
    return (uint32_t)s_completed;
}
