/* recomp_cheats.c -- see recomp_cheats.h (XBA-197).
 *
 * Each cheat compiles to a flat op list. One code (one, two or more lines: slides, serial repeaters, group writes,
 * super codes, button writes) is one op, so a conditional's "the next N codes" is "the next N ops". Conditionals are
 * jumps: an IF op holds the index to go to when its test fails; Action Replay v3 blocks (if / else / endif) become an
 * IF whose target is just past the else's JUMP, and that JUMP's target the endif.
 *
 * Code formats, from the published descriptions of each device (EnHacklopedia's GBA page, the GameShark / Action
 * Replay code type lists of each console); our own implementation. The GBA GameShark / Action Replay encryption is a
 * TEA variant (32 rounds, delta 0x9E3779B9) keyed by four seeds that the devices derive from two 256-byte tables
 * and a DEADFACE parameter; with the default parameter they are the constants below (cross-checked by deriving them
 * from those tables while writing this; the tables are not needed here since DEADFACE is not supported). */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE          /* st_mtimespec, hidden by _POSIX_C_SOURCE */
#endif
#include "recomp_cheats.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- Ops ------------------------------------------------------------------------------------------------------ */

enum {
    OP_WRITE = 1, /* size at addr = value, count times, addr += astep, value += vstep */
    OP_ADD,       /* size at addr += value (two's complement: subtract) */
    OP_OR,
    OP_AND,
    OP_IF,        /* when cond(size at addr, value) fails, jump to count */
    OP_JUMP,      /* jump to count */
    OP_PTR,       /* size at (word at addr) + astep = value, when the pointer is in RAM */
    OP_ROMPATCH,  /* GBA: 16-bit cartridge word at addr = value */
    OP_COPY,      /* count bytes from addr to value */
    OP_SUPER,     /* count bytes of data from offset value written from addr */
    OP_GROUP,     /* 32-bit value written to count addresses (data, 4 bytes each, from offset astep) */
    OP_DELAY,     /* PS1 C1: jump to count while fewer than value frames have run */
};

enum {
    C_EQ = 1, C_NE, C_LTU, C_GTU, C_LEU, C_GEU, C_LTS, C_GTS, C_AND, C_FALSE, C_KEYS_HELD, C_KEYS_NOT_HELD,
};

#define TARGET_REL 0x80000000u    /* count = TARGET_REL | n: the next n ops (resolved at the end) */
#define TARGET_OPEN 0x7FFFFFFFu   /* a block not closed yet */
#define TARGET_END 0x7FFFFFFEu    /* the end of the list */
#define OPS_MAX 4096
#define DATA_MAX 65536
#define BLOCKS_MAX 16

const uint32_t recomp_cheat_seeds_gsa[4] = {0x09F4FBBDu, 0x9681884Au, 0x352027E9u, 0xF3DEE5A7u};
const uint32_t recomp_cheat_seeds_arv3[4] = {0x7AA9648Fu, 0x7FAE6994u, 0xC0EFAAD5u, 0x42712C57u};

void recomp_cheat_tea_decrypt(uint32_t *op1, uint32_t *op2, const uint32_t s[4])
{
    uint32_t a = *op1, v = *op2, sum = 0xC6EF3720u;
    for (int i = 0; i < 32; i++) {
        v -= ((a << 4) + s[2]) ^ (a + sum) ^ ((a >> 5) + s[3]);
        a -= ((v << 4) + s[0]) ^ (v + sum) ^ ((v >> 5) + s[1]);
        sum -= 0x9E3779B9u;
    }
    *op1 = a;
    *op2 = v;
}

void recomp_cheat_tea_encrypt(uint32_t *op1, uint32_t *op2, const uint32_t s[4])
{
    uint32_t a = *op1, v = *op2, sum = 0;
    for (int i = 0; i < 32; i++) {
        sum += 0x9E3779B9u;
        a += ((v << 4) + s[0]) ^ (v + sum) ^ ((v >> 5) + s[1]);
        v += ((a << 4) + s[2]) ^ (a + sum) ^ ((a >> 5) + s[3]);
    }
    *op1 = a;
    *op2 = v;
}

static const char *const FORMAT_NAMES[RECOMP_CF_COUNT] = {"auto",     "raw", "gsa", "gsa-raw",
                                                          "arv3",     "arv3-raw", "cb",  "gs"};

const char *recomp_cheat_format_name(recomp_cheat_format f)
{
    return (unsigned)f < RECOMP_CF_COUNT ? FORMAT_NAMES[f] : "?";
}

int recomp_cheat_format_parse(const char *name)
{
    for (int i = 0; i < RECOMP_CF_COUNT; i++)
        if (name && !strcmp(name, FORMAT_NAMES[i]))
            return i;
    return -1;
}

/* ---- The compiler --------------------------------------------------------------------------------------------- */

typedef struct cc {
    recomp_cheat *c;
    recomp_cheat_console con;
    recomp_cheat_format fmt;
    size_t cap, dcap;
    int line;                 /* 1-based, for messages */
    /* A code waiting for its next line(s). */
    int pend;                 /* PEND_* */
    recomp_cheat_op pop;
    uint32_t remaining;       /* group write addresses / super code bytes still to come */
    uint32_t block[BLOCKS_MAX];
    int nblock;
    int ended;                /* AR v3 end-of-list code: the rest is ignored */
    int ignored;              /* lines ignored with a note */
    int ids;                  /* GBA ID codes (xxxxxxxx 001DC0DE) */
} cc;

enum { PEND_NONE, PEND_VALUE, PEND_SLIDE, PEND_PATCH, PEND_GROUP, PEND_SUPER, PEND_SERIAL, PEND_COPY, PEND_CBSLIDE };

static int fail(cc *k, const char *fmt, ...)
{
    if (!k->c->error[0]) {
        char msg[128];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        snprintf(k->c->error, sizeof k->c->error, "line %d: %s", k->line, msg);
    }
    return -1;
}

static int note(cc *k, const char *what)
{
    k->ignored++;
    if (!k->c->note[0])
        snprintf(k->c->note, sizeof k->c->note, "line %d: %s ignored", k->line, what);
    return 0;
}

static recomp_cheat_op *emit(cc *k, int kind, int size, uint32_t addr, uint32_t value)
{
    if (k->c->nops >= OPS_MAX) {
        fail(k, "too many codes");
        return NULL;
    }
    if (k->c->nops == k->cap) {
        size_t cap = k->cap ? k->cap * 2 : 16;
        recomp_cheat_op *p = realloc(k->c->ops, cap * sizeof *p);
        if (!p) {
            fail(k, "out of memory");
            return NULL;
        }
        k->c->ops = p;
        k->cap = cap;
    }
    recomp_cheat_op *o = &k->c->ops[k->c->nops++];
    memset(o, 0, sizeof *o);
    o->kind = (uint8_t)kind;
    o->size = (uint8_t)size;
    o->addr = addr;
    o->value = size == 1 ? value & 0xFF : size == 2 ? value & 0xFFFF : value;
    o->count = 1;
    return o;
}

