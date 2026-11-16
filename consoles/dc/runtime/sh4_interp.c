/* sh4_interp.c -- the SH-4 interpreter (XBA-227): decodes with sh4_optab.h (generated from dc_tools/sh4.py), runs
 * sh4_ops.h's semantics, handles the branches and delay slots itself. It runs whatever lifted code does not cover
 * and records where it had to start inside the lifted image (dc_recomp_missed). */
#include <stdio.h>
#include <stdlib.h>

#include "dc_internal.h"
#include "sh4_optab.h"

enum {
#define X(NAME, MASK, MATCH, FLAGS) OP_##NAME,
    SH4_OPTAB(X)
#undef X
    OP_ILLEGAL,
    OP_COUNT
};

static const char *const op_names[OP_COUNT] = {
#define X(NAME, MASK, MATCH, FLAGS) #NAME,
    SH4_OPTAB(X)
#undef X
    "-",
};

static const struct {
    uint16_t mask, match;
} op_pat[OP_ILLEGAL] = {
#define X(NAME, MASK, MATCH, FLAGS) {MASK, MATCH},
    SH4_OPTAB(X)
#undef X
};

static uint8_t decode_tab[65536];
static int decode_ready;

void sh4_decode_init(void)
{
    if (decode_ready)
        return;
    for (uint32_t op = 0; op < 65536; op++) {
        uint8_t id = OP_ILLEGAL;
        for (int i = 0; i < OP_ILLEGAL; i++) {
            if ((op & op_pat[i].mask) == op_pat[i].match) {
                id = (uint8_t)i;
                break;
            }
        }
        decode_tab[op] = id;
    }
    decode_ready = 1;
}

void dc_dump_decode_table(void *file)
{
    sh4_decode_init();
    for (uint32_t op = 0; op < 65536; op++)
        fprintf((FILE *)file, "%04x %s\n", op, op_names[decode_tab[op]]);
}

/* A non-branch instruction (also what a delay slot runs). */
static void exec_plain(sh4_cpu *c, uint32_t pc, uint32_t op, uint8_t id)
{
    switch (id) {
#define X(NAME, MASK, MATCH, FLAGS)                                                                                  \
    case OP_##NAME:                                                                                                  \
        if (!((FLAGS) & SH4F_BRANCH))                                                                                \
            sh4i_##NAME(c, pc, op);                                                                                  \
        break;
        SH4_OPTAB(X)
#undef X
    default:
        break;
    }
}

static void note_missed(dc_machine *m, uint32_t pc)
{
    if (!m->rc || pc < m->img_lo || pc >= m->img_hi)
        return;
    for (size_t i = 0; i < m->nmissed; i++)
        if (m->missed[i] == pc)
            return;
    if (m->nmissed == m->missed_cap) {
        size_t cap = m->missed_cap ? m->missed_cap * 2 : 256;
        uint32_t *p = realloc(m->missed, cap * sizeof *p);
        if (!p)
            return;
        m->missed = p;
        m->missed_cap = cap;
    }
    m->missed[m->nmissed++] = pc;
}

uint32_t sh4_interp_run(dc_machine *m, uint32_t max)
{
    sh4_cpu *c = &m->cpu;
    uint32_t ran = 0;
    note_missed(m, c->pc);
    while (ran < max) {
        uint32_t pc = c->pc;
        uint32_t op = sh4_r16(c, pc);
        uint8_t id = decode_tab[op];
        bool branched = false;
        uint32_t tgt = 0;
        ran++;
        c->cycles++;
        switch (id) {
        case OP_BT:
        case OP_BF:
            if (c->t == (id == OP_BT)) {
                c->pc = sh4_bdisp8(pc, op);
                branched = true;
            } else {
                c->pc = pc + 2;
            }
            break;
        case OP_BT_S:
        case OP_BF_S: {
            bool take = c->t == (id == OP_BT_S);
            uint32_t sop = sh4_r16(c, pc + 2);
            tgt = sh4_bdisp8(pc, op);
            exec_plain(c, pc + 2, sop, decode_tab[sop]);
            ran++;
            c->cycles++;
            c->pc = take ? tgt : pc + 4;
            branched = take;
            break;
        }
        case OP_BRA:
        case OP_BSR:
        case OP_BRAF:
        case OP_BSRF:
        case OP_JMP:
        case OP_JSR:
        case OP_RTS:
        case OP_RTE: {
            unsigned n = (op >> 8) & 15;
            switch (id) {
            case OP_BRA: tgt = sh4_bdisp12(pc, op); break;
            case OP_BSR: tgt = sh4_bdisp12(pc, op); c->pr = pc + 4; break;
            case OP_BRAF: tgt = pc + 4 + c->r[n]; break;
            case OP_BSRF: tgt = pc + 4 + c->r[n]; c->pr = pc + 4; break;
            case OP_JMP: tgt = c->r[n]; break;
            case OP_JSR: tgt = c->r[n]; c->pr = pc + 4; break;
            case OP_RTS: tgt = c->pr; break;
            default: /* RTE: SR comes back before the delay slot runs */
                tgt = c->spc;
                sh4_set_sr(c, c->ssr);
                break;
            }
            uint32_t sop = sh4_r16(c, pc + 2);
            exec_plain(c, pc + 2, sop, decode_tab[sop]);
            ran++;
            c->cycles++;
            c->pc = tgt;
            branched = true;
            if (id == OP_RTE)
                return ran; /* interrupts may be unmasked */
            break;
        }
        case OP_LDC_SR:
        case OP_LDCL_SR:
        case OP_SLEEP:
        case OP_TRAPA:
            exec_plain(c, pc, op, id); /* they set c->pc */
            return ran;
        case OP_ILLEGAL:
            if (m->verbose)
                fprintf(stderr, "dc: illegal instruction %04x at %08x\n", op, pc);
            c->expevt = 0x180;
            sh4_enter_exception(c, 0x100, pc);
            return ran;
        default:
            exec_plain(c, pc, op, id);
            c->pc = pc + 2;
            break;
        }
        if (c->cycles >= c->next_event)
            break;
        if (branched && (dc_bios_is_stub(c->pc) || (m->rc && dc_rc_lookup(m, c->pc))))
            break;
    }
    return ran;
}