static int push_data(cc *k, const void *p, size_t n)
{
    if (k->c->ndata + n > DATA_MAX)
        return fail(k, "code too long");
    if (k->c->ndata + n > k->dcap) {
        size_t cap = k->dcap ? k->dcap : 64;
        while (cap < k->c->ndata + n)
            cap *= 2;
        uint8_t *d = realloc(k->c->data, cap);
        if (!d)
            return fail(k, "out of memory");
        k->c->data = d;
        k->dcap = cap;
    }
    memcpy(k->c->data + k->c->ndata, p, n);
    k->c->ndata += n;
    return 0;
}

static recomp_cheat_op *emit_if(cc *k, int cond, int size, uint32_t addr, uint32_t value, uint32_t target)
{
    recomp_cheat_op *o = emit(k, OP_IF, size, addr, value);
    if (o) {
        o->cond = (uint8_t)cond;
        o->count = target;
    }
    return o;
}

static int open_block(cc *k)
{
    if (k->nblock >= BLOCKS_MAX)
        return fail(k, "blocks nested too deep");
    k->block[k->nblock++] = (uint32_t)(k->c->nops - 1);
    return 0;
}

/* ---- GBA ---- */

static uint32_t parv3_addr(uint32_t x)
{
    return (x & 0xFFFFFu) | ((x << 4) & 0x0F000000u);
}

/* GameShark / Action Replay v1-v2, decrypted. */
static int gba_gsa(cc *k, uint32_t a, uint32_t v)
{
    if (k->pend == PEND_GROUP) {
        uint32_t two[2] = {a, v};
        for (int i = 0; i < 2 && k->remaining; i++, k->remaining--) {
            if (push_data(k, &two[i], 4))
                return -1;
            k->c->ops[k->c->nops - 1].count++;
        }
        if (!k->remaining)
            k->pend = PEND_NONE;
        return 0;
    }
    if (v == 0x001DC0DEu) {
        k->ids++;
        return note(k, "game ID code");
    }
    if (a == 0xDEADFACEu)
        return fail(k, "DEADFACE (other encryption seeds) is not supported");
    uint32_t addr = a & 0x0FFFFFFFu;
    switch (a >> 28) {
    case 0x0:
        return emit(k, OP_WRITE, 1, addr, v) ? 0 : -1;
    case 0x1:
        return emit(k, OP_WRITE, 2, addr, v) ? 0 : -1;
    case 0x2:
        return emit(k, OP_WRITE, 4, addr, v) ? 0 : -1;
    case 0x3:
        switch ((a >> 16) & 0xFF) {
        case 0x00: { /* 3000cccc xxxxxxxx, then cccc addresses, two per line */
            uint32_t n = a & 0xFFFF;
            recomp_cheat_op *o = emit(k, OP_GROUP, 4, 0, v);
            if (!o)
                return -1;
            o->count = 0;
            o->astep = (uint32_t)k->c->ndata;
            if (n) {
                k->pend = PEND_GROUP;
                k->remaining = n;
            }
            return 0;
        }
        case 0x10:
            return emit(k, OP_ADD, 1, v & 0x0FFFFFFFu, a & 0xFF) ? 0 : -1;
        case 0x20:
            return emit(k, OP_ADD, 1, v & 0x0FFFFFFFu, 0u - (a & 0xFF)) ? 0 : -1;
        case 0x30:
            return emit(k, OP_ADD, 2, v & 0x0FFFFFFFu, a & 0xFFFF) ? 0 : -1;
        case 0x40:
            return emit(k, OP_ADD, 2, v & 0x0FFFFFFFu, 0u - (a & 0xFFFF)) ? 0 : -1;
        default:
            return fail(k, "GameShark code type 3%X is not supported", (a >> 16) & 0xFF);
        }
    case 0x6: /* 6aaaaaaa Z000yyyy: ROM patch at (aaaaaaa << 1) */
        if (v & 0x0FFF0000u)
            return fail(k, "malformed ROM patch code");
        return emit(k, OP_ROMPATCH, 2, 0x08000000u | ((a & 0xFFFFFFu) << 1), v) ? 0 : -1;
    case 0x8:
        switch ((a >> 20) & 0xF) {
        case 1:
        case 2: {
            recomp_cheat_op *o = emit(k, OP_WRITE, (a >> 20) & 0xF, a & 0x0F0FFFFFu, v);
            if (!o)
                return -1;
            o->once = 1;
            return 0;
        }
        case 0xF:
            return note(k, "slowdown code");
        default:
            return fail(k, "GameShark button code 8x%Xxxxxx is not supported", (a >> 20) & 0xF);
        }
    case 0xD: {
        static const uint8_t conds[4] = {C_EQ, C_NE, C_LEU, C_GEU};
        if ((v >> 20) > 3)
            return fail(k, "unknown GameShark condition");
        return emit_if(k, conds[v >> 20], 2, addr, v, TARGET_REL | 1) ? 0 : -1;
    }
    case 0xE: { /* E0zzyyyy Taaaaaaa: if 16-bit at a (T: ==, !=, <=, >=) yyyy, else skip zz codes */
        static const uint8_t conds[4] = {C_EQ, C_NE, C_LEU, C_GEU};
        if ((v >> 28) > 3)
            return fail(k, "unknown GameShark condition");
        return emit_if(k, conds[v >> 28], 2, v & 0x0FFFFFFFu, a, TARGET_REL | ((a >> 16) & 0xFF)) ? 0 : -1;
    }
    case 0xF:
        return note(k, "hook (master) code");
    default:
        return fail(k, "unknown GameShark code type %X", a >> 28);
    }
}

/* Action Replay v3, decrypted. */
static int gba_arv3(cc *k, uint32_t a, uint32_t v)
{
    recomp_cheat_op *o;
    switch (k->pend) {
    case PEND_VALUE: /* button write: the value */
        o = &k->c->ops[k->c->nops - 1];
        o->value = o->size == 1 ? a & 0xFF : o->size == 2 ? a & 0xFFFF : a;
        k->pend = PEND_NONE;
        return 0;
    case PEND_PATCH:
        k->c->ops[k->c->nops - 1].value = a & 0xFFFF;
        k->pend = PEND_NONE;
        return 0;
    case PEND_SLIDE: /* value, then vinc << 24 | count << 16 | address step (in units) */
        o = &k->c->ops[k->c->nops - 1];
        o->value = o->size == 1 ? a & 0xFF : o->size == 2 ? a & 0xFFFF : a;
        o->vstep = v >> 24;
        o->count = (v >> 16) & 0xFF;
        o->astep = (v & 0xFFFF) * o->size;
        k->pend = PEND_NONE;
        return 0;
    default:
        break;
    }
    if (k->ended)
        return 0;
    if (v == 0x001DC0DEu) {
        k->ids++;
        return note(k, "game ID code");
    }
    if (a == 0xDEADFACEu)
        return fail(k, "DEADFACE (other encryption seeds) is not supported");
    if (a == 0) { /* special codes */
        uint32_t addr = parv3_addr(v);
        switch (v & 0xFF000000u) {
        case 0x00000000u:
            k->ended = 1;
            return 0;
        case 0x08000000u:
            return fail(k, "slowdown codes are not supported");
        case 0x10000000u:
        case 0x12000000u:
        case 0x14000000u:
            o = emit(k, OP_WRITE, 1 << ((v >> 25) & 3), addr, 0);
            if (!o)
                return -1;
            o->once = 1;
            k->pend = PEND_VALUE;
            return 0;
        case 0x18000000u:
        case 0x1A000000u:
        case 0x1C000000u:
        case 0x1E000000u:
            if (!emit(k, OP_ROMPATCH, 2, 0x08000000u | ((v & 0xFFFFFFu) << 1), 0))
                return -1;
            k->pend = PEND_PATCH;
            return 0;
        case 0x40000000u: /* endif */
            if (!k->nblock)
                return fail(k, "endif without if");
            k->c->ops[k->block[--k->nblock]].count = (uint32_t)k->c->nops;
            return 0;
        case 0x60000000u: { /* else */
            if (!k->nblock)
                return fail(k, "else without if");
            if (!emit(k, OP_JUMP, 4, 0, 0))
                return -1;
            k->c->ops[k->c->nops - 1].count = TARGET_OPEN;
            k->c->ops[k->block[k->nblock - 1]].count = (uint32_t)k->c->nops;
            k->block[k->nblock - 1] = (uint32_t)(k->c->nops - 1);
            return 0;
        }
        case 0x80000000u:
        case 0x82000000u:
        case 0x84000000u:
            if (!emit(k, OP_WRITE, 1 << ((v >> 25) & 3), addr, 0))
                return -1;
            k->pend = PEND_SLIDE;
            return 0;
        default:
            return fail(k, "unknown Action Replay special code %08X", v);
        }
    }
    if ((a >> 25) == (0xC4u >> 1))
        return note(k, "hook (master) code");
    uint32_t addr = parv3_addr(a);
    int wbits = (a >> 25) & 3;
    if (a & 0x38000000u) { /* a conditional */
        static const uint8_t conds[8] = {0, C_EQ, C_NE, C_LTS, C_GTS, C_LTU, C_GTU, C_AND};
        int cond = conds[(a >> 27) & 7], size = wbits == 3 ? 4 : 1 << wbits;
        if (wbits == 3)
            cond = C_FALSE;
        if (a & 0x01000000u)
            return fail(k, "unknown Action Replay code %08X", a);
        switch (a >> 30) {
        case 0:
            return emit_if(k, cond, size, addr, v, TARGET_REL | 1) ? 0 : -1;
        case 1:
            return emit_if(k, cond, size, addr, v, TARGET_REL | 2) ? 0 : -1;
        case 2:
            return emit_if(k, cond, size, addr, v, TARGET_OPEN) ? open_block(k) : -1;
        default:
            return emit_if(k, cond, size, addr, v, TARGET_END) ? 0 : -1;
        }
    }
    if ((a >> 30) == 3) { /* C6 / C7: I/O register writes (16 / 32-bit) */
        if ((a & 0xFE000000u) != 0xC6000000u)
            return fail(k, "unknown Action Replay code %08X", a);
        uint32_t off = a & 0x3FF;
        int size = (a & 0x01000000u) ? 4 : 2;
        if (off == 0x006 || off == 0x130) /* VCOUNT, KEYINPUT: read-only */
            return note(k, "write to a read-only register");
        return emit(k, OP_WRITE, size, 0x04000000u | off, v) ? 0 : -1;
    }
    if (wbits == 3 || (a & 0x01000000u))
        return fail(k, "unknown Action Replay code %08X", a);
    int size = 1 << wbits;
    switch (a >> 30) {
    case 0: /* write and fill: the bytes above the value repeat it */
        o = emit(k, OP_WRITE, size, addr, v);
        if (!o)
            return -1;
        if (size < 4) {
            o->count = (v >> (size * 8)) + 1;
            o->astep = (uint32_t)size;
        }
        return 0;
    case 1: /* pointer write */
        o = emit(k, OP_PTR, size, addr, v);
        if (!o)
            return -1;
        if (size < 4)
            o->astep = (v >> (size * 8)) * (uint32_t)size;
        return 0;
    default:
        return emit(k, OP_ADD, size, addr, v) ? 0 : -1;
    }
}

/* CodeBreaker, unencrypted (aaaaaaaa vvvv). */
static int gba_cb(cc *k, uint32_t a, uint32_t v)
{
    recomp_cheat_op *o;
    if (k->pend == PEND_CBSLIDE) { /* vinc << 16 | count, address step */
        o = &k->c->ops[k->c->nops - 1];
        o->vstep = a >> 16;
        o->count = a & 0xFFFF;
        o->astep = v;
        k->pend = PEND_NONE;
        return 0;
    }
    if (k->pend == PEND_SUPER) { /* six bytes per line, in reading order */
        uint8_t b[6] = {(uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a,
                        (uint8_t)(v >> 8), (uint8_t)v};
        uint32_t n = k->remaining < 6 ? k->remaining : 6;
        if (push_data(k, b, n))
            return -1;
        k->remaining -= n;
        if (!k->remaining)
            k->pend = PEND_NONE;
        return 0;
    }
    uint32_t addr = a & 0x0FFFFFFFu;
    switch (a >> 28) {
    case 0x0:
        return note(k, "game CRC (master) code");
    case 0x1:
        return note(k, "hook (master) code");
    case 0x2:
        return emit(k, OP_OR, 2, addr & ~1u, v) ? 0 : -1;
    case 0x3:
        return emit(k, OP_WRITE, 1, addr, v) ? 0 : -1;
    case 0x4:
        if (!emit(k, OP_WRITE, 2, addr & ~1u, v))
            return -1;
        k->pend = PEND_CBSLIDE;
        return 0;
    case 0x5:
        o = emit(k, OP_SUPER, 1, addr, (uint32_t)k->c->ndata);
        if (!o)
            return -1;
        o->value = (uint32_t)k->c->ndata;
        o->count = 2 * (v & 0xFFFF);
        k->remaining = o->count;
        if (k->remaining)
            k->pend = PEND_SUPER;
        return 0;
    case 0x6:
        return emit(k, OP_AND, 2, addr & ~1u, v) ? 0 : -1;
    case 0x7:
        return emit_if(k, C_EQ, 2, addr & ~1u, v, TARGET_REL | 1) ? 0 : -1;
    case 0x8:
        return emit(k, OP_WRITE, 2, addr & ~1u, v) ? 0 : -1;
    case 0x9:
        return fail(k, "encrypted CodeBreaker codes (9xxxxxxx) are not supported: use the decrypted codes");
    case 0xA:
        return emit_if(k, C_NE, 2, addr & ~1u, v, TARGET_REL | 1) ? 0 : -1;
    case 0xB:
        return emit_if(k, C_GTU, 2, addr & ~1u, v, TARGET_REL | 1) ? 0 : -1;
    case 0xC:
        return emit_if(k, C_LTU, 2, addr & ~1u, v, TARGET_REL | 1) ? 0 : -1;
    case 0xD: /* D00000x0 kkkk: x = 0 / 1 keys held, 2 keys not held */
        if ((a & 0xF0) > 0x20)
            return fail(k, "unknown CodeBreaker key condition");
        return emit_if(k, (a & 0xF0) == 0x20 ? C_KEYS_NOT_HELD : C_KEYS_HELD, 2, 0, v, TARGET_REL | 1) ? 0 : -1;
    case 0xE: /* add a signed 16-bit value; odd address: to the 32-bit word */
        if (a & 1)
            return emit(k, OP_ADD, 4, addr & ~1u, (uint32_t)(int32_t)(int16_t)v) ? 0 : -1;
        return emit(k, OP_ADD, 2, addr, v) ? 0 : -1;
    default: /* 0xF: AND test */
        return emit_if(k, C_AND, 2, addr & ~1u, v, TARGET_REL | 1) ? 0 : -1;
    }
}

/* ---- PS1 / N64 GameShark ---- */

static int ps1_gs(cc *k, uint32_t a, uint32_t v)
{
    uint32_t t = a >> 24, addr = 0x80000000u | (a & 0xFFFFFFu);
    recomp_cheat_op *o;
    if (k->pend == PEND_SERIAL) {
        o = &k->c->ops[k->c->nops - 1];
        if (t != 0x80 && t != 0x30)
            return fail(k, "a 50 serial code must be followed by an 80 or 30 code");
        o->size = t == 0x80 ? 2 : 1;
        o->addr = addr;
        o->value = t == 0x80 ? v & 0xFFFF : v & 0xFF;
        k->pend = PEND_NONE;
        return 0;
    }
    if (k->pend == PEND_COPY) {
        if (t != 0x80)
            return fail(k, "a C2 copy code must be followed by an 80 code");
        k->c->ops[k->c->nops - 1].value = addr;
        k->pend = PEND_NONE;
        return 0;
    }
    switch (t) {
    case 0x80:
        return emit(k, OP_WRITE, 2, addr, v) ? 0 : -1;
    case 0x30:
        return emit(k, OP_WRITE, 1, addr, v) ? 0 : -1;
    case 0x10:
        return emit(k, OP_ADD, 2, addr, v) ? 0 : -1;
    case 0x11:
        return emit(k, OP_ADD, 2, addr, 0u - v) ? 0 : -1;
    case 0x20:
        return emit(k, OP_ADD, 1, addr, v) ? 0 : -1;
    case 0x21:
        return emit(k, OP_ADD, 1, addr, 0u - v) ? 0 : -1;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        static const uint8_t conds[4] = {C_EQ, C_NE, C_LTU, C_GTU};
        return emit_if(k, conds[t - 0xD0], 2, addr, v, TARGET_REL | 1) ? 0 : -1;
    }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: {
        static const uint8_t conds[4] = {C_EQ, C_NE, C_LTU, C_GTU};
        return emit_if(k, conds[t - 0xE0], 1, addr, v, TARGET_REL | 1) ? 0 : -1;
    }
    case 0x50: /* 5000nnss vvvv: the next code nn times, address += ss, value += vvvv */
        o = emit(k, OP_WRITE, 2, 0, 0);
        if (!o)
            return -1;
        o->count = (a >> 8) & 0xFF;
        o->astep = a & 0xFF;
        o->vstep = v & 0xFFFF;
        k->pend = PEND_SERIAL;
        return 0;
    case 0xC0: /* every code after it only while the 16-bit value matches */
        return emit_if(k, C_EQ, 2, addr, v, TARGET_END) ? 0 : -1;
    case 0xC1: /* the codes after it start YYYY / 3 frames after the game */
        o = emit(k, OP_DELAY, 4, 0, v / 3);
        if (!o)
            return -1;
        o->count = TARGET_END;
        return 0;
    case 0xC2: /* copy vvvv bytes from addr to the next code's address */
        o = emit(k, OP_COPY, 1, addr, 0);
        if (!o)
            return -1;
        o->count = v & 0xFFFF;
        k->pend = PEND_COPY;
        return 0;
    case 0xD4: case 0xD5: case 0xD6:
        return fail(k, "joker (button) codes D4-D6 are not supported");
    default:
        return fail(k, "unknown PlayStation GameShark code type %02X", t);
    }
}

static int n64_gs(cc *k, uint32_t a, uint32_t v)
{
    uint32_t t = a >> 24, addr = 0x80000000u | (a & 0xFFFFFFu);
    recomp_cheat_op *o;
    if (k->pend == PEND_SERIAL) {
        o = &k->c->ops[k->c->nops - 1];
        if (t != 0x80 && t != 0x81 && t != 0xA0 && t != 0xA1)
            return fail(k, "a 50 serial code must be followed by an 80 / 81 code");
        o->size = (t & 1) ? 2 : 1;
        o->addr = addr;
        o->value = (t & 1) ? v & 0xFFFF : v & 0xFF;
        k->pend = PEND_NONE;
        return 0;
    }
    switch (t) {
    case 0x80:
    case 0xA0:
        return emit(k, OP_WRITE, 1, addr, v) ? 0 : -1;
    case 0x81:
    case 0xA1:
        return emit(k, OP_WRITE, 2, addr, v) ? 0 : -1;
    case 0x88: /* the GameShark button: once, when the cheat is turned on */
    case 0x89:
    case 0xF0: /* boot-time writes: once, at the first frame */
    case 0xF1:
        o = emit(k, OP_WRITE, (t & 1) ? 2 : 1, addr, v);
        if (!o)
            return -1;
        o->once = 1;
        return 0;
    case 0xD0:
        return emit_if(k, C_EQ, 1, addr, v, TARGET_REL | 1) ? 0 : -1;
    case 0xD1:
        return emit_if(k, C_EQ, 2, addr, v, TARGET_REL | 1) ? 0 : -1;
    case 0xD2:
        return emit_if(k, C_NE, 1, addr, v, TARGET_REL | 1) ? 0 : -1;
    case 0xD3:
        return emit_if(k, C_NE, 2, addr, v, TARGET_REL | 1) ? 0 : -1;
    case 0x50:
        o = emit(k, OP_WRITE, 2, 0, 0);
        if (!o)
            return -1;
        o->count = (a >> 8) & 0xFF;
        o->astep = a & 0xFF;
        o->vstep = (uint32_t)(int32_t)(int16_t)v;
        k->pend = PEND_SERIAL;
        return 0;
    case 0xDE:
        return fail(k, "DE (entry point) codes are not supported");
    case 0xEE:
    case 0xFF:
    case 0xCC:
        return note(k, "enabler code");
    default:
        return fail(k, "unknown N64 GameShark code type %02X", t);
    }
}

/* raw: address and value, the size from the value's digits. */
static int raw_line(cc *k, uint32_t a, uint32_t v, int lv)
{
    int size = lv <= 2 ? 1 : lv <= 4 ? 2 : 4;
    return emit(k, OP_WRITE, size, a, v) ? 0 : -1;
}

/* ---- Lines ---- */

typedef struct line {
    uint32_t a, v;
    int la, lv;    /* digits of each */
    int colon;     /* written ADDR:VALUE */
} line;

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch = tolower(ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

/* One code line: 0 blank, 1 parsed, -1 malformed. Separators: space, tab, ':', '-', ','; '#' starts a comment; a
 * 0x prefix is accepted; 16 or 12 digits without a separator split 8 + rest. */
static int parse_line(const char *s, size_t n, line *out)
{
    char tok[2][24];
    int nt = 0, len = 0;
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i <= n; i++) {
        int ch = i < n ? (unsigned char)s[i] : ' ';
        if (ch == '#')
            ch = ' ', n = i;
        if (ch == ':')
            out->colon = 1;
        if (hexval(ch) >= 0 || ch == 'x' || ch == 'X') {
            if (nt >= 2 || len >= 23)
                return -1;
            tok[nt][len++] = (char)ch;
        } else if (ch == ' ' || ch == '\t' || ch == ':' || ch == '-' || ch == ',' || ch == '\r') {
            if (len) {
                tok[nt][len] = 0;
                nt++;
                len = 0;
            }
        } else {
            return -1;
        }
    }
    if (!nt)
        return 0;
    for (int t = 0; t < nt; t++) { /* strip 0x */
        char *p = tok[t];
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            memmove(p, p + 2, strlen(p + 2) + 1);
        for (char *q = p; *q; q++)
            if (hexval((unsigned char)*q) < 0)
                return -1;
        if (!*p)
            return -1;
    }
    if (nt == 1) {
        size_t l = strlen(tok[0]);
        if (l != 16 && l != 12)
            return -1;
        memcpy(tok[1], tok[0] + 8, l - 8 + 1);
        tok[0][8] = 0;
        nt = 2;
    }
    if (strlen(tok[0]) > 8 || strlen(tok[1]) > 8)
        return -1;
    out->a = (uint32_t)strtoul(tok[0], NULL, 16);
    out->v = (uint32_t)strtoul(tok[1], NULL, 16);
    out->la = (int)strlen(tok[0]);
    out->lv = (int)strlen(tok[1]);
    return 1;
}

static int line_fits(recomp_cheat_format f, const line *l)
{
    switch (f) {
    case RECOMP_CF_RAW:
        return 1;
    case RECOMP_CF_GSA:
    case RECOMP_CF_GSA_RAW:
    case RECOMP_CF_ARV3:
    case RECOMP_CF_ARV3_RAW:
        return l->la == 8 && l->lv == 8;
    case RECOMP_CF_CB:
    case RECOMP_CF_GS:
        return l->la == 8 && l->lv == 4;
    default:
        return 0;
    }
}

static int formats_ok(recomp_cheat_console con, recomp_cheat_format f)
{
    if (f == RECOMP_CF_RAW)
        return 1;
    if (con == RECOMP_CHEAT_GBA)
        return f >= RECOMP_CF_GSA && f <= RECOMP_CF_CB;
    return f == RECOMP_CF_GS;
}

static void finish(cc *k)
{
    recomp_cheat *c = k->c;
    for (size_t i = 0; i < c->nops; i++) {
        recomp_cheat_op *o = &c->ops[i];
        if (o->kind != OP_IF && o->kind != OP_JUMP && o->kind != OP_DELAY)
            continue;
        uint32_t t = o->count;
        if (t & TARGET_REL)
            t = (uint32_t)i + 1 + (t & ~TARGET_REL);
        else if (t == TARGET_OPEN || t == TARGET_END)
            t = (uint32_t)c->nops;
        o->count = t > c->nops ? (uint32_t)c->nops : t;
    }
}

/* Compiles in one format (not AUTO). */
static int compile_as(recomp_cheat *c, recomp_cheat_console con, recomp_cheat_format f, const char *code)
{
    cc k;
    memset(&k, 0, sizeof k);
    k.c = c;
    k.con = con;
    k.fmt = f;
    c->used = f;
    const char *p = code ? code : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        k.line++;
        line l;
        int r = parse_line(p, n, &l);
        p += n + (e ? 1 : 0);
        if (r == 0)
            continue;
        if (r < 0 || !line_fits(f, &l)) {
            if (k.pend == PEND_SUPER && f == RECOMP_CF_CB && r > 0) {
                /* the last super code line may be short */
            } else {
                return fail(&k, r < 0 ? "not a code line" : "not a %s code line", recomp_cheat_format_name(f));
            }
        }
        int rc;
        uint32_t a = l.a, v = l.v;
        switch (f) {
        case RECOMP_CF_RAW:
            rc = raw_line(&k, a, v, l.lv);
            break;
        case RECOMP_CF_GSA:
            recomp_cheat_tea_decrypt(&a, &v, recomp_cheat_seeds_gsa);
            rc = gba_gsa(&k, a, v);
            break;
        case RECOMP_CF_GSA_RAW:
            rc = gba_gsa(&k, a, v);
            break;
        case RECOMP_CF_ARV3:
            recomp_cheat_tea_decrypt(&a, &v, recomp_cheat_seeds_arv3);
            rc = gba_arv3(&k, a, v);
            break;
        case RECOMP_CF_ARV3_RAW:
            rc = gba_arv3(&k, a, v);
            break;
        case RECOMP_CF_CB:
            rc = gba_cb(&k, a, v);
            break;
        default:
            rc = con == RECOMP_CHEAT_PS1 ? ps1_gs(&k, a, v) : n64_gs(&k, a, v);
            break;
        }
        if (rc)
            return -1;
    }
    if (k.pend != PEND_NONE)
        return fail(&k, "the last code is missing a line");
    if (!c->nops && !k.ignored)
        return fail(&k, "no code");
    finish(&k);
    return 0;
}

/* How believable a GBA reading is: codes on real memory score, others cost. */
static int gba_real(uint32_t a)
{
    return (a >= 0x02000000u && a < 0x02040000u) || (a >= 0x03000000u && a < 0x03008000u) ||
           (a >= 0x04000000u && a < 0x04000400u) || (a >= 0x05000000u && a < 0x05000400u) ||
           (a >= 0x06000000u && a < 0x06018000u) || (a >= 0x07000000u && a < 0x07000400u);
}

static int gba_score(const recomp_cheat *c)
{
    int s = 0;
    for (size_t i = 0; i < c->nops; i++) {
        const recomp_cheat_op *o = &c->ops[i];
        switch (o->kind) {
        case OP_ROMPATCH:
            s += o->addr < 0x0A000000u ? 3 : -4;
            break;
        case OP_JUMP:
            s += 2;
            break;
        case OP_IF:
            s += o->cond == C_KEYS_HELD || o->cond == C_KEYS_NOT_HELD ? 1 : gba_real(o->addr) ? 4 : -4;
            break;
        case OP_GROUP:
            s += 1;
            break;
        default:
            s += gba_real(o->addr) ? 4 : -4;
            break;
        }
    }
    return s;
}

static void clear_ops(recomp_cheat *c)
{
    free(c->ops);
    free(c->data);
    c->ops = NULL;
    c->data = NULL;
    c->nops = c->ndata = 0;
    c->error[0] = c->note[0] = 0;
}

void recomp_cheat_clear(recomp_cheat *c)
{
    clear_ops(c);
}

int recomp_cheat_compile(recomp_cheat *c, recomp_cheat_console con, recomp_cheat_format fmt, const char *code)
{
    clear_ops(c);
    if (fmt != RECOMP_CF_AUTO) {
        if (!formats_ok(con, fmt)) {
            snprintf(c->error, sizeof c->error, "format %s is not for this console", recomp_cheat_format_name(fmt));
            c->used = fmt;
            return -1;
        }
        return compile_as(c, con, fmt, code);
    }
    /* AUTO: what the lines look like. */
    int colon = 0, n84 = 0, n88 = 0, nlines = 0;
    const char *p = code ? code : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        line l;
        int r = parse_line(p, n, &l);
        p += n + (e ? 1 : 0);
        if (r <= 0)
            continue;
        nlines++;
        colon |= l.colon;
        n84 += l.la == 8 && l.lv == 4;
        n88 += l.la == 8 && l.lv == 8;
    }
    if (colon || con != RECOMP_CHEAT_GBA) {
        recomp_cheat_format f = colon ? RECOMP_CF_RAW : RECOMP_CF_GS;
        if (!colon && nlines && n84 != nlines && con != RECOMP_CHEAT_GBA && n84 == 0)
            f = RECOMP_CF_RAW; /* ADDR VALUE with other widths */
        return compile_as(c, con, f, code);
    }
    if (n84 && n84 >= n88)
        return compile_as(c, con, RECOMP_CF_CB, code);
    if (!n88)
        return compile_as(c, con, RECOMP_CF_RAW, code);
    static const recomp_cheat_format cand[4] = {RECOMP_CF_GSA, RECOMP_CF_ARV3, RECOMP_CF_GSA_RAW,
                                                RECOMP_CF_ARV3_RAW};
    int best = -1, best_score = 0;
    recomp_cheat t;
    for (int i = 0; i < 4; i++) {
        memset(&t, 0, sizeof t);
        if (compile_as(&t, con, cand[i], code) == 0) {
            int sc = gba_score(&t) + 2 * (int)(t.note[0] != 0);
            /* ID codes (xxxxxxxx 001DC0DE) only decode in the right reading */
            if (strstr(t.note, "game ID"))
                sc += 50;
            if (best < 0 || sc > best_score) {
                best = i;
                best_score = sc;
            }
        }
        clear_ops(&t);
    }
    if (best < 0) {
        int r = compile_as(c, con, RECOMP_CF_GSA, code);
        if (r) {
            char why[160];
            snprintf(why, sizeof why, "not a GameShark / Action Replay code (%.100s)", c->error);
            snprintf(c->error, sizeof c->error, "%s", why);
            c->used = RECOMP_CF_AUTO;
        }
        return r;
    }
    return compile_as(c, con, cand[best], code);
}

/* ---- The list ------------------------------------------------------------------------------------------------- */

void recomp_cheats_init(recomp_cheats *s, recomp_cheat_console console)
{
    memset(s, 0, sizeof *s);
    s->console = console;
    s->mtime_ns = s->size = -1;
}

static void free_list(recomp_cheats *s)
{
    for (size_t i = 0; i < s->n; i++)
        clear_ops(&s->list[i]);
    free(s->list);
    s->list = NULL;
    s->n = 0;
}

void recomp_cheats_free(recomp_cheats *s)
{
    free_list(s);
}

static char *trim(char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'))
        p[--n] = 0;
    return p;
}

int recomp_cheats_parse_text(recomp_cheats *s, const char *text)
{
    /* Keep who was on and done, so a reload does not repeat once-codes. */
    typedef struct prev { char name[96]; int on; } prev;
    size_t np = s->n;
    prev *was = np ? calloc(np, sizeof *was) : NULL;
    for (size_t i = 0; was && i < np; i++) {
        memcpy(was[i].name, s->list[i].name, sizeof was[i].name);
        was[i].on = s->list[i].enabled && s->list[i].ran;
    }
    free_list(s);
    recomp_cheat *list = calloc(RECOMP_CHEATS_MAX, sizeof *list);
    char **codes = calloc(RECOMP_CHEATS_MAX, sizeof *codes);
    size_t *clen = calloc(RECOMP_CHEATS_MAX, sizeof *clen);
    if (!list || !codes || !clen) {
        free(list);
        free(codes);
        free(clen);
        free(was);
        return 0;
    }
    size_t n = 0;
    const char *p = text ? text : "";
    char buf[1024];
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        size_t cp = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, p, cp);
        buf[cp] = 0;
        p += len + (e ? 1 : 0);
        char *l = trim(buf);
        if (!*l || *l == '#')
            continue;
        if (*l == '[') {
            char *end = strrchr(l, ']');
            if (!end || n >= RECOMP_CHEATS_MAX)
                continue;
            *end = 0;
            recomp_cheat *c = &list[n++];
            snprintf(c->name, sizeof c->name, "%s", trim(l + 1));
            c->enabled = 0;
            continue;
        }
        if (!n)
            continue;
        recomp_cheat *c = &list[n - 1];
        char *eq = strchr(l, '=');
        if (eq) {
            *eq = 0;
            char *key = trim(l), *val = trim(eq + 1);
            if (!strcmp(key, "enabled"))
                c->enabled = !strcmp(val, "1") || !strcmp(val, "on") || !strcmp(val, "true") || !strcmp(val, "yes");
            else if (!strcmp(key, "format")) {
                int f = recomp_cheat_format_parse(val);
                c->format = f < 0 ? RECOMP_CF_COUNT : (recomp_cheat_format)f;
            }
            continue;
        }
        size_t ll = strlen(l);
        char *nc = realloc(codes[n - 1], clen[n - 1] + ll + 2);
        if (!nc)
            continue;
        memcpy(nc + clen[n - 1], l, ll);
        clen[n - 1] += ll;
        nc[clen[n - 1]++] = '\n';
        nc[clen[n - 1]] = 0;
        codes[n - 1] = nc;
    }
    for (size_t i = 0; i < n; i++) {
        recomp_cheat *c = &list[i];
        if (c->format == RECOMP_CF_COUNT) {
            snprintf(c->error, sizeof c->error, "unknown format");
            c->format = RECOMP_CF_AUTO;
        } else {
            recomp_cheat_compile(c, s->console, c->format, codes[i] ? codes[i] : "");
        }
        free(codes[i]);
        c->ran = 0;
        for (size_t j = 0; was && j < np; j++)
            if (was[j].on && !strcmp(was[j].name, c->name)) {
                c->ran = 1;
                break;
            }
    }
    free(codes);
    free(clen);
    free(was);
    s->list = list;
    s->n = n;
    return (int)n;
}

static int stat_of(const char *path, long long *mtime_ns, long long *size)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
#if defined(__APPLE__)
    *mtime_ns = (long long)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec;
#else
    *mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
#endif
    *size = (long long)st.st_size;
    return 0;
}

static void log_list(const recomp_cheats *s, const char *tag)
{
    for (size_t i = 0; i < s->n; i++) {
        const recomp_cheat *c = &s->list[i];
        if (c->error[0])
            printf("%s: cheat [%s] \"%s\" not applied: %s\n", tag, c->enabled ? "on" : "off", c->name, c->error);
        else
            printf("%s: cheat [%s] \"%s\" (%s, %zu op%s)%s%s\n", tag, c->enabled ? "on" : "off", c->name,
                   recomp_cheat_format_name(c->used), c->nops, c->nops == 1 ? "" : "s", c->note[0] ? ", " : "",
                   c->note);
    }
    fflush(stdout);
}

static const char *tag_of(const recomp_cheats *s)
{
    return s->console == RECOMP_CHEAT_GBA ? "gba" : s->console == RECOMP_CHEAT_PS1 ? "ps1" : "n64";
}

int recomp_cheats_load(recomp_cheats *s, const char *path)
{
    snprintf(s->path, sizeof s->path, "%s", path ? path : "");
    if (!path || !*path)
        return 0;
    long long mt = -1, sz = -1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        s->mtime_ns = s->size = -1;
        recomp_cheats_parse_text(s, "");
        if (errno == ENOENT) {
            printf("%s: cheats: %s does not exist (yet): none\n", tag_of(s), path);
            return 0;
        }
        fprintf(stderr, "%s: cheats: cannot read %s\n", tag_of(s), path);
        return -1;
    }
    stat_of(path, &mt, &sz);
    char *text = NULL;
    size_t n = 0, cap = 0;
    char chunk[4096];
    size_t r;
    while ((r = fread(chunk, 1, sizeof chunk, f)) > 0 && n < (1u << 20)) {
        if (n + r + 1 > cap) {
            cap = (n + r + 1) * 2;
            char *t = realloc(text, cap);
            if (!t)
                break;
            text = t;
        }
        memcpy(text + n, chunk, r);
        n += r;
    }
    fclose(f);
    if (text)
        text[n] = 0;
    int cnt = recomp_cheats_parse_text(s, text ? text : "");
    free(text);
    s->mtime_ns = mt;
    s->size = sz;
    printf("%s: cheats: %d from %s, %zu on\n", tag_of(s), cnt, path, recomp_cheats_active(s));
    log_list(s, tag_of(s));
    return cnt;
}

int recomp_cheats_load_env(recomp_cheats *s, const char *tag)
{
    (void)tag;
    const char *p = getenv("RECOMP_CHEATS_FILE");
    if (!p || !*p)
        return 0;
    return recomp_cheats_load(s, p);
}

int recomp_cheats_poll(recomp_cheats *s)
{
    if (!s->path[0])
        return 0;
    long long mt = -1, sz = -1;
    if (stat_of(s->path, &mt, &sz) != 0)
        mt = sz = -1;
    if (mt == s->mtime_ns && sz == s->size)
        return 0;
    char path[sizeof s->path];
    memcpy(path, s->path, sizeof path);
    recomp_cheats_load(s, path);
    return 1;
}

size_t recomp_cheats_active(const recomp_cheats *s)
{
    size_t n = 0;
    for (size_t i = 0; i < s->n; i++)
        n += s->list[i].enabled && !s->list[i].error[0];
    return n;
}

/* ---- Running ---------------------------------------------------------------------------------------------------- */

static uint32_t mask_of(int size)
{
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

static uint32_t bus_read(const recomp_cheat_bus *b, uint32_t addr, int size)
{
    if (b->read)
        return b->read(b->user, addr, size) & mask_of(size);
    if (!b->read32)
        return 0;
    if (size == 4 && !(addr & 3))
        return b->read32(b->user, addr);
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        uint32_t x = addr + (uint32_t)i, w = b->read32(b->user, x & ~3u);
        int sh = b->big_endian ? (int)(3 - (x & 3)) * 8 : (int)(x & 3) * 8;
        uint32_t byte = (w >> sh) & 0xFF;
        v |= b->big_endian ? byte << ((size - 1 - i) * 8) : byte << (i * 8);
    }
    return v;
}

static void bus_write(const recomp_cheat_bus *b, uint32_t addr, uint32_t v, int size)
{
    v &= mask_of(size);
    if (bus_read(b, addr, size) == v)
        return; /* unchanged: no write, no code page invalidated */
    if (b->write) {
        b->write(b->user, addr, v, size);
        return;
    }
    if (!b->read32 || !b->write32)
        return;
    /* Read-modify-write of the word(s) the bytes fall in. */
    for (int i = 0; i < size;) {
        uint32_t x = addr + (uint32_t)i, wa = x & ~3u, w = b->read32(b->user, wa), nw = w;
        for (; i < size && ((addr + (uint32_t)i) & ~3u) == wa; i++) {
            uint32_t xi = addr + (uint32_t)i;
            uint32_t byte = b->big_endian ? (v >> ((size - 1 - i) * 8)) & 0xFF : (v >> (i * 8)) & 0xFF;
            int sh = b->big_endian ? (int)(3 - (xi & 3)) * 8 : (int)(xi & 3) * 8;
            nw = (nw & ~(0xFFu << sh)) | byte << sh;
        }
        if (nw != w)
            b->write32(b->user, wa, nw);
    }
}

static int test(const recomp_cheat_op *o, const recomp_cheat_bus *b)
{
    if (o->cond == C_FALSE)
        return 0;
    if (o->cond == C_KEYS_HELD)
        return (b->keys & o->value) == o->value;
    if (o->cond == C_KEYS_NOT_HELD)
        return (b->keys & o->value) == 0;
    uint32_t x = bus_read(b, o->addr, o->size), y = o->value;
    int32_t sx = o->size == 1 ? (int8_t)x : o->size == 2 ? (int16_t)x : (int32_t)x;
    int32_t sy = o->size == 1 ? (int8_t)y : o->size == 2 ? (int16_t)y : (int32_t)y;
    switch (o->cond) {
    case C_EQ: return x == y;
    case C_NE: return x != y;
    case C_LTU: return x < y;
    case C_GTU: return x > y;
    case C_LEU: return x <= y;
    case C_GEU: return x >= y;
    case C_LTS: return sx < sy;
    case C_GTS: return sx > sy;
    case C_AND: return (x & y) != 0;
    default: return 0;
    }
}

static int ptr_ok(recomp_cheat_console con, uint32_t p)
{
    switch (con) {
    case RECOMP_CHEAT_GBA:
        return (p >= 0x02000000u && p < 0x02040000u) || (p >= 0x03000000u && p < 0x03008000u);
    case RECOMP_CHEAT_PS1:
        return (p & 0x1FFFFFFFu) < 0x00200000u;
    default:
        return p >= 0x80000000u && p < 0x80800000u;
    }
}

static void want_patch(recomp_cheat_patch *w, size_t *nw, uint32_t addr, uint16_t v)
{
    for (size_t i = 0; i < *nw; i++)
        if (w[i].addr == addr) {
            w[i].value = v;
            return;
        }
    if (*nw < RECOMP_CHEAT_PATCHES_MAX) {
        w[*nw].addr = addr;
        w[*nw].value = v;
        (*nw)++;
    }
}

static void run(const recomp_cheats *s, recomp_cheat *c, const recomp_cheat_bus *b, recomp_cheat_patch *want,
                size_t *nwant)
{
    int once = !c->ran;
    size_t budget = 65536; /* writes per cheat per frame, at most (a runaway fill / repeat) */
    for (size_t i = 0; i < c->nops;) {
        const recomp_cheat_op *o = &c->ops[i];
        switch (o->kind) {
        case OP_WRITE:
            if (o->once && !once)
                break;
            for (uint32_t r = 0; r < o->count && budget; r++, budget--)
                bus_write(b, o->addr + r * o->astep, o->value + r * o->vstep, o->size);
            break;
        case OP_ADD:
            bus_write(b, o->addr, bus_read(b, o->addr, o->size) + o->value, o->size);
            break;
        case OP_OR:
            bus_write(b, o->addr, bus_read(b, o->addr, o->size) | o->value, o->size);
            break;
        case OP_AND:
            bus_write(b, o->addr, bus_read(b, o->addr, o->size) & o->value, o->size);
            break;
        case OP_IF:
            if (!test(o, b)) {
                i = o->count;
                continue;
            }
            break;
        case OP_JUMP:
            i = o->count;
            continue;
        case OP_DELAY:
            if (s->frames < o->value) {
                i = o->count;
                continue;
            }
            break;
        case OP_PTR: {
            uint32_t p = bus_read(b, o->addr, 4);
            if (ptr_ok(s->console, p))
                bus_write(b, p + o->astep, o->value, o->size);
            break;
        }
        case OP_ROMPATCH:
            want_patch(want, nwant, o->addr, (uint16_t)o->value);
            break;
        case OP_COPY:
            for (uint32_t k = 0; k < o->count && budget; k++, budget--)
                bus_write(b, o->value + k, bus_read(b, o->addr + k, 1), 1);
            break;
        case OP_SUPER:
            for (uint32_t k = 0; k < o->count && o->value + k < c->ndata; k++)
                bus_write(b, o->addr + k, c->data[o->value + k], 1);
            break;
        case OP_GROUP:
            for (uint32_t k = 0; k < o->count && o->astep + 4 * k + 4 <= c->ndata; k++) {
                uint32_t a;
                memcpy(&a, c->data + o->astep + 4 * k, 4);
                bus_write(b, a & 0x0FFFFFFFu, o->value, 4);
            }
            break;
        default:
            break;
        }
        i++;
    }
    c->ran = 1;
}

static void patch_rom(const recomp_cheat_bus *b, uint32_t addr, uint16_t v)
{
    if (b->rom_write16)
        b->rom_write16(b->user, addr, v);
}

size_t recomp_cheats_apply(recomp_cheats *s, const recomp_cheat_bus *b)
{
    recomp_cheat_patch want[RECOMP_CHEAT_PATCHES_MAX];
    size_t nwant = 0;
    for (size_t i = 0; i < s->n; i++) {
        recomp_cheat *c = &s->list[i];
        if (c->enabled && !c->error[0])
            run(s, c, b, want, &nwant);
        else
            c->ran = 0; /* turned on later: its once-codes then */
    }
    s->frames++;
    /* Cartridge patches: put back the ones no longer wanted, then the new ones. */
    if (b->rom_write16 && (nwant || s->npatch)) {
        for (size_t i = 0; i < s->npatch;) {
            size_t j = 0;
            while (j < nwant && want[j].addr != s->patch[i].addr)
                j++;
            if (j == nwant) {
                patch_rom(b, s->patch[i].addr, s->patch[i].old);
                s->patch[i] = s->patch[--s->npatch];
                continue;
            }
            if (want[j].value != s->patch[i].value) {
                s->patch[i].value = want[j].value;
                patch_rom(b, s->patch[i].addr, want[j].value);
            }
            i++;
        }
        for (size_t j = 0; j < nwant; j++) {
            size_t i = 0;
            while (i < s->npatch && s->patch[i].addr != want[j].addr)
                i++;
            if (i < s->npatch || s->npatch >= RECOMP_CHEAT_PATCHES_MAX)
                continue;
            s->patch[s->npatch].addr = want[j].addr;
            s->patch[s->npatch].value = want[j].value;
            s->patch[s->npatch].old = (uint16_t)bus_read(b, want[j].addr, 2);
            s->npatch++;
            patch_rom(b, want[j].addr, want[j].value);
        }
    }
    return s->npatch;
}

void recomp_cheats_restore(recomp_cheats *s, const recomp_cheat_bus *b)
{
    while (s->npatch) {
        s->npatch--;
        patch_rom(b, s->patch[s->npatch].addr, s->patch[s->npatch].old);
    }
}
