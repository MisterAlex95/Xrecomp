/* The PS1 HLE BIOS (P-5 HLE, XBA-163): see ps1_hle.h for the design and consoles/ps1/HLE_BIOS.md for the call list.
 *
 * Behaviour follows OpenBIOS (MIT, PCSX-Redux's nugget tree: the BIOS our LLE path builds from source) and nocash's
 * psx-spx « BIOS Function Summary »; the code is our own. Devices: `cdrom:` reads ISO 9660 straight from the disc
 * image (ps1_disc), `bu00:` / `bu10:` work on the memory card images (ps1_card) in the card's own file system, so
 * neither goes through the CD-ROM controller or SIO0 (games that drive those chips themselves still can). */
#define _POSIX_C_SOURCE 200809L /* strnlen, strncasecmp */
#include "ps1_hle.h"
#include "ps1_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- Kernel RAM layout (see ps1_hle.h) ---- */
#define K_RAMSIZE 0x0060u     /* 0x60: RAM size in MB (2), 0x64 / 0x68 as OpenBIOS */
#define K_TOT 0x0100u         /* table of tables */
#define K_CMDLINE 0x0180u
#define K_VARS 0x0A00u        /* our kernel variables: +0 exception jmp_buf pointer */
#define K_LOADHDR 0x0A40u     /* LoadExec's header (0x3C bytes) */
#define K_JMPBUF 0x0B00u      /* the default exception jmp_buf (12 words) */
#define K_HINFO 0x0B40u       /* built-in HandlerInfo structs, 16 bytes each */
#define K_HARRAY 0x0C00u      /* handlers array: 4 priorities x 8 bytes */
#define K_PCB 0x0C20u         /* process control block: the current TCB */
#define K_EXC 0x0C80u         /* exception handler code */
#define K_IDLE 0x0D00u        /* b . ; nop: nothing to run, or halted */
#define K_KHEAP 0x5000u       /* kernel heap (B(00h)), 8 KB */
#define K_KHEAP_SIZE 0x2000u
#define K_EXSTACK 0x80008FF0u /* exception stack top (0x7000..0x8FFF) */
#define TCB_SIZE 0xC0u
#define EVCB_SIZE 0x1Cu
#define TCB_MAX 16
#define EVCB_MAX 256
#define NFILES 16

/* Built-in HandlerInfo structs (K_HINFO + 16 * n). */
enum { HI_SYSCALL, HI_IRQ, HI_T0, HI_T1, HI_T2, HI_T3, HI_SIO0, HI_COUNT };

/* The kernel's own trap entry points (table 3). */
enum {
    K_SYSCALL_VERIFIER,
    K_IRQ_VERIFIER,
    K_T0_VERIFIER, K_T1_VERIFIER, K_T2_VERIFIER, K_T3_VERIFIER,
    K_T0_HANDLER, K_T1_HANDLER, K_T2_HANDLER, K_T3_HANDLER,
    K_SIO0_VERIFIER, K_SIO0_HANDLER,
    K_EXC_ENTER, K_EXC_LEAVE,
    K_GUEST_RET,   /* return address of a C -> guest call */
    K_EXEC_RET,    /* return address of a program Exec started */
};

/* Event classes, modes and states. */
#define EV_FREE 0x0000u
#define EV_DISABLED 0x1000u
#define EV_ENABLED 0x2000u
#define EV_PENDING 0x4000u
#define EVM_CALLBACK 0x1000u
#define EVM_NOCALLBACK 0x2000u
#define EVC_VBLANK 0xF0000001u
#define EVC_SW_CARD 0xF4000001u
#define EVC_HW_CARD 0xF0000011u
#define EVS_IOE 0x0004u
#define EVS_ERROR 0x8000u
#define EVS_TIMEOUT 0x0100u
#define EVS_NEWCARD 0x2000u

/* File modes and errno values (psx-spx). */
#define F_READ 0x0001u
#define F_WRITE 0x0002u
#define F_NBLOCK 0x0004u
#define F_APPEND 0x0100u
#define F_CREAT 0x0200u
#define F_TRUNC 0x0400u
#define F_ASYNC 0x8000u
enum { E_NOERR = 0, E_NOENT = 2, E_BADF = 9, E_BUSY = 16, E_EXIST = 17, E_NODEV = 19, E_INVAL = 22, E_MFILE = 24,
       E_NOSPC = 28 };

enum { DEV_NONE, DEV_TTY, DEV_CDROM, DEV_BU };

typedef struct hle_file {
    int dev;        /* DEV_*, DEV_NONE = free */
    int port;       /* memory card slot */
    uint32_t first; /* cdrom: extent LBA; bu: first directory block (0..14) */
    uint32_t size;
    uint32_t pos;
    uint32_t mode;
    int err;
} hle_file;

struct ps1_hle {
    /* Guest calls from C (event callbacks, qsort / bsearch / lsearch comparisons). */
    int depth;
    bool returned;      /* K_GUEST_RET ran at the current depth */
    bool escaped;       /* a call left the C frames for good (ReturnFromException, longjmp, ChangeTh, Exec) */
    bool pc_set;        /* the call set pc itself (else: return to ra) */
    /* IRQ and VBlank handling. */
    bool irq_autoack[11];
    bool timer_autoack[4];
    bool sio0_autoack;
    bool pad_started, card_started;
    uint32_t pad_buf[2], pad_size[2];
    uint32_t pad_hl;    /* B(15h)'s user buffer */
    /* Kernel objects. */
    uint32_t tcb_count, evcb_count;
    uint32_t conf_tcb, conf_ev, conf_stack;
    uint32_t exec_hdr;  /* the header of the program Exec started (its return restores from there) */
    /* Heaps: [start, end) in guest memory, blocks with a header word (size | 1 when used). */
    uint32_t heap_start, heap_end;
    uint32_t kheap_start, kheap_end;
    /* libc state. */
    uint32_t rand_next;
    uint32_t strtok_next;
    /* Files. */
    hle_file files[NFILES];
    int last_err;
    uint32_t cd_dir_lba, cd_dir_size; /* B(40h) cd's current directory (0: root) */
    /* firstfile / nextfile. */
    int find_dev, find_port;
    char find_pat[24];
    uint32_t find_pos;  /* bu: next block; cdrom: byte offset in the directory */
    uint32_t find_dir_lba, find_dir_size;
    bool card_new[2];   /* _card_info reports a new card once */
    uint32_t card_chan; /* the last low-level card port */
    /* Unimplemented calls, reported once each. */
    uint8_t warned[3][32];
    int unimpl;
    bool trace; /* $PS1_HLE_TRACE: every call on stderr */
};

/* ---- Guest memory ---- */

static uint8_t *gptr(ps1_machine *m, uint32_t a)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u)
        return m->ram + (p & (PS1_RAM_SIZE - 1));
    if ((p & 0xFFFFFC00u) == 0x1F800000u)
        return m->scratch + (p & 0x3FFu);
    return NULL;
}

static uint8_t rd8(ps1_machine *m, uint32_t a)
{
    return ps1_peek8(m, a);
}

static void wr8(ps1_machine *m, uint32_t a, uint8_t v)
{
    uint8_t *p = gptr(m, a);
    if (!p)
        return;
    *p = v;
    if (p >= m->ram && p < m->ram + PS1_RAM_SIZE)
        PS1_RAM_TOUCH(m, (uint32_t)(p - m->ram));
}

static uint16_t rd16(ps1_machine *m, uint32_t a)
{
    return (uint16_t)(rd8(m, a) | rd8(m, a + 1) << 8);
}

static uint32_t rd32(ps1_machine *m, uint32_t a)
{
    return (uint32_t)rd16(m, a) | (uint32_t)rd16(m, a + 2) << 16;
}

static void wr16(ps1_machine *m, uint32_t a, uint32_t v)
{
    wr8(m, a, (uint8_t)v);
    wr8(m, a + 1, (uint8_t)(v >> 8));
}

static void wr32(ps1_machine *m, uint32_t a, uint32_t v)
{
    wr16(m, a, v);
    wr16(m, a + 2, v >> 16);
}

/* A NUL-terminated guest string into buf (truncated to n - 1). */
static void gstr(ps1_machine *m, uint32_t a, char *buf, size_t n)
{
    size_t i = 0;
    if (a) {
        for (; i + 1 < n; i++) {
            char c = (char)rd8(m, a + (uint32_t)i);
            if (!c)
                break;
            buf[i] = c;
        }
    }
    buf[i] = 0;
}

static void gread(ps1_machine *m, uint32_t a, void *dst, size_t n)
{
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++)
        d[i] = rd8(m, a + (uint32_t)i);
}

static void gwrite(ps1_machine *m, uint32_t a, const void *src, size_t n)
{
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++)
        wr8(m, a + (uint32_t)i, s[i]);
}

/* ---- Registers, returns ---- */

#define A0 (m->r[4])
#define A1 (m->r[5])
#define A2 (m->r[6])
#define A3 (m->r[7])
#define SP (m->r[29])
#define RA (m->r[31])

/* Argument n of a call (0-based): a0..a3, then the caller's stack (O32: sp + 4n). */
static uint32_t arg(ps1_machine *m, int n)
{
    return n < 4 ? m->r[4 + n] : rd32(m, SP + 4u * (uint32_t)n);
}

static void ret(ps1_machine *m, uint32_t v)
{
    m->r[2] = v;
}

static void set_pc(ps1_machine *m, uint32_t pc)
{
    m->pc = pc;
    m->next_pc = pc + 4;
    m->next_delay = false;
    m->next_taken = false;
    m->hle->pc_set = true;
}

static uint32_t stub_addr(int table, int fn)
{
    return 0x80000000u | (PS1_HLE_STUBS + (uint32_t)(table * 0x100 + fn) * 4u);
}

static uint32_t tcb_cur(ps1_machine *m)
{
    return rd32(m, K_PCB);
}

/* ---- IRQ controller, timers, GPU through the bus (side effects included) ---- */

static uint32_t io_rd(ps1_machine *m, uint32_t phys)
{
    return ps1_io_read(m, phys, 4);
}

static void io_wr(ps1_machine *m, uint32_t phys, uint32_t v)
{
    ps1_io_write(m, phys, v, 4);
}

#define I_STAT 0x1F801070u
#define I_MASK 0x1F801074u

static void irq_ack(ps1_machine *m, uint32_t bits)
{
    io_wr(m, I_STAT, ~bits);
}

/* ---- C -> guest calls ---- */

static void tty_putc(ps1_machine *m, int c)
{
    if (m->tty)
        m->tty(m->tty_ctx, (char)c);
}

typedef struct cpu_save {
    uint32_t r[32], hi, lo, pc, next_pc, ld_reg, ld_val, pend_reg, pend_val, cur_pc;
    bool in_delay, next_delay, branch_taken, next_taken, pc_set;
} cpu_save;

/* Runs guest function fn(a0, a1, a2) to its return in the interpreter (interrupts held back meanwhile, as in the
 * exception handler) and returns v0. The registers are restored after it. When the function escapes instead
 * (ReturnFromException, longjmp...), *escaped is set and the machine is left as the escape put it: the caller must
 * return at once. */
static uint32_t guest_call(ps1_machine *m, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, bool *escaped)
{
    ps1_hle *h = m->hle;
    *escaped = false;
    if (!fn || h->depth >= 8)
        return 0;
    cpu_save s;
    memcpy(s.r, m->r, sizeof s.r);
    s.hi = m->hi;
    s.lo = m->lo;
    s.pc = m->pc;
    s.next_pc = m->next_pc;
    s.ld_reg = m->ld_reg;
    s.ld_val = m->ld_val;
    s.pend_reg = m->pend_reg;
    s.pend_val = m->pend_val;
    s.cur_pc = m->cur_pc;
    s.in_delay = m->in_delay;
    s.next_delay = m->next_delay;
    s.branch_taken = m->branch_taken;
    s.next_taken = m->next_taken;
    s.pc_set = h->pc_set;

    m->r[4] = a0;
    m->r[5] = a1;
    m->r[6] = a2;
    m->r[29] -= 0x20;
    m->r[31] = stub_addr(3, K_GUEST_RET);
    m->ld_reg = m->pend_reg = 0;
    m->pc = fn;
    m->next_pc = fn + 4;
    m->next_delay = m->next_taken = false;
    h->depth++;
    h->returned = false;
    h->escaped = false;
    uint64_t budget = 200000000u; /* instructions: a callback that never returns hangs nothing for good */
    while (!h->returned && !h->escaped && budget--) {
        if (m->cycles >= m->next_event)
            ps1_sched_events(m);
        ps1_cpu_step(m);
    }
    h->depth--;
    if (!h->returned) {
        *escaped = true;
        h->escaped = h->depth > 0; /* the frames above bail out too, the outermost trap keeps the escape's pc */
        h->pc_set = true;
        return 0;
    }
    h->returned = false;
    uint32_t v0 = m->r[2];
    memcpy(m->r, s.r, sizeof s.r);
    m->hi = s.hi;
    m->lo = s.lo;
    m->pc = s.pc;
    m->next_pc = s.next_pc;
    m->ld_reg = s.ld_reg;
    m->ld_val = s.ld_val;
    m->pend_reg = s.pend_reg;
    m->pend_val = s.pend_val;
    m->cur_pc = s.cur_pc;
    m->in_delay = s.in_delay;
    m->next_delay = s.next_delay;
    m->branch_taken = s.branch_taken;
    m->next_taken = s.next_taken;
    h->pc_set = s.pc_set;
    return v0;
}

/* A call that leaves the C frames (it set pc and the registers for good). */
static void escape(ps1_machine *m)
{
    m->hle->pc_set = true;
    if (m->hle->depth > 0)
        m->hle->escaped = true;
}

/* ---- Events (B(07h)..B(0Dh), B(20h)) ---- */

static uint32_t evcb(uint32_t id)
{
    return PS1_HLE_EVCB + (id & 0xFFFFu) * EVCB_SIZE;
}

static bool ev_valid(ps1_machine *m, uint32_t id)
{
    return (id & 0xFFFFu) < m->hle->evcb_count;
}

/* Returns false when a callback escaped. */
static bool deliver_event(ps1_machine *m, uint32_t cls, uint32_t spec)
{
    ps1_hle *h = m->hle;
    for (uint32_t i = 0; i < h->evcb_count; i++) {
        uint32_t e = PS1_HLE_EVCB + i * EVCB_SIZE;
        if (rd32(m, e + 4) != EV_ENABLED || rd32(m, e) != cls || rd32(m, e + 8) != spec)
            continue;
        uint32_t mode = rd32(m, e + 12);
        if (mode == EVM_NOCALLBACK) {
            wr32(m, e + 4, EV_PENDING);
        } else if (mode == EVM_CALLBACK && rd32(m, e + 16)) {
            bool esc;
            guest_call(m, rd32(m, e + 16), 0, 0, 0, &esc);
            if (esc)
                return false;
        }
    }
    return true;
}

static void undeliver_event(ps1_machine *m, uint32_t cls, uint32_t spec)
{
    for (uint32_t i = 0; i < m->hle->evcb_count; i++) {
        uint32_t e = PS1_HLE_EVCB + i * EVCB_SIZE;
        if (rd32(m, e + 4) == EV_PENDING && rd32(m, e) == cls && rd32(m, e + 8) == spec &&
            rd32(m, e + 12) == EVM_NOCALLBACK)
            wr32(m, e + 4, EV_ENABLED);
    }
}

static void init_events(ps1_machine *m, uint32_t count)
{
    if (count == 0)
        count = 16;
    if (count > EVCB_MAX)
        count = EVCB_MAX;
    m->hle->evcb_count = count;
    for (uint32_t i = 0; i < count * EVCB_SIZE; i += 4)
        wr32(m, PS1_HLE_EVCB + i, 0);
    wr32(m, K_TOT + 0x20, 0x80000000u | PS1_HLE_EVCB);
    wr32(m, K_TOT + 0x24, count * EVCB_SIZE);
}

static void init_threads(ps1_machine *m, uint32_t count)
{
    if (count == 0)
        count = 4;
    if (count > TCB_MAX)
        count = TCB_MAX;
    m->hle->tcb_count = count;
    for (uint32_t i = 0; i < count * TCB_SIZE; i += 4)
        wr32(m, PS1_HLE_TCB + i, 0);
    for (uint32_t i = 0; i < count; i++)
        wr32(m, PS1_HLE_TCB + i * TCB_SIZE, 0x1000u);
    wr32(m, PS1_HLE_TCB, 0x4000u);
    wr32(m, K_PCB, 0x80000000u | PS1_HLE_TCB);
    wr32(m, K_TOT + 0x08, 0x80000000u | K_PCB);
    wr32(m, K_TOT + 0x0C, 4);
    wr32(m, K_TOT + 0x10, 0x80000000u | PS1_HLE_TCB);
    wr32(m, K_TOT + 0x14, count * TCB_SIZE);
}

/* ---- Handler chains (C(02h) / C(03h)) ---- */

static uint32_t deq_int(ps1_machine *m, uint32_t prio, uint32_t info);

static void enq_int(ps1_machine *m, uint32_t prio, uint32_t info)
{
    /* a node already in the chain is moved to its front (enqueued twice, it would loop the chain forever) */
    deq_int(m, prio, info);
    uint32_t arr = rd32(m, K_TOT) + (prio & 3) * 8;
    wr32(m, info, rd32(m, arr));
    wr32(m, arr, info);
}

static uint32_t deq_int(ps1_machine *m, uint32_t prio, uint32_t info)
{
    uint32_t arr = rd32(m, K_TOT) + (prio & 3) * 8;
    uint32_t p = rd32(m, arr), prev = 0;
    for (int guard = 0; p && guard < 4096; guard++) {
        if ((p & 0x1FFFFFFFu) == (info & 0x1FFFFFFFu)) {
            if (prev)
                wr32(m, prev, rd32(m, p));
            else
                wr32(m, arr, rd32(m, p));
            return p;
        }
        prev = p;
        p = rd32(m, p);
    }
    return 0;
}

static uint32_t hinfo(int n)
{
    return 0x80000000u | (K_HINFO + 16u * (uint32_t)n);
}

/* ---- Exceptions ---- */

/* The registers into the current TCB (the handler's first trap; k0 was used by the vector). */
static void exc_enter(ps1_machine *m)
{
    uint32_t t = tcb_cur(m) + 8;
    for (int i = 1; i < 32; i++)
        if (i != 26)
            wr32(m, t + 4u * (uint32_t)i, m->r[i]);
    uint32_t cause = m->cop0[C0_CAUSE], epc = m->cop0[C0_EPC];
    /* An interrupt taken on a GTE command: it already ran (ps1_cpu.c), resume after it. */
    if ((cause & 0x3Cu) == 0 && ((rd32(m, epc) >> 24) & 0xFEu) == 0x4Au)
        epc += 4;
    wr32(m, t + 0x80, epc);
    wr32(m, t + 0x84, m->hi);
    wr32(m, t + 0x88, m->lo);
    wr32(m, t + 0x8C, m->cop0[C0_SR]);
    wr32(m, t + 0x90, cause);
    m->r[29] = K_EXSTACK;
    m->r[30] = K_EXSTACK;
    m->r[28] = 0;
    m->r[19] = rd32(m, K_TOT);                  /* s3: the handlers array */
    m->r[20] = m->r[19] + rd32(m, K_TOT + 4);   /* s4: its end */
    set_pc(m, m->pc);                           /* on to the chain walk */
}

/* B(17h): the registers back from the current TCB, RFE, jump to its EPC. */
static void return_from_exception(ps1_machine *m)
{
    uint32_t t = tcb_cur(m) + 8;
    for (int i = 1; i < 32; i++)
        if (i != 26)
            m->r[i] = rd32(m, t + 4u * (uint32_t)i);
    m->hi = rd32(m, t + 0x84);
    m->lo = rd32(m, t + 0x88);
    uint32_t sr = rd32(m, t + 0x8C);
    m->cop0[C0_SR] = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
    m->ld_reg = m->pend_reg = 0;
    set_pc(m, rd32(m, t + 0x80));
    ps1_cpu_update_irq(m);
    escape(m);
}

/* longjmp(buf, v): ra, sp, fp, s0..s7, gp. */
static void do_longjmp(ps1_machine *m, uint32_t buf, uint32_t v)
{
    m->r[31] = rd32(m, buf);
    m->r[29] = rd32(m, buf + 4);
    m->r[30] = rd32(m, buf + 8);
    for (int i = 0; i < 8; i++)
        m->r[16 + i] = rd32(m, buf + 12 + 4u * (uint32_t)i);
    m->r[28] = rd32(m, buf + 44);
    m->r[2] = v;
    m->ld_reg = m->pend_reg = 0;
    set_pc(m, m->r[31]);
    escape(m);
}

static void do_setjmp(ps1_machine *m, uint32_t buf)
{
    wr32(m, buf, RA);
    wr32(m, buf + 4, SP);
    wr32(m, buf + 8, m->r[30]);
    for (int i = 0; i < 8; i++)
        wr32(m, buf + 12 + 4u * (uint32_t)i, m->r[16 + i]);
    wr32(m, buf + 44, m->r[28]);
}

static void halt(ps1_machine *m, const char *why)
{
    fprintf(stderr, "ps1: HLE BIOS: %s (pc %08X, epc %08X, cause %08X); halted\n", why, m->r[31], m->cop0[C0_EPC],
            m->cop0[C0_CAUSE]);
    m->cop0[C0_SR] &= ~SR_IEC;
    ps1_cpu_update_irq(m);
    set_pc(m, 0x80000000u | K_IDLE);
    escape(m);
}

static void syscall_verifier(ps1_machine *m)
{
    uint32_t t = tcb_cur(m) + 8;
    uint32_t code = rd32(m, t + 0x90) & 0x3Cu;
    if (code == 0x00) { /* an interrupt: not ours */
        ret(m, 0);
        return;
    }
    if (code == 0x20) { /* syscall */
        wr32(m, t + 0x80, rd32(m, t + 0x80) + 4);
        uint32_t sr = rd32(m, t + 0x8C);
        switch (rd32(m, t + 0x10)) { /* a0 */
        case 0:
            break;
        case 1: /* EnterCriticalSection */
            wr32(m, t + 0x08, (sr & 0x404u) == 0x404u);
            wr32(m, t + 0x8C, sr & ~0x404u);
            break;
        case 2: /* ExitCriticalSection */
            wr32(m, t + 0x8C, sr | 0x404u);
            break;
        case 3: /* ChangeThreadSubFunction */
            wr32(m, K_PCB, rd32(m, t + 0x14));
            wr32(m, t + 0x08, 1);
            break;
        default:
            if (!deliver_event(m, 0xF0000010u, 0x4000u))
                return;
            break;
        }
        return_from_exception(m);
        return;
    }
    if (!deliver_event(m, 0xF0000010u, 0x1000u))
        return;
    /* A(40h) SystemErrorUnresolvedException through the RAM table, as OpenBIOS calls it: programs hook it there
     * (ps1-tests' exception helper); its return value is the verifier's */
    uint32_t entry = rd32(m, PS1_HLE_ATAB + 0x40 * 4);
    if (entry != stub_addr(0, 0x40)) {
        set_pc(m, entry); /* a tail call: it returns to the chain walk */
        return;
    }
    halt(m, "unresolved exception");
}

static const uint32_t s_irq_event[11] = {
    0xF0000001u, 0xF0000002u, 0xF0000003u, 0xF0000004u, 0xF0000005u, 0xF0000006u,
    0xF0000006u, /* timer 2: the retail BIOS's copy-paste, kept by OpenBIOS */
    0xF0000008u, 0xF000000Bu, 0xF0000009u, 0xF000000Au,
};

/* The IRQ verifier (priority 3): an event per pending IRQ, acknowledged when its auto-ack is on. Order as OpenBIOS. */
static void irq_verifier(ps1_machine *m)
{
    static const int order[11] = {2, 9, 1, 10, 8, 0, 4, 5, 6, 7, 3};
    for (int k = 0; k < 11; k++) {
        int irq = order[k];
        if (io_rd(m, I_MASK) & io_rd(m, I_STAT) & (1u << irq)) {
            if (!deliver_event(m, s_irq_event[irq], 0x1000u))
                return;
            if (m->hle->irq_autoack[irq])
                irq_ack(m, 1u << irq);
        }
    }
    ret(m, 0);
}

static void timer_verifier(ps1_machine *m, int t)
{
    uint32_t bit = t == 3 ? 1u : 1u << (4 + t);
    if (!(io_rd(m, I_MASK) & bit) || !(io_rd(m, I_STAT) & bit)) {
        ret(m, 0);
        return;
    }
    if (!deliver_event(m, 0xF2000000u | (uint32_t)t, 2))
        return;
    ret(m, 1);
}

static void timer_handler(ps1_machine *m, int t)
{
    if (!m->hle->timer_autoack[t])
        return;
    irq_ack(m, t == 3 ? 1u : 1u << (4 + t));
    return_from_exception(m);
}

/* ---- Pads: the BIOS's VBlank pad / card handler (priority 2), straight from the pad state ---- */

static void pad_read(ps1_machine *m, int port)
{
    uint32_t b = m->hle->pad_buf[port];
    if (!b)
        return;
    const ps1_pad *p = &m->pads[port];
    if (p->kind == PS1_PAD_NONE) {
        wr8(m, b, 0xFF);
        return;
    }
    bool analog = (p->kind == PS1_PAD_ANALOG || p->kind == PS1_PAD_DUALSHOCK) && p->analog;
    uint16_t btn = (uint16_t)~p->buttons;
    if (!analog)
        btn |= PS1_PAD_L3 | PS1_PAD_R3;
    uint8_t data[8] = {0x00, analog ? 0x73 : 0x41, (uint8_t)btn, (uint8_t)(btn >> 8), p->rx, p->ry, p->lx, p->ly};
    uint32_t n = analog ? 8 : 4;
    if (m->hle->pad_size[port] && n > m->hle->pad_size[port])
        n = m->hle->pad_size[port];
    gwrite(m, b, data, n);
}

static void sio0_verifier(ps1_machine *m)
{
    ret(m, (io_rd(m, I_MASK) & 1u) && (io_rd(m, I_STAT) & 1u));
}

static void sio0_handler(ps1_machine *m)
{
    ps1_hle *h = m->hle;
    if (h->pad_started) {
        pad_read(m, 0);
        pad_read(m, 1);
        if (h->pad_hl) {
            uint32_t v = 0xFFFFFFFFu;
            for (int port = 0; port < 2; port++) {
                uint32_t b = h->pad_buf[port];
                if (b && rd8(m, b) == 0 && rd8(m, b + 1) == 0x41)
                    v = (v & ~(0xFFFFu << (16 * port))) | (uint32_t)(rd8(m, b + 2) << 8 | rd8(m, b + 3))
                                                              << (16 * port);
            }
            wr32(m, h->pad_hl, v);
        }
    }
    if (h->sio0_autoack)
        irq_ack(m, 1u);
}

static void sio0_install(ps1_machine *m)
{
    deq_int(m, 2, hinfo(HI_SIO0));
    enq_int(m, 2, hinfo(HI_SIO0));
    io_wr(m, I_MASK, io_rd(m, I_MASK) | 1u);
    m->hle->sio0_autoack = true;
    m->hle->timer_autoack[3] = false;
}

/* ---- Heaps (A(33h)..A(39h), B(00h) / B(01h)) ---- */

static void heap_init(ps1_machine *m, uint32_t *start, uint32_t *end, uint32_t addr, uint32_t size)
{
    addr = (addr + 3) & ~3u;
    size &= ~3u;
    *start = addr;
    *end = addr + size;
    if (size >= 8)
        wr32(m, addr, size); /* one free block: header = size (including the header), bit 0 = used */
}

static uint32_t heap_alloc(ps1_machine *m, uint32_t start, uint32_t end, uint32_t n)
{
    if (start == end)
        return 0;
    n = ((n + 3) & ~3u) + 4;
    for (uint32_t p = start; p + 4 <= end;) {
        uint32_t hdr = rd32(m, p), sz = hdr & ~3u;
        if (sz < 4 || p + sz > end)
            return 0; /* a corrupt heap */
        if (!(hdr & 1)) {
            /* merge the free blocks after this one */
            for (uint32_t q = p + sz; q + 4 <= end;) {
                uint32_t qh = rd32(m, q), qs = qh & ~3u;
                if ((qh & 1) || qs < 4 || q + qs > end)
                    break;
                sz += qs;
                q += qs;
            }
            wr32(m, p, sz);
            if (sz >= n) {
                if (sz - n >= 8) {
                    wr32(m, p + n, sz - n);
                    sz = n;
                }
                wr32(m, p, sz | 1);
                return p + 4;
            }
        }
        p += sz;
    }
    return 0;
}

static void heap_free(ps1_machine *m, uint32_t start, uint32_t end, uint32_t ptr)
{
    if (!ptr || ptr < start + 4 || ptr >= end)
        return;
    uint32_t p = ptr - 4;
    wr32(m, p, rd32(m, p) & ~1u);
}

/* ---- printf (A(3Fh)) ---- */

static void out_str(ps1_machine *m, const char *s, int n)
{
    for (int i = 0; i < n; i++)
        tty_putc(m, s[i]);
}

static void hle_printf(ps1_machine *m, uint32_t fmt, int first_arg)
{
    int ai = first_arg;
    for (uint32_t p = fmt;; p++) {
        char c = (char)rd8(m, p);
        if (!c)
            break;
        if (c != '%') {
            tty_putc(m, c);
            continue;
        }
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;;) {
            c = (char)rd8(m, ++p);
            if (c == '-') left = true;
            else if (c == '0') zero = true;
            else if (c == '+') plus = true;
            else if (c == ' ') space = true;
            else if (c == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (c == '*') {
            width = (int)arg(m, ai++);
            c = (char)rd8(m, ++p);
        } else {
            /* the BIOS's printf takes a flag after the width too ("%2-d": ps1-tests cpu/access-time, whose
             * hardware log shows it left-justified) */
            while ((c >= '0' && c <= '9') || (width && c == '-')) {
                if (c == '-')
                    left = true;
                else
                    width = width * 10 + (c - '0');
                c = (char)rd8(m, ++p);
            }
        }
        if (c == '.') {
            prec = 0;
            c = (char)rd8(m, ++p);
            if (c == '*') {
                prec = (int)arg(m, ai++);
                c = (char)rd8(m, ++p);
            } else {
                while (c >= '0' && c <= '9') {
                    prec = prec * 10 + (c - '0');
                    c = (char)rd8(m, ++p);
                }
            }
        }
        while (c == 'l' || c == 'h' || c == 'L')
            c = (char)rd8(m, ++p);
        if (!c)
            break;
        char buf[64], body[48];
        int n = 0;
        const char *pre = "";
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': case 'b': {
            uint32_t v = arg(m, ai++);
            bool neg = false;
            if ((c == 'd' || c == 'i') && (int32_t)v < 0) {
                neg = true;
                v = (uint32_t)-(int32_t)v;
            }
            unsigned base = (c == 'x' || c == 'X' || c == 'p') ? 16 : c == 'o' ? 8 : c == 'b' ? 2 : 10;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            int k = 0;
            char tmp[40];
            do {
                tmp[k++] = dig[v % base];
                v /= base;
            } while (v);
            if (c == 'p' && prec < 0)
                prec = 8;
            while (k < prec && k < 32)
                tmp[k++] = '0';
            for (int i = 0; i < k; i++)
                body[i] = tmp[k - 1 - i];
            n = k;
            pre = neg ? "-" : plus && (c == 'd' || c == 'i') ? "+" : space && (c == 'd' || c == 'i') ? " " : "";
            if (alt && (c == 'x' || c == 'X'))
                pre = c == 'x' ? "0x" : "0X";
            if (prec >= 0)
                zero = false;
            break;
        }
        case 'c':
            body[0] = (char)arg(m, ai++);
            n = 1;
            break;
        case 's': {
            uint32_t s = arg(m, ai++);
            int len = 0;
            const char *nul = "(null)";
            if (!s) {
                while (nul[len] && (prec < 0 || len < prec))
                    len++;
            } else {
                while (rd8(m, s + (uint32_t)len) && (prec < 0 || len < prec) && len < 1 << 16)
                    len++;
            }
            int padn = width > len ? width - len : 0;
            if (!left)
                for (int i = 0; i < padn; i++)
                    tty_putc(m, ' ');
            for (int i = 0; i < len; i++)
                tty_putc(m, s ? rd8(m, s + (uint32_t)i) : nul[i]);
            if (left)
                for (int i = 0; i < padn; i++)
                    tty_putc(m, ' ');
            continue;
        }
        case '%':
            tty_putc(m, '%');
            continue;
        default: /* unknown: printed as is */
            tty_putc(m, '%');
            tty_putc(m, c);
            continue;
        }
        int pl = (int)strlen(pre), total = pl + n, padn = width > total ? width - total : 0, k = 0;
        if (!left && !zero)
            for (int i = 0; i < padn && k < 60; i++)
                buf[k++] = ' ';
        for (int i = 0; i < pl; i++)
            buf[k++] = pre[i];
        if (!left && zero)
            for (int i = 0; i < padn && k < 60; i++)
                buf[k++] = '0';
        out_str(m, buf, k);
        out_str(m, body, n);
        if (left)
            for (int i = 0; i < padn; i++)
                tty_putc(m, ' ');
    }
}

/* ---- ISO 9660 on the disc image (cdrom:) ---- */

static int cd_sector(ps1_machine *m, uint32_t lba, uint8_t out[2048])
{
    uint8_t raw[2352];
    if (!m->disc || ps1_disc_read_sector(m->disc, lba, raw) != 0)
        return -1;
    memcpy(out, raw[15] == 2 ? raw + 24 : raw + 16, 2048);
    return 0;
}

static int cd_root(ps1_machine *m, uint32_t *lba, uint32_t *size)
{
    uint8_t pvd[2048];
    if (cd_sector(m, 16, pvd) != 0 || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0)
        return -1;
    *lba = (uint32_t)pvd[158] | (uint32_t)pvd[159] << 8 | (uint32_t)pvd[160] << 16 | (uint32_t)pvd[161] << 24;
    *size = (uint32_t)pvd[166] | (uint32_t)pvd[167] << 8 | (uint32_t)pvd[168] << 16 | (uint32_t)pvd[169] << 24;
    return 0;
}

typedef struct cd_ent {
    char name[40]; /* upper case, ";1" kept off */
    uint32_t lba, size;
    bool dir;
} cd_ent;

/* The directory record at byte *pos of directory (lba, size); advances *pos. 1 = an entry, 0 = the end. */
static int cd_dir_next(ps1_machine *m, uint32_t lba, uint32_t size, uint32_t *pos, cd_ent *e)
{
    uint8_t sec[2048];
    while (*pos < size) {
        uint32_t s = *pos / 2048, off = *pos % 2048;
        if (cd_sector(m, lba + s, sec) != 0)
            return 0;
        uint32_t n = sec[off];
        if (n == 0) {
            *pos = (s + 1) * 2048;
            continue;
        }
        if (n < 33 || off + n > 2048)
            return 0;
        *pos += n;
        uint32_t nl = sec[off + 32];
        if (nl == 1 && (sec[off + 33] == 0 || sec[off + 33] == 1))
            continue; /* "." and ".." */
        size_t k = 0;
        for (uint32_t i = 0; i < nl && k + 1 < sizeof e->name; i++) {
            char c = (char)sec[off + 33 + i];
            if (c == ';')
                break;
            e->name[k++] = (char)toupper((unsigned char)c);
        }
        e->name[k] = 0;
        const uint8_t *r = sec + off;
        e->lba = (uint32_t)r[2] | (uint32_t)r[3] << 8 | (uint32_t)r[4] << 16 | (uint32_t)r[5] << 24;
        e->size = (uint32_t)r[10] | (uint32_t)r[11] << 8 | (uint32_t)r[12] << 16 | (uint32_t)r[13] << 24;
        e->dir = (r[25] & 2) != 0;
        return 1;
    }
    return 0;
}

/* A path ("\\DIR\\FILE.EXT;1", '/' too, case-insensitive) from the current directory (or the root when it starts
 * with a separator): its extent and size. 0, or -1. */
static int cd_find(ps1_machine *m, const char *path, uint32_t *lba, uint32_t *size, bool *is_dir)
{
    uint32_t dl, ds;
    if (cd_root(m, &dl, &ds) != 0)
        return -1;
    if (path[0] != '\\' && path[0] != '/' && m->hle->cd_dir_lba) {
        dl = m->hle->cd_dir_lba;
        ds = m->hle->cd_dir_size;
    }
    bool dir = true;
    const char *p = path;
    for (;;) {
        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            break;
        char part[40];
        size_t k = 0;
        while (*p && *p != '\\' && *p != '/') {
            if (*p == ';') {
                while (*p && *p != '\\' && *p != '/')
                    p++;
                break;
            }
            if (k + 1 < sizeof part)
                part[k++] = (char)toupper((unsigned char)*p);
            p++;
        }
        part[k] = 0;
        uint32_t pos = 0;
        cd_ent e;
        bool found = false;
        while (cd_dir_next(m, dl, ds, &pos, &e)) {
            if (!strcmp(e.name, part)) {
                found = true;
                break;
            }
        }
        if (!found)
            return -1;
        dl = e.lba;
        ds = e.size;
        dir = e.dir;
    }
    *lba = dl;
    *size = ds;
    if (is_dir)
        *is_dir = dir;
    return 0;
}

/* ---- Memory cards (bu00: / bu10:): the card's own file system, on ps1_card ---- */

static ps1_card *card_of(ps1_machine *m, int port)
{
    ps1_card *c = &m->cards[port & 1];
    return c->inserted ? c : NULL;
}

static uint8_t *card_frame(ps1_card *c, int f)
{
    return c->data + f * 128;
}

static void card_fix_chk(uint8_t *fr)
{
    uint8_t x = 0;
    for (int i = 0; i < 0x7F; i++)
        x ^= fr[i];
    fr[0x7F] = x;
}

static bool card_formatted(ps1_card *c)
{
    return c->data[0] == 'M' && c->data[1] == 'C';
}

static void card_dirty(ps1_card *c)
{
    c->dirty = true;
}

/* The directory entry of block b (0..14). */
static uint8_t *card_dir(ps1_card *c, int b)
{
    return card_frame(c, 1 + b);
}

static bool name_match(const char *name, const char *pat)
{
    /* OpenBIOS's patternMatch: '?' any character, same length unless the pattern ends in '?'s. */
    const char *n = name, *p = pat;
    while (*n) {
        if (*p != '?' && *p != *n)
            return false;
        if (*p)
            p++;
        n++;
    }
    while (*p == '?')
        p++;
    return *p == 0;
}

/* A pattern for name_match from a firstfile argument: '*' fills the rest with '?' (20 characters). */
static void make_pattern(const char *in, char out[24])
{
    size_t k = 0;
    for (const char *p = in; *p && k < 20; p++) {
        if (*p == '*') {
            while (k < 20)
                out[k++] = '?';
            break;
        }
        out[k++] = *p;
    }
    out[k] = 0;
}

static int card_find(ps1_card *c, const char *pat, int from)
{
    for (int b = from; b < 15; b++) {
        uint8_t *e = card_dir(c, b);
        if (e[0] != 0x51)
            continue;
        char name[21];
        memcpy(name, e + 10, 20);
        name[20] = 0;
        if (name[0] && name_match(name, pat))
            return b;
    }
    return -1;
}

/* The card's absolute frame of byte offset `off` in the file starting at block `first`, or -1. */
static int card_file_frame(ps1_card *c, int first, uint32_t off)
{
    int b = first;
    uint32_t sector = off >> 7;
    while (sector >= 64) {
        uint8_t *e = card_dir(c, b);
        int nx = e[8] | e[9] << 8;
        if (nx == 0xFFFF || nx > 14)
            return -1;
        b = nx;
        sector -= 64;
    }
    return (b + 1) * 64 + (int)sector;
}

static int card_create(ps1_card *c, const char *name, uint32_t blocks, int *first_out)
{
    if (blocks == 0)
        blocks = 1;
    int free_n = 0;
    for (int b = 0; b < 15; b++)
        if ((card_dir(c, b)[0] & 0xF0) == 0xA0)
            free_n++;
    if ((int)blocks > free_n)
        return E_NOSPC;
    int prev = -1, first = -1;
    uint32_t done = 0;
    for (int b = 0; b < 15 && done < blocks; b++) {
        uint8_t *e = card_dir(c, b);
        if ((e[0] & 0xF0) != 0xA0)
            continue;
        memset(e, 0, 128);
        if (first < 0) {
            first = b;
            e[0] = 0x51;
            uint32_t sz = blocks * 8192u;
            e[4] = (uint8_t)sz;
            e[5] = (uint8_t)(sz >> 8);
            e[6] = (uint8_t)(sz >> 16);
            e[7] = (uint8_t)(sz >> 24);
            memcpy(e + 10, name, strnlen(name, 20));
        } else {
            e[0] = done + 1 == blocks ? 0x53 : 0x52;
        }
        if (prev >= 0) {
            uint8_t *pe = card_dir(c, prev);
            pe[8] = (uint8_t)b;
            pe[9] = 0;
            card_fix_chk(pe);
        }
        e[8] = e[9] = 0xFF;
        card_fix_chk(e);
        prev = b;
        done++;
    }
    card_dirty(c);
    *first_out = first;
    return E_NOERR;
}

static void card_erase_file(ps1_card *c, int first)
{
    int b = first;
    for (int guard = 0; guard < 15 && b >= 0 && b < 15; guard++) {
        uint8_t *e = card_dir(c, b);
        int nx = e[8] | e[9] << 8;
        e[0] = (uint8_t)((e[0] & 0x0F) | 0xA0);
        card_fix_chk(e);
        b = nx == 0xFFFF ? -1 : nx;
    }
    card_dirty(c);
}

/* ---- Files (A(00h)..A(09h), B(32h)..B(3Fh), B(40h)..B(46h)) ---- */

/* Splits "dev:path": device kind and port, the path after ':'. */
static int parse_dev(const char *full, int *port, const char **path)
{
    const char *colon = strchr(full, ':');
    *port = 0;
    if (!colon) {
        *path = full;
        return DEV_NONE;
    }
    *path = colon + 1;
    size_t n = (size_t)(colon - full);
    if (n >= 5 && !strncasecmp(full, "cdrom", 5))
        return DEV_CDROM;
    if (n >= 2 && !strncasecmp(full, "bu", 2)) {
        /* bu00 / bu10 (port 1 = the second slot) */
        *port = n >= 3 && full[2] == '1' ? 1 : 0;
        return DEV_BU;
    }
    if (n >= 3 && !strncasecmp(full, "tty", 3))
        return DEV_TTY;
    return DEV_NONE;
}

static hle_file *fd_file(ps1_machine *m, uint32_t fd)
{
    if (fd >= NFILES || m->hle->files[fd].dev == DEV_NONE)
        return NULL;
    return &m->hle->files[fd];
}

static int32_t f_open(ps1_machine *m, uint32_t name_a, uint32_t mode)
{
    ps1_hle *h = m->hle;
    char full[128];
    gstr(m, name_a, full, sizeof full);
    int port;
    const char *path;
    int dev = parse_dev(full, &port, &path);
    int fd = -1;
    for (int i = 2; i < NFILES; i++) /* 0 and 1: stdin / stdout */
        if (h->files[i].dev == DEV_NONE) {
            fd = i;
            break;
        }
    if (fd < 0) {
        h->last_err = E_MFILE;
        return -1;
    }
    hle_file *f = &h->files[fd];
    memset(f, 0, sizeof *f);
    f->mode = mode;
    f->port = port;
    if (dev == DEV_CDROM) {
        bool dir;
        if (cd_find(m, path, &f->first, &f->size, &dir) != 0 || dir) {
            h->last_err = E_NOENT;
            return -1;
        }
    } else if (dev == DEV_BU) {
        ps1_card *c = card_of(m, port);
        if (!c || !card_formatted(c)) {
            h->last_err = E_NODEV;
            return -1;
        }
        char name[24];
        snprintf(name, sizeof name, "%.20s", path);
        int b = card_find(c, name, 0);
        if (mode & F_CREAT) {
            if (b >= 0) {
                h->last_err = E_EXIST;
                return -1;
            }
            int e = card_create(c, name, (mode >> 16) & 0xFFFFu, &b);
            if (e) {
                h->last_err = e;
                return -1;
            }
        } else if (b < 0) {
            h->last_err = E_NOENT;
            return -1;
        }
        f->first = (uint32_t)b;
        uint8_t *e = card_dir(c, b);
        f->size = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
    } else if (dev == DEV_TTY) {
        f->size = 0;
    } else {
        h->last_err = E_NODEV;
        return -1;
    }
    f->dev = dev;
    f->err = E_NOERR;
    h->last_err = E_NOERR;
    return fd;
}

static void card_events_done(ps1_machine *m, bool ok)
{
    /* the BIOS card driver's completion events (async callers wait on them; sync callers ignore them) */
    deliver_event(m, EVC_SW_CARD, ok ? EVS_IOE : EVS_ERROR);
    deliver_event(m, EVC_HW_CARD, ok ? EVS_IOE : EVS_ERROR);
}

static int32_t f_rw(ps1_machine *m, uint32_t fd, uint32_t buf, uint32_t len, bool wr)
{
    ps1_hle *h = m->hle;
    if (fd <= 1 && wr) { /* stdout */
        for (uint32_t i = 0; i < len; i++)
            tty_putc(m, rd8(m, buf + i));
        return (int32_t)len;
    }
    hle_file *f = fd_file(m, fd);
    if (!f) {
        h->last_err = E_BADF;
        return -1;
    }
    if (f->dev == DEV_TTY) {
        if (wr)
            for (uint32_t i = 0; i < len; i++)
                tty_putc(m, rd8(m, buf + i));
        return wr ? (int32_t)len : 0;
    }
    if (f->dev == DEV_CDROM) {
        if (wr) {
            f->err = E_INVAL;
            return -1;
        }
        if (f->pos >= f->size)
            return 0;
        if (len > f->size - f->pos)
            len = f->size - f->pos;
        uint8_t sec[2048];
        uint32_t done = 0;
        while (done < len) {
            uint32_t off = f->pos % 2048, n = 2048 - off;
            if (n > len - done)
                n = len - done;
            if (cd_sector(m, f->first + f->pos / 2048, sec) != 0)
                break;
            gwrite(m, buf + done, sec + off, n);
            done += n;
            f->pos += n;
        }
        return (int32_t)done;
    }
    /* bu: whole 128-byte frames from a frame boundary */
    ps1_card *c = card_of(m, f->port);
    if (!c || (f->pos & 0x7F) || f->pos >= f->size) {
        f->err = E_INVAL;
        if (f->mode & F_ASYNC)
            card_events_done(m, false);
        return -1;
    }
    uint32_t frames = len >> 7, done = 0;
    for (uint32_t i = 0; i < frames && f->pos < f->size; i++) {
        int fr = card_file_frame(c, (int)f->first, f->pos);
        if (fr < 0)
            break;
        if (wr)
            gread(m, buf + done, card_frame(c, fr), 128);
        else
            gwrite(m, buf + done, card_frame(c, fr), 128);
        done += 128;
        f->pos += 128;
    }
    if (wr && done)
        card_dirty(c);
    f->err = E_NOERR;
    if (f->mode & F_ASYNC) {
        card_events_done(m, true);
        return 0;
    }
    return (int32_t)done;
}

static int32_t f_lseek(ps1_machine *m, uint32_t fd, int32_t off, uint32_t whence)
{
    hle_file *f = fd_file(m, fd);
    if (!f) {
        m->hle->last_err = E_BADF;
        return -1;
    }
    int64_t np = whence == 0 ? off : whence == 1 ? (int64_t)f->pos + off : (int64_t)f->size + off;
    if (np < 0) {
        f->err = E_INVAL;
        return -1;
    }
    f->pos = (uint32_t)np;
    return (int32_t)f->pos;
}

static int32_t f_close(ps1_machine *m, uint32_t fd)
{
    hle_file *f = fd_file(m, fd);
    if (!f) {
        m->hle->last_err = E_BADF;
        return -1;
    }
    f->dev = DEV_NONE;
    return (int32_t)fd;
}

/* DIRENTRY: name[20], attr, size, next, head (first block / LBA), system[4]. */
static void put_dirent(ps1_machine *m, uint32_t d, const char *name, uint32_t attr, uint32_t size, uint32_t head)
{
    char n20[20] = {0};
    strncpy(n20, name, 19);
    gwrite(m, d, n20, 20);
    wr32(m, d + 20, attr);
    wr32(m, d + 24, size);
    wr32(m, d + 28, 0);
    wr32(m, d + 32, head);
    wr32(m, d + 36, 0);
}

static uint32_t find_next(ps1_machine *m, uint32_t d)
{
    ps1_hle *h = m->hle;
    if (h->find_dev == DEV_BU) {
        ps1_card *c = card_of(m, h->find_port);
        if (!c || !card_formatted(c))
            return 0;
        int b = card_find(c, h->find_pat, (int)h->find_pos);
        if (b < 0)
            return 0;
        h->find_pos = (uint32_t)b + 1;
        uint8_t *e = card_dir(c, b);
        char name[21];
        memcpy(name, e + 10, 20);
        name[20] = 0;
        put_dirent(m, d, name, e[0], (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 |
                                     (uint32_t)e[7] << 24, (uint32_t)b);
        return d;
    }
    if (h->find_dev == DEV_CDROM) {
        cd_ent e;
        while (cd_dir_next(m, h->find_dir_lba, h->find_dir_size, &h->find_pos, &e)) {
            if (!name_match(e.name, h->find_pat))
                continue;
            put_dirent(m, d, e.name, e.dir ? 0x10 : 0, e.size, e.lba);
            return d;
        }
    }
    return 0;
}

static uint32_t find_first(ps1_machine *m, uint32_t name_a, uint32_t d)
{
    ps1_hle *h = m->hle;
    char full[128];
    gstr(m, name_a, full, sizeof full);
    const char *path;
    h->find_dev = parse_dev(full, &h->find_port, &path);
    h->find_pos = 0;
    if (h->find_dev == DEV_CDROM) {
        /* a directory part, then the pattern */
        char dir[128];
        snprintf(dir, sizeof dir, "%s", path);
        char *sep = strrchr(dir, '\\');
        char *sep2 = strrchr(dir, '/');
        if (sep2 > sep)
            sep = sep2;
        const char *pat = path;
        if (sep) {
            pat = path + (sep - dir) + 1;
            sep[1] = 0;
        } else {
            dir[0] = 0;
        }
        bool isdir;
        if (cd_find(m, dir[0] ? dir : "", &h->find_dir_lba, &h->find_dir_size, &isdir) != 0 || !isdir)
            return 0;
        char up[24];
        size_t k = 0;
        for (; pat[k] && pat[k] != ';' && k < 20; k++)
            up[k] = (char)toupper((unsigned char)pat[k]);
        up[k] = 0;
        make_pattern(up, h->find_pat);
    } else {
        make_pattern(path, h->find_pat);
    }
    return find_next(m, d);
}

static int32_t f_erase(ps1_machine *m, uint32_t name_a)
{
    char full[128];
    gstr(m, name_a, full, sizeof full);
    int port;
    const char *path;
    if (parse_dev(full, &port, &path) != DEV_BU)
        return 0;
    ps1_card *c = card_of(m, port);
    if (!c || !card_formatted(c))
        return 0;
    int b = card_find(c, path, 0);
    if (b < 0) {
        m->hle->last_err = E_NOENT;
        return 0;
    }
    card_erase_file(c, b);
    return 1;
}

static int32_t f_undelete(ps1_machine *m, uint32_t name_a)
{
    char full[128];
    gstr(m, name_a, full, sizeof full);
    int port;
    const char *path;
    if (parse_dev(full, &port, &path) != DEV_BU)
        return 0;
    ps1_card *c = card_of(m, port);
    if (!c || !card_formatted(c))
        return 0;
    for (int b = 0; b < 15; b++) {
        uint8_t *e = card_dir(c, b);
        char name[21];
        memcpy(name, e + 10, 20);
        name[20] = 0;
        if (e[0] == 0xA1 && name_match(name, path)) {
            for (int k = b, guard = 0; k >= 0 && k < 15 && guard < 15; guard++) {
                uint8_t *x = card_dir(c, k);
                int nx = x[8] | x[9] << 8;
                x[0] = (uint8_t)((x[0] & 0x0F) | 0x50);
                card_fix_chk(x);
                k = nx == 0xFFFF ? -1 : nx;
            }
            card_dirty(c);
            return 1;
        }
    }
    return 0;
}

static int32_t f_rename(ps1_machine *m, uint32_t old_a, uint32_t new_a)
{
    char o[128], n[128];
    gstr(m, old_a, o, sizeof o);
    gstr(m, new_a, n, sizeof n);
    int po, pn;
    const char *op, *np;
    if (parse_dev(o, &po, &op) != DEV_BU || parse_dev(n, &pn, &np) != DEV_BU || po != pn)
        return 0;
    ps1_card *c = card_of(m, po);
    if (!c || !card_formatted(c) || card_find(c, np, 0) >= 0)
        return 0;
    int b = card_find(c, op, 0);
    if (b < 0)
        return 0;
    uint8_t *e = card_dir(c, b);
    memset(e + 10, 0, 20);
    strncpy((char *)e + 10, np, 20);
    card_fix_chk(e);
    card_dirty(c);
    return 1;
}

static int32_t f_format(ps1_machine *m, uint32_t name_a)
{
    char full[128];
    gstr(m, name_a, full, sizeof full);
    int port;
    const char *path;
    if (parse_dev(full, &port, &path) != DEV_BU)
        return 0;
    ps1_card *c = card_of(m, port);
    if (!c)
        return 0;
    ps1_card_format(c->data);
    card_dirty(c);
    return 1;
}

static int32_t f_cd(ps1_machine *m, uint32_t name_a)
{
    char full[128];
    gstr(m, name_a, full, sizeof full);
    int port;
    const char *path;
    if (parse_dev(full, &port, &path) != DEV_CDROM)
        return 0;
    uint32_t lba, size;
    bool dir;
    if (cd_find(m, path, &lba, &size, &dir) != 0 || !dir)
        return 0;
    m->hle->cd_dir_lba = lba;
    m->hle->cd_dir_size = size;
    return 1;
}

/* ---- Programs (A(41h)..A(43h), A(51h)) ---- */

/* The file's PS-X EXE header (0x3C bytes from offset 0x10) into hdr; with text, the text into memory. */
static int load_exe(ps1_machine *m, uint32_t name_a, uint32_t hdr, bool text)
{
    int32_t fd = f_open(m, name_a, F_READ);
    if (fd < 0)
        return 0;
    hle_file *f = &m->hle->files[fd];
    uint8_t head[0x800];
    int ok = 0;
    if (f->dev == DEV_CDROM && f->size >= 0x800) {
        uint8_t sec[2048];
        if (cd_sector(m, f->first, sec) == 0) {
            memcpy(head, sec, 0x800);
            gwrite(m, hdr, head + 0x10, 0x3C);
            ok = 1;
            if (text) {
                uint32_t t_addr = rd32(m, hdr + 8), t_size = rd32(m, hdr + 12);
                f->pos = 0x800;
                if (t_size > f->size - 0x800)
                    t_size = f->size - 0x800;
                f_rw(m, (uint32_t)fd, t_addr, t_size, false);
            }
        }
    }
    f_close(m, (uint32_t)fd);
    return ok;
}

/* A(43h) Exec(hdr, argc, argv): BSS cleared, the stack from the header, the program called; when it returns, Exec
 * returns 1 to its caller with sp / fp / gp / ra restored from the header. */
static void exec_prog(ps1_machine *m, uint32_t hdr, uint32_t argc, uint32_t argv)
{
    uint32_t b_addr = rd32(m, hdr + 0x18), b_size = rd32(m, hdr + 0x1C);
    uint32_t s_addr = rd32(m, hdr + 0x20), s_size = rd32(m, hdr + 0x24);
    for (uint32_t i = 0; i < b_size; i += 4)
        wr32(m, b_addr + i, 0);
    wr32(m, hdr + 0x28, SP);
    wr32(m, hdr + 0x2C, m->r[30]);
    wr32(m, hdr + 0x30, m->r[28]);
    wr32(m, hdr + 0x34, RA);
    wr32(m, hdr + 0x38, m->r[16]);
    m->hle->exec_hdr = hdr;
    if (s_addr) {
        SP = s_addr + s_size;
        m->r[30] = SP;
    }
    m->r[28] = rd32(m, hdr + 4);
    m->r[4] = argc;
    m->r[5] = argv;
    RA = stub_addr(3, K_EXEC_RET);
    set_pc(m, rd32(m, hdr));
    escape(m);
}

static void exec_ret(ps1_machine *m)
{
    uint32_t hdr = m->hle->exec_hdr;
    if (!hdr) {
        halt(m, "the program returned");
        return;
    }
    SP = rd32(m, hdr + 0x28);
    m->r[30] = rd32(m, hdr + 0x2C);
    m->r[28] = rd32(m, hdr + 0x30);
    RA = rd32(m, hdr + 0x34);
    m->r[16] = rd32(m, hdr + 0x38);
    ret(m, 1);
    set_pc(m, RA);
}

/* ---- libc helpers on guest memory ---- */

static uint32_t g_strlen(ps1_machine *m, uint32_t s)
{
    uint32_t n = 0;
    while (s && rd8(m, s + n) && n < 0x200000u)
        n++;
    return n;
}

static int32_t g_strcmp(ps1_machine *m, uint32_t a, uint32_t b, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++) {
        uint8_t x = rd8(m, a + i), y = rd8(m, b + i);
        if (x != y)
            return (int32_t)x - (int32_t)y;
        if (!x)
            break;
    }
    return 0;
}

static uint32_t g_strchr(ps1_machine *m, uint32_t s, uint8_t c, bool last)
{
    uint32_t found = 0;
    if (!s)
        return 0;
    for (uint32_t i = 0;; i++) {
        uint8_t x = rd8(m, s + i);
        if (x == c) {
            found = s + i;
            if (!last)
                return found;
        }
        if (!x || i > 0x200000u)
            return found;
    }
}

static bool in_set(ps1_machine *m, uint32_t set, uint8_t c)
{
    for (uint32_t i = 0;; i++) {
        uint8_t x = rd8(m, set + i);
        if (!x)
            return false;
        if (x == c)
            return true;
    }
}

static int digit_of(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return 99;
}

static uint32_t g_strtol(ps1_machine *m, uint32_t s, uint32_t endp, int base, bool is_signed)
{
    uint32_t p = s;
    if (!s) {
        if (endp)
            wr32(m, endp, s);
        return 0;
    }
    while (isspace(rd8(m, p)))
        p++;
    bool neg = false;
    if (rd8(m, p) == '-' || rd8(m, p) == '+') {
        neg = rd8(m, p) == '-';
        p++;
    }
    if ((base == 0 || base == 16) && rd8(m, p) == '0' && (rd8(m, p + 1) | 0x20) == 'x') {
        p += 2;
        base = 16;
    } else if (base == 0 && rd8(m, p) == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    uint32_t v = 0, start = p;
    for (;; p++) {
        int d = digit_of(rd8(m, p));
        if (d >= base)
            break;
        v = v * (uint32_t)base + (uint32_t)d;
    }
    if (endp)
        wr32(m, endp, p == start ? s : p);
    (void)is_signed;
    return neg ? (uint32_t)-(int32_t)v : v;
}

/* qsort with the guest comparison (insertion sort on guest memory: stable, small arrays in practice). */
static void g_qsort(ps1_machine *m, uint32_t base, uint32_t n, uint32_t size, uint32_t cmp)
{
    if (size == 0 || size > 1024)
        return;
    uint8_t key[1024], tmp[1024];
    for (uint32_t i = 1; i < n; i++) {
        gread(m, base + i * size, key, size);
        uint32_t j = i;
        while (j > 0) {
            /* the key goes to a scratch slot past the array? No: compare in place through a kernel buffer */
            gwrite(m, 0x80000000u | 0x9000u, key, size);
            bool esc;
            int32_t r = (int32_t)guest_call(m, cmp, base + (j - 1) * size, 0x80000000u | 0x9000u, 0, &esc);
            if (esc)
                return;
            if (r <= 0)
                break;
            gread(m, base + (j - 1) * size, tmp, size);
            gwrite(m, base + j * size, tmp, size);
            j--;
        }
        gwrite(m, base + j * size, key, size);
    }
}

/* ---- Unimplemented calls ---- */

static void unimplemented(ps1_machine *m, int table, int fn)
{
    ps1_hle *h = m->hle;
    if (table < 3 && fn < 256 && !(h->warned[table][fn >> 3] & (1u << (fn & 7)))) {
        h->warned[table][fn >> 3] |= (uint8_t)(1u << (fn & 7));
        h->unimpl++;
        fprintf(stderr, "ps1: HLE BIOS: unimplemented call %c(%02Xh) from %08X\n", "ABC"[table], fn, RA);
    }
    ret(m, 0);
}

/* ---- The tables ---- */

typedef void (*hle_fn)(ps1_machine *m);
typedef struct hle_call {
    const char *name;
    hle_fn fn;
} hle_call;

#define FN(name) static void name(ps1_machine *m)

/* A(00h)..: files and libc */
FN(a_open) { ret(m, (uint32_t)f_open(m, A0, A1)); }
FN(a_lseek) { ret(m, (uint32_t)f_lseek(m, A0, (int32_t)A1, A2)); }
FN(a_read) { ret(m, (uint32_t)f_rw(m, A0, A1, A2, false)); }
FN(a_write) { ret(m, (uint32_t)f_rw(m, A0, A1, A2, true)); }
FN(a_close) { ret(m, (uint32_t)f_close(m, A0)); }
FN(a_ioctl) { ret(m, fd_file(m, A0) ? 1 : 0); }
FN(a_exit) { halt(m, "exit"); }
FN(a_isatty) { hle_file *f = fd_file(m, A0); ret(m, A0 <= 1 || (f && f->dev == DEV_TTY)); }
FN(a_getc) { ret(m, 0xFFFFFFFFu); }
FN(a_putc) { if (A1 <= 1) tty_putc(m, (int)A0); ret(m, A0); }
FN(a_todigit) { int d = digit_of((int)(A0 & 0xFF)); ret(m, d < 36 ? (uint32_t)d : 9999999u); }
FN(a_strtoul) { ret(m, g_strtol(m, A0, A1, (int)A2, false)); }
FN(a_strtol) { ret(m, g_strtol(m, A0, A1, (int)A2, true)); }
FN(a_abs) { ret(m, (int32_t)A0 < 0 ? (uint32_t)-(int32_t)A0 : A0); }
FN(a_atoi) { ret(m, g_strtol(m, A0, 0, 10, true)); }
FN(a_atob) { wr32(m, A1, g_strtol(m, A0, 0, 10, true)); ret(m, A0 + g_strlen(m, A0)); }
FN(a_setjmp) { do_setjmp(m, A0); ret(m, 0); }
FN(a_longjmp) { do_longjmp(m, A0, A1); }
FN(a_strcat)
{
    uint32_t d = A0 + g_strlen(m, A0), i = 0;
    for (;; i++) {
        uint8_t c = rd8(m, A1 + i);
        wr8(m, d + i, c);
        if (!c)
            break;
    }
    ret(m, A0);
}
FN(a_strncat)
{
    uint32_t d = A0 + g_strlen(m, A0), i = 0;
    for (; i < A2; i++) {
        uint8_t c = rd8(m, A1 + i);
        if (!c)
            break;
        wr8(m, d + i, c);
    }
    wr8(m, d + i, 0);
    ret(m, A0);
}
FN(a_strcmp) { ret(m, (uint32_t)g_strcmp(m, A0, A1, 0xFFFFFFFFu)); }
FN(a_strncmp) { ret(m, (uint32_t)g_strcmp(m, A0, A1, A2)); }
FN(a_strcpy)
{
    for (uint32_t i = 0;; i++) {
        uint8_t c = rd8(m, A1 + i);
        wr8(m, A0 + i, c);
        if (!c)
            break;
    }
    ret(m, A0);
}
FN(a_strncpy)
{
    bool end = false;
    for (uint32_t i = 0; i < A2; i++) {
        uint8_t c = end ? 0 : rd8(m, A1 + i);
        if (!c)
            end = true;
        wr8(m, A0 + i, c);
    }
    ret(m, A0);
}
FN(a_strlen) { ret(m, g_strlen(m, A0)); }
FN(a_strchr) { ret(m, g_strchr(m, A0, (uint8_t)A1, false)); }
FN(a_strrchr) { ret(m, g_strchr(m, A0, (uint8_t)A1, true)); }
FN(a_strpbrk)
{
    for (uint32_t p = A0; p && rd8(m, p); p++)
        if (in_set(m, A1, rd8(m, p))) {
            ret(m, p);
            return;
        }
    ret(m, 0);
}
FN(a_strspn)
{
    uint32_t n = 0;
    while (rd8(m, A0 + n) && in_set(m, A1, rd8(m, A0 + n)))
        n++;
    ret(m, n);
}
FN(a_strcspn)
{
    uint32_t n = 0;
    while (rd8(m, A0 + n) && !in_set(m, A1, rd8(m, A0 + n)))
        n++;
    ret(m, n);
}
FN(a_strtok)
{
    ps1_hle *h = m->hle;
    uint32_t p = A0 ? A0 : h->strtok_next;
    if (!p) {
        ret(m, 0);
        return;
    }
    while (rd8(m, p) && in_set(m, A1, rd8(m, p)))
        p++;
    if (!rd8(m, p)) {
        h->strtok_next = 0;
        ret(m, 0);
        return;
    }
    uint32_t tok = p;
    while (rd8(m, p) && !in_set(m, A1, rd8(m, p)))
        p++;
    if (rd8(m, p)) {
        wr8(m, p, 0);
        h->strtok_next = p + 1;
    } else {
        h->strtok_next = 0;
    }
    ret(m, tok);
}
FN(a_strstr)
{
    uint32_t nl = g_strlen(m, A1);
    for (uint32_t p = A0; p && rd8(m, p); p++)
        if (g_strcmp(m, p, A1, nl) == 0) {
            ret(m, p);
            return;
        }
    ret(m, nl == 0 ? A0 : 0);
}
FN(a_toupper) { ret(m, (uint32_t)toupper((int)(A0 & 0xFF))); }
FN(a_tolower) { ret(m, (uint32_t)tolower((int)(A0 & 0xFF))); }
static void g_memmove(ps1_machine *m, uint32_t d, uint32_t s, uint32_t n)
{
    if (d > s && d < s + n)
        for (uint32_t i = n; i-- > 0;)
            wr8(m, d + i, rd8(m, s + i));
    else
        for (uint32_t i = 0; i < n; i++)
            wr8(m, d + i, rd8(m, s + i));
}
FN(a_bcopy) { g_memmove(m, A1, A0, A2); ret(m, A1); }
FN(a_bzero) { for (uint32_t i = 0; i < A1; i++) wr8(m, A0 + i, 0); ret(m, A0); }
static int32_t g_memcmp(ps1_machine *m, uint32_t a, uint32_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = rd8(m, a + i), y = rd8(m, b + i);
        if (x != y)
            return (int32_t)x - (int32_t)y;
    }
    return 0;
}
FN(a_bcmp) { ret(m, (uint32_t)g_memcmp(m, A0, A1, A2)); }
FN(a_memcpy) { for (uint32_t i = 0; i < A2; i++) wr8(m, A0 + i, rd8(m, A1 + i)); ret(m, A0); }
FN(a_memset) { for (uint32_t i = 0; i < A2; i++) wr8(m, A0 + i, (uint8_t)A1); ret(m, A0); }
FN(a_memmove) { g_memmove(m, A0, A1, A2); ret(m, A0); }
FN(a_memchr)
{
    for (uint32_t i = 0; i < A2; i++)
        if (rd8(m, A0 + i) == (uint8_t)A1) {
            ret(m, A0 + i);
            return;
        }
    ret(m, 0);
}
FN(a_rand)
{
    ps1_hle *h = m->hle;
    h->rand_next = h->rand_next * 1103515245u + 12345u;
    ret(m, (h->rand_next >> 16) & 0x7FFFu);
}
FN(a_srand) { m->hle->rand_next = A0; ret(m, 0); }
FN(a_qsort) { g_qsort(m, A0, A1, A2, A3); ret(m, 0); }
FN(a_malloc) { ret(m, heap_alloc(m, m->hle->heap_start, m->hle->heap_end, A0)); }
FN(a_free) { heap_free(m, m->hle->heap_start, m->hle->heap_end, A0); ret(m, 0); }
FN(a_lsearch)
{
    /* lsearch(key, base, nmemb, size, cmp): the element equal to key (cmp == 0), else key appended. Arguments 5..: sp. */
    uint32_t key = A0, base = A1, np = A2, size = A3, cmp = arg(m, 4);
    uint32_t n = rd32(m, np);
    for (uint32_t i = 0; i < n; i++) {
        bool esc;
        int32_t r = (int32_t)guest_call(m, cmp, key, base + i * size, 0, &esc);
        if (esc)
            return;
        if (r == 0) {
            ret(m, base + i * size);
            return;
        }
    }
    g_memmove(m, base + n * size, key, size);
    wr32(m, np, n + 1);
    ret(m, base + n * size);
}
FN(a_bsearch)
{
    uint32_t key = A0, base = A1, n = A2, size = A3, cmp = arg(m, 4);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        bool esc;
        int32_t r = (int32_t)guest_call(m, cmp, key, base + mid * size, 0, &esc);
        if (esc)
            return;
        if (r == 0) {
            ret(m, base + mid * size);
            return;
        }
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    ret(m, 0);
}
FN(a_calloc)
{
    uint32_t n = A0 * A1, p = heap_alloc(m, m->hle->heap_start, m->hle->heap_end, n);
    for (uint32_t i = 0; p && i < n; i++)
        wr8(m, p + i, 0);
    ret(m, p);
}
FN(a_realloc)
{
    ps1_hle *h = m->hle;
    uint32_t old = A0, n = A1;
    if (!old) {
        ret(m, heap_alloc(m, h->heap_start, h->heap_end, n));
        return;
    }
    if (!n) {
        heap_free(m, h->heap_start, h->heap_end, old);
        ret(m, 0);
        return;
    }
    uint32_t osz = (rd32(m, old - 4) & ~3u) - 4;
    uint32_t p = heap_alloc(m, h->heap_start, h->heap_end, n);
    if (p) {
        g_memmove(m, p, old, osz < n ? osz : n);
        heap_free(m, h->heap_start, h->heap_end, old);
    }
    ret(m, p);
}
FN(a_initheap) { heap_init(m, &m->hle->heap_start, &m->hle->heap_end, A0, A1); ret(m, 0); }
FN(a_getchar) { ret(m, 0xFFFFFFFFu); }
FN(a_putchar) { tty_putc(m, (int)A0); ret(m, A0); }
FN(a_gets) { if (A0) wr8(m, A0, 0); ret(m, 0); }
FN(a_puts)
{
    for (uint32_t i = 0; A0 && i < 0x10000; i++) {
        uint8_t c = rd8(m, A0 + i);
        if (!c)
            break;
        tty_putc(m, c);
    }
    tty_putc(m, '\n');
    ret(m, 0);
}
FN(b_puts)
{
    /* B(3Fh) puts: no newline added (the retail BIOS's quirk, OpenBIOS keeps it) */
    for (uint32_t i = 0; A0 && i < 0x10000; i++) {
        uint8_t c = rd8(m, A0 + i);
        if (!c)
            break;
        tty_putc(m, c);
    }
    ret(m, 0);
}
FN(a_printf) { hle_printf(m, A0, 1); ret(m, 0); }
FN(a_unresolved) { halt(m, "unresolved exception (A(40h))"); }
FN(a_loadtest) { ret(m, (uint32_t)load_exe(m, A0, A1, false)); }
FN(a_load) { ret(m, (uint32_t)load_exe(m, A0, A1, true)); }
FN(a_exec) { exec_prog(m, A0, A1, A2); }
FN(a_flushcache) { ret(m, 0); }
FN(a_init_vectors) { ret(m, 0); } /* the vectors are in place since the boot */
#define GP0 0x1F801810u
#define GP1 0x1F801814u
FN(a_gpu_dw)
{
    /* GPU_dw(x, y, w, h, src): a CPU-to-VRAM rectangle, words from src */
    uint32_t x = A0, y = A1, w = A2, hh = A3, src = arg(m, 4);
    io_wr(m, GP0, 0xA0000000u);
    io_wr(m, GP0, (y & 0xFFFF) << 16 | (x & 0xFFFF));
    io_wr(m, GP0, (hh & 0xFFFF) << 16 | (w & 0xFFFF));
    uint32_t words = (w * hh + 1) / 2;
    for (uint32_t i = 0; i < words; i++)
        io_wr(m, GP0, rd32(m, src + 4 * i));
    ret(m, 0);
}
FN(a_gpu_send_dma) { a_gpu_dw(m); }
FN(a_gpu_gp1) { io_wr(m, GP1, A0); ret(m, 0); }
FN(a_gpu_cw) { io_wr(m, GP0, A0); ret(m, 0); }
FN(a_gpu_cwp) { for (uint32_t i = 0; i < A1; i++) io_wr(m, GP0, rd32(m, A0 + 4 * i)); ret(m, 0); }
FN(a_gpu_list)
{
    /* send_gpu_linked_list(src): an ordering table walked to its 0xFFFFFF terminator */
    uint32_t p = A0 & 0xFFFFFFu;
    for (int guard = 0; guard < 1 << 20; guard++) {
        uint32_t hdr = rd32(m, p);
        uint32_t n = hdr >> 24;
        for (uint32_t i = 0; i < n; i++)
            io_wr(m, GP0, rd32(m, p + 4 + 4 * i));
        p = hdr & 0xFFFFFFu;
        if (p == 0xFFFFFFu)
            break;
    }
    ret(m, 0);
}
FN(a_gpu_abort) { io_wr(m, GP1, 0x04000000u); ret(m, 0); }
FN(a_gpu_status) { ret(m, io_rd(m, GP1)); }
FN(a_gpu_sync) { ret(m, 0); }
FN(a_loadexec)
{
    uint32_t hdr = 0x80000000u | K_LOADHDR;
    uint32_t sp = A1, spoff = A2;
    if (!load_exe(m, A0, hdr, true)) {
        halt(m, "LoadExec: no such file");
        return;
    }
    wr32(m, hdr + 0x20, sp);
    wr32(m, hdr + 0x24, spoff);
    exec_prog(m, hdr, 1, 0);
}
FN(a_dummy) { ret(m, 0); }
FN(a_one) { ret(m, 1); }
FN(a_setconf)
{
    /* SetConf(ev, tcb, stack): the kernel objects reset with the new counts */
    ps1_hle *h = m->hle;
    h->conf_ev = A0;
    h->conf_tcb = A1;
    h->conf_stack = A2;
    init_events(m, A0);
    init_threads(m, A1);
    ret(m, 0);
}
FN(a_getconf)
{
    ps1_hle *h = m->hle;
    if (A0) wr32(m, A0, h->evcb_count);
    if (A1) wr32(m, A1, h->tcb_count);
    if (A2) wr32(m, A2, h->conf_stack);
    ret(m, 0);
}
FN(a_setmemsize) { wr32(m, K_RAMSIZE, A0); ret(m, 0); }
FN(a_cdgetlbn)
{
    char full[128];
    gstr(m, A0, full, sizeof full);
    int port;
    const char *path;
    parse_dev(full, &port, &path);
    uint32_t lba, size;
    ret(m, cd_find(m, path, &lba, &size, NULL) == 0 ? lba : 0xFFFFFFFFu);
}
FN(a_cdreadsector)
{
    /* CdReadSector(count, sector, buf): 2048-byte sectors, synchronous */
    uint8_t sec[2048];
    for (uint32_t i = 0; i < A0; i++) {
        if (cd_sector(m, A1 + i, sec) != 0) {
            ret(m, 0xFFFFFFFFu);
            return;
        }
        gwrite(m, A2 + i * 2048, sec, 2048);
    }
    ret(m, A0);
}
FN(a_cdgetstatus) { ret(m, m->disc ? 0x02u : 0x10u); }
FN(a_card_info)
{
    /* _card_info(port): the card's state as an event (IOE: there, TIMEOUT: none, NEWCARD: inserted since) */
    int port = (A0 >> 4) & 1;
    ps1_card *c = card_of(m, port);
    m->hle->card_chan = A0;
    uint32_t spec = !c ? EVS_TIMEOUT : (c->flag & 0x08) && !m->hle->card_new[port] ? EVS_NEWCARD : EVS_IOE;
    if (c && spec == EVS_NEWCARD)
        m->hle->card_new[port] = true;
    deliver_event(m, EVC_SW_CARD, spec);
    deliver_event(m, EVC_HW_CARD, spec);
    ret(m, 1);
}
FN(a_card_load_dir) { deliver_event(m, EVC_SW_CARD, card_of(m, (A0 >> 4) & 1) ? EVS_IOE : EVS_TIMEOUT); ret(m, 1); }
FN(a_getsysinfo)
{
    /* GetSystemInfo(index): 0 the kernel date (BCD yyyymmdd), 2 the RAM size in KB */
    ret(m, A0 == 0 ? 0x19951204u : A0 == 2 ? 2048u : 0);
}

/* B(00h)..: kernel */
FN(b_kmalloc) { ret(m, heap_alloc(m, m->hle->kheap_start, m->hle->kheap_end, A0)); }
FN(b_kfree) { heap_free(m, m->hle->kheap_start, m->hle->kheap_end, A0); ret(m, 0); }
FN(b_init_timer)
{
    uint32_t t = A0 & 0xFFFF;
    if (t >= 3) {
        ret(m, 0);
        return;
    }
    uint32_t base = 0x1F801100u + 0x10u * t;
    io_wr(m, base + 4, 0);
    io_wr(m, base + 8, A1 & 0xFFFF);
    uint32_t mode = 0x0048, fl = A2;
    if (fl & 0x0010) mode |= 0x0001;
    if (fl & 0x0001) mode |= 0x0100;
    if (fl & 0x1000) mode |= 0x0010;
    io_wr(m, base + 4, mode);
    ret(m, 1);
}
FN(b_get_timer) { uint32_t t = A0 & 0xFFFF; ret(m, t > 2 ? 0 : io_rd(m, 0x1F801100u + 0x10u * t) & 0xFFFF); }
static const uint32_t s_timer_mask[4] = {0x10, 0x20, 0x40, 0x01};
FN(b_enable_timer_irq) { uint32_t t = A0 & 3; io_wr(m, I_MASK, io_rd(m, I_MASK) | s_timer_mask[t]); ret(m, (A0 & 0xFFFF) <= 2); }
FN(b_disable_timer_irq) { io_wr(m, I_MASK, io_rd(m, I_MASK) & ~s_timer_mask[A0 & 3]); ret(m, 1); }
FN(b_restart_timer)
{
    uint32_t t = A0 & 0xFFFF;
    if (t > 2) {
        ret(m, 0);
        return;
    }
    io_wr(m, 0x1F801100u + 0x10u * t, 0);
    ret(m, 1);
}
FN(b_deliver_event) { if (deliver_event(m, A0, A1)) ret(m, 0); }
FN(b_open_event)
{
    ps1_hle *h = m->hle;
    for (uint32_t i = 0; i < h->evcb_count; i++) {
        uint32_t e = PS1_HLE_EVCB + i * EVCB_SIZE;
        if (rd32(m, e + 4) == EV_FREE) {
            wr32(m, e, A0);
            wr32(m, e + 8, A1);
            wr32(m, e + 12, A2);
            wr32(m, e + 16, A3);
            wr32(m, e + 4, EV_DISABLED);
            ret(m, i | 0xF1000000u);
            return;
        }
    }
    ret(m, 0xFFFFFFFFu);
}
FN(b_close_event) { if (ev_valid(m, A0)) wr32(m, evcb(A0) + 4, EV_FREE); ret(m, 1); }
FN(b_wait_event)
{
    if (!ev_valid(m, A0)) {
        ret(m, 0);
        return;
    }
    uint32_t e = evcb(A0) + 4, st = rd32(m, e);
    if (st == EV_PENDING) {
        wr32(m, e, EV_ENABLED);
        ret(m, 1);
    } else if (st == EV_ENABLED) {
        /* busy: run the stub again (interrupts come in between) until an IRQ delivers the event */
        m->cycles += 16;
        set_pc(m, m->cur_pc);
    } else {
        ret(m, 0);
    }
}
FN(b_test_event)
{
    if (!ev_valid(m, A0)) {
        ret(m, 0);
        return;
    }
    uint32_t e = evcb(A0) + 4;
    if (rd32(m, e) == EV_PENDING) {
        wr32(m, e, EV_ENABLED);
        ret(m, 1);
    } else {
        ret(m, 0);
    }
}
FN(b_enable_event) { if (ev_valid(m, A0) && rd32(m, evcb(A0) + 4)) wr32(m, evcb(A0) + 4, EV_ENABLED); ret(m, 1); }
FN(b_disable_event) { if (ev_valid(m, A0) && rd32(m, evcb(A0) + 4)) wr32(m, evcb(A0) + 4, EV_DISABLED); ret(m, 1); }
FN(b_open_thread)
{
    ps1_hle *h = m->hle;
    for (uint32_t i = 0; i < h->tcb_count; i++) {
        uint32_t t = PS1_HLE_TCB + i * TCB_SIZE;
        if (rd32(m, t) == 0x1000u) {
            wr32(m, t, 0x4000u);
            wr32(m, t + 4, 0x1000u);
            wr32(m, t + 8 + 29 * 4, A1);
            wr32(m, t + 8 + 30 * 4, A1);
            wr32(m, t + 8 + 28 * 4, A2);
            wr32(m, t + 8 + 0x80, A0);
            wr32(m, t + 8 + 0x8C, 0x40000404u); /* COP2 on, interrupts on after the RFE */
            ret(m, i | 0xFF000000u);
            return;
        }
    }
    ret(m, 0xFFFFFFFFu);
}
FN(b_close_thread) { if ((A0 & 0xFFFF) < m->hle->tcb_count) wr32(m, PS1_HLE_TCB + (A0 & 0xFFFF) * TCB_SIZE, 0x1000u); ret(m, 1); }
FN(b_change_thread)
{
    /* as the syscall it is (ChangeThreadSubFunction): this thread's registers saved as at the syscall, returning 1
     * to ra; then the other thread's registers back */
    uint32_t id = A0 & 0xFFFF;
    if (id >= m->hle->tcb_count) {
        ret(m, 0);
        return;
    }
    uint32_t t = tcb_cur(m) + 8;
    for (int i = 1; i < 32; i++)
        wr32(m, t + 4u * (uint32_t)i, m->r[i]);
    wr32(m, t + 0x08, 1);
    wr32(m, t + 0x80, RA);
    wr32(m, t + 0x84, m->hi);
    wr32(m, t + 0x88, m->lo);
    /* the SR as the syscall's RFE leaves it */
    uint32_t sr = m->cop0[C0_SR];
    wr32(m, t + 0x8C, (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu));
    wr32(m, K_PCB, 0x80000000u | (PS1_HLE_TCB + id * TCB_SIZE));
    return_from_exception(m);
}
FN(b_init_pad)
{
    ps1_hle *h = m->hle;
    h->pad_buf[0] = A0;
    h->pad_size[0] = A1;
    h->pad_buf[1] = A2;
    h->pad_size[1] = A3;
    h->pad_hl = 0;
    for (uint32_t i = 0; i < A1; i++) wr8(m, A0 + i, 0);
    for (uint32_t i = 0; i < A3; i++) wr8(m, A2 + i, 0);
    h->pad_started = true;
    ret(m, 1);
}
FN(b_start_pad)
{
    sio0_install(m);
    irq_ack(m, 1u);
    ret(m, 1);
}
FN(b_stop_pad)
{
    m->hle->timer_autoack[3] = true;
    deq_int(m, 2, hinfo(HI_SIO0));
    ret(m, 1);
}
FN(b_pad_init_hl)
{
    /* OutdatedPadInitAndStart(type, buf, ...): type 0x20000000 / 0x20000001 only (6 buttons / flying-V) */
    ps1_hle *h = m->hle;
    if (A0 != 0x20000000u && A0 != 0x20000001u) {
        ret(m, 0);
        return;
    }
    uint32_t b1 = 0x80000000u | 0x9800u, b2 = b1 + 0x24;
    for (uint32_t i = 0; i < 0x22; i++) {
        wr8(m, b1 + i, 0xFF);
        wr8(m, b2 + i, 0xFF);
    }
    h->pad_buf[0] = b1;
    h->pad_buf[1] = b2;
    h->pad_size[0] = h->pad_size[1] = 0x22;
    h->pad_started = true;
    h->pad_hl = A1;
    sio0_install(m);
    ret(m, 2);
}
FN(b_pad_get_hl)
{
    uint32_t v = m->hle->pad_hl ? rd32(m, m->hle->pad_hl) : 0xFFFFFFFFu;
    ret(m, v);
}
FN(b_return_from_exception) { return_from_exception(m); }
FN(b_set_default_exit) { wr32(m, K_VARS, 0x80000000u | K_JMPBUF); ret(m, 0); }
FN(b_set_custom_exit) { wr32(m, K_VARS, A0); ret(m, 0); }
FN(b_undeliver_event) { undeliver_event(m, A0, A1); ret(m, 0); }
FN(b_cd) { ret(m, (uint32_t)f_cd(m, A0)); }
FN(b_format) { ret(m, (uint32_t)f_format(m, A0)); }
FN(b_firstfile) { ret(m, find_first(m, A0, A1)); }
FN(b_nextfile) { ret(m, find_next(m, A0)); }
FN(b_rename) { ret(m, (uint32_t)f_rename(m, A0, A1)); }
FN(b_erase) { ret(m, (uint32_t)f_erase(m, A0)); }
FN(b_undelete) { ret(m, (uint32_t)f_undelete(m, A0)); }
FN(b_init_card)
{
    m->hle->pad_started = A0 != 0 && m->hle->pad_buf[0];
    ret(m, 0);
}
FN(b_start_card)
{
    sio0_install(m);
    m->hle->card_started = true;
    ret(m, 1);
}
FN(b_stop_card)
{
    m->hle->timer_autoack[3] = true;
    deq_int(m, 2, hinfo(HI_SIO0));
    m->hle->card_started = false;
    ret(m, 1);
}
FN(b_card_write)
{
    /* write_card_sector(port, sector, src): synchronous here, the completion events delivered at once */
    int port = (A0 >> 4) & 1;
    ps1_card *c = card_of(m, port);
    m->hle->card_chan = A0;
    if (!c || A1 >= 1024) {
        deliver_event(m, EVC_SW_CARD, EVS_TIMEOUT);
        deliver_event(m, EVC_HW_CARD, EVS_TIMEOUT);
        ret(m, c ? 0 : 1);
        return;
    }
    gread(m, A2, card_frame(c, (int)A1), 128);
    card_dirty(c);
    c->flag &= ~0x08u;
    card_events_done(m, true);
    ret(m, 1);
}
FN(b_card_read)
{
    int port = (A0 >> 4) & 1;
    ps1_card *c = card_of(m, port);
    m->hle->card_chan = A0;
    if (!c || A1 >= 1024) {
        deliver_event(m, EVC_SW_CARD, EVS_TIMEOUT);
        deliver_event(m, EVC_HW_CARD, EVS_TIMEOUT);
        ret(m, c ? 0 : 1);
        return;
    }
    gwrite(m, A2, card_frame(c, (int)A1), 128);
    card_events_done(m, true);
    ret(m, 1);
}
FN(b_card_allow_new) { ret(m, 0); }
FN(b_krom2rawadd) { ret(m, 0xFFFFFFFFu); } /* no font ROM in HLE */
FN(b_get_errno) { hle_file *f = fd_file(m, A0); ret(m, f ? (uint32_t)f->err : (uint32_t)m->hle->last_err); }
FN(b_get_error) { ret(m, (uint32_t)m->hle->last_err); }
FN(b_get_c0_table) { ret(m, 0x80000000u | PS1_HLE_CTAB); }
FN(b_get_b0_table) { ret(m, 0x80000000u | PS1_HLE_BTAB); }
FN(b_card_chan) { ret(m, m->hle->card_chan); }
FN(b_change_clear_pad) { m->hle->sio0_autoack = A0 != 0; ret(m, 0); }
FN(b_card_status) { ret(m, 1); } /* ready: the operations complete at once */
FN(b_card_wait) { ret(m, 1); }

/* C(00h)..: kernel set-up */
FN(c_enq_rcnt)
{
    io_wr(m, I_MASK, io_rd(m, I_MASK) & ~0x71u);
    for (int i = 0; i < 4; i++) {
        m->hle->timer_autoack[i] = true;
        enq_int(m, A0, hinfo(HI_T0 + i));
    }
    for (uint32_t t = 0; t < 3; t++) {
        io_wr(m, 0x1F801104u + 0x10u * t, 0);
        io_wr(m, 0x1F801108u + 0x10u * t, 0);
        io_wr(m, 0x1F801100u + 0x10u * t, 0);
    }
    ret(m, 0);
}
FN(c_enq_syscall) { enq_int(m, A0, hinfo(HI_SYSCALL)); ret(m, 0); }
FN(c_sys_enq_int) { enq_int(m, A0, A1); ret(m, 0); }
FN(c_sys_deq_int) { ret(m, deq_int(m, A0, A1)); }
FN(c_free_evcb)
{
    for (uint32_t i = 0; i < m->hle->evcb_count; i++)
        if (rd32(m, PS1_HLE_EVCB + i * EVCB_SIZE + 4) == EV_FREE) {
            ret(m, i);
            return;
        }
    ret(m, 0xFFFFFFFFu);
}
FN(c_free_tcb)
{
    for (uint32_t i = 0; i < m->hle->tcb_count; i++)
        if (rd32(m, PS1_HLE_TCB + i * TCB_SIZE) == 0x1000u) {
            ret(m, i);
            return;
        }
    ret(m, 0xFFFFFFFFu);
}
FN(c_exception_handler) { set_pc(m, 0x80000000u | K_EXC); } /* jumps into the handler, as the vector does */
FN(c_install_exc) { ret(m, 0); }
FN(c_sys_init_memory) { heap_init(m, &m->hle->kheap_start, &m->hle->kheap_end, A0, A1); ret(m, 0); }
FN(c_change_clear_rcnt)
{
    uint32_t t = A0 & 3, old = m->hle->timer_autoack[t];
    m->hle->timer_autoack[t] = A1 != 0;
    ret(m, old);
}
FN(c_system_error) { halt(m, "SystemError (C(0Bh))"); }
FN(c_init_def_int)
{
    for (int i = 0; i < 11; i++)
        m->hle->irq_autoack[i] = false;
    enq_int(m, A0, hinfo(HI_IRQ));
    ret(m, 0);
}
FN(c_set_irq_autoack) { if (A0 < 11) m->hle->irq_autoack[A0] = A1 != 0; ret(m, 0); }
FN(c_adjust_a0) { ret(m, 0); }

/* clang-format off */
static const hle_call s_a[0xC0] = {
    [0x00] = {"open", a_open}, [0x01] = {"lseek", a_lseek}, [0x02] = {"read", a_read}, [0x03] = {"write", a_write},
    [0x04] = {"close", a_close}, [0x05] = {"ioctl", a_ioctl}, [0x06] = {"exit", a_exit}, [0x07] = {"isatty", a_isatty},
    [0x08] = {"getc", a_getc}, [0x09] = {"putc", a_putc}, [0x0A] = {"todigit", a_todigit},
    [0x0C] = {"strtoul", a_strtoul}, [0x0D] = {"strtol", a_strtol}, [0x0E] = {"abs", a_abs}, [0x0F] = {"labs", a_abs},
    [0x10] = {"atoi", a_atoi}, [0x11] = {"atol", a_atoi}, [0x12] = {"atob", a_atob}, [0x13] = {"setjmp", a_setjmp},
    [0x14] = {"longjmp", a_longjmp}, [0x15] = {"strcat", a_strcat}, [0x16] = {"strncat", a_strncat},
    [0x17] = {"strcmp", a_strcmp}, [0x18] = {"strncmp", a_strncmp}, [0x19] = {"strcpy", a_strcpy},
    [0x1A] = {"strncpy", a_strncpy}, [0x1B] = {"strlen", a_strlen}, [0x1C] = {"index", a_strchr},
    [0x1D] = {"rindex", a_strrchr}, [0x1E] = {"strchr", a_strchr}, [0x1F] = {"strrchr", a_strrchr},
    [0x20] = {"strpbrk", a_strpbrk}, [0x21] = {"strspn", a_strspn}, [0x22] = {"strcspn", a_strcspn},
    [0x23] = {"strtok", a_strtok}, [0x24] = {"strstr", a_strstr}, [0x25] = {"toupper", a_toupper},
    [0x26] = {"tolower", a_tolower}, [0x27] = {"bcopy", a_bcopy}, [0x28] = {"bzero", a_bzero}, [0x29] = {"bcmp", a_bcmp},
    [0x2A] = {"memcpy", a_memcpy}, [0x2B] = {"memset", a_memset}, [0x2C] = {"memmove", a_memmove},
    [0x2D] = {"memcmp", a_bcmp}, [0x2E] = {"memchr", a_memchr}, [0x2F] = {"rand", a_rand}, [0x30] = {"srand", a_srand},
    [0x31] = {"qsort", a_qsort}, [0x33] = {"malloc", a_malloc}, [0x34] = {"free", a_free},
    [0x35] = {"lsearch", a_lsearch}, [0x36] = {"bsearch", a_bsearch}, [0x37] = {"calloc", a_calloc},
    [0x38] = {"realloc", a_realloc}, [0x39] = {"InitHeap", a_initheap}, [0x3A] = {"_exit", a_exit},
    [0x3B] = {"getchar", a_getchar}, [0x3C] = {"putchar", a_putchar}, [0x3D] = {"gets", a_gets},
    [0x3E] = {"puts", a_puts}, [0x3F] = {"printf", a_printf}, [0x40] = {"SystemErrorUnresolvedException", a_unresolved},
    [0x41] = {"LoadTest", a_loadtest}, [0x42] = {"Load", a_load}, [0x43] = {"Exec", a_exec},
    [0x44] = {"FlushCache", a_flushcache}, [0x45] = {"init_a0_b0_c0_vectors", a_init_vectors},
    [0x46] = {"GPU_dw", a_gpu_dw}, [0x47] = {"gpu_send_dma", a_gpu_send_dma}, [0x48] = {"SendGP1Command", a_gpu_gp1},
    [0x49] = {"GPU_cw", a_gpu_cw}, [0x4A] = {"GPU_cwp", a_gpu_cwp}, [0x4B] = {"send_gpu_linked_list", a_gpu_list},
    [0x4C] = {"gpu_abort_dma", a_gpu_abort}, [0x4D] = {"GetGPUStatus", a_gpu_status}, [0x4E] = {"gpu_sync", a_gpu_sync},
    [0x51] = {"LoadExec", a_loadexec}, [0x54] = {"_96_init", a_one}, [0x55] = {"_bu_init", a_dummy},
    [0x56] = {"_96_remove", a_dummy}, [0x57] = {"return 0", a_dummy}, [0x58] = {"return 0", a_dummy},
    [0x59] = {"return 0", a_dummy}, [0x5A] = {"return 0", a_dummy},
    [0x70] = {"_bu_init", a_dummy}, [0x71] = {"_96_init", a_one}, [0x72] = {"_96_remove", a_dummy},
    [0x73] = {"return 0", a_dummy}, [0x74] = {"return 0", a_dummy}, [0x75] = {"return 0", a_dummy},
    [0x76] = {"return 0", a_dummy}, [0x77] = {"return 0", a_dummy},
    [0x96] = {"AddCDROMDevice", a_dummy}, [0x97] = {"AddMemCardDevice", a_dummy},
    [0x98] = {"AddDuartTtyDevice", a_dummy}, [0x99] = {"AddDummyTtyDevice", a_dummy},
    [0x9C] = {"SetConf", a_setconf}, [0x9D] = {"GetConf", a_getconf}, [0x9E] = {"SetCdromIrqAutoAbort", a_dummy},
    [0x9F] = {"SetMemSize", a_setmemsize}, [0xA4] = {"CdGetLbn", a_cdgetlbn}, [0xA5] = {"CdReadSector", a_cdreadsector},
    [0xA6] = {"CdGetStatus", a_cdgetstatus}, [0xAB] = {"_card_info", a_card_info},
    [0xAC] = {"_card_async_load_directory", a_card_load_dir}, [0xAD] = {"set_card_auto_format", a_dummy},
    [0xB4] = {"GetSystemInfo", a_getsysinfo},
};
static const hle_call s_b[0x60] = {
    [0x00] = {"alloc_kernel_memory", b_kmalloc}, [0x01] = {"free_kernel_memory", b_kfree},
    [0x02] = {"init_timer", b_init_timer}, [0x03] = {"get_timer", b_get_timer},
    [0x04] = {"enable_timer_irq", b_enable_timer_irq}, [0x05] = {"disable_timer_irq", b_disable_timer_irq},
    [0x06] = {"restart_timer", b_restart_timer}, [0x07] = {"DeliverEvent", b_deliver_event},
    [0x08] = {"OpenEvent", b_open_event}, [0x09] = {"CloseEvent", b_close_event}, [0x0A] = {"WaitEvent", b_wait_event},
    [0x0B] = {"TestEvent", b_test_event}, [0x0C] = {"EnableEvent", b_enable_event},
    [0x0D] = {"DisableEvent", b_disable_event}, [0x0E] = {"OpenTh", b_open_thread}, [0x0F] = {"CloseTh", b_close_thread},
    [0x10] = {"ChangeTh", b_change_thread}, [0x12] = {"InitPad", b_init_pad}, [0x13] = {"StartPad", b_start_pad},
    [0x14] = {"StopPad", b_stop_pad}, [0x15] = {"OutdatedPadInitAndStart", b_pad_init_hl},
    [0x16] = {"OutdatedPadGetButtons", b_pad_get_hl}, [0x17] = {"ReturnFromException", b_return_from_exception},
    [0x18] = {"SetDefaultExitFromException", b_set_default_exit},
    [0x19] = {"SetCustomExitFromException", b_set_custom_exit}, [0x20] = {"UnDeliverEvent", b_undeliver_event},
    [0x32] = {"open", a_open}, [0x33] = {"lseek", a_lseek}, [0x34] = {"read", a_read}, [0x35] = {"write", a_write},
    [0x36] = {"close", a_close}, [0x37] = {"ioctl", a_ioctl}, [0x38] = {"exit", a_exit}, [0x39] = {"isatty", a_isatty},
    [0x3A] = {"getc", a_getc}, [0x3B] = {"putc", a_putc}, [0x3C] = {"getchar", a_getchar},
    [0x3D] = {"putchar", a_putchar}, [0x3E] = {"gets", a_gets}, [0x3F] = {"puts", b_puts}, [0x40] = {"cd", b_cd},
    [0x41] = {"format", b_format}, [0x42] = {"firstfile", b_firstfile}, [0x43] = {"nextfile", b_nextfile},
    [0x44] = {"rename", b_rename}, [0x45] = {"erase", b_erase}, [0x46] = {"undelete", b_undelete},
    [0x47] = {"AddDrv", a_dummy}, [0x48] = {"DelDrv", a_dummy}, [0x49] = {"PrintInstalledDevices", a_dummy},
    [0x4A] = {"InitCard", b_init_card}, [0x4B] = {"StartCard", b_start_card}, [0x4C] = {"StopCard", b_stop_card},
    [0x4D] = {"_card_info_subfunc", a_card_info}, [0x4E] = {"write_card_sector", b_card_write},
    [0x4F] = {"read_card_sector", b_card_read}, [0x50] = {"allow_new_card", b_card_allow_new},
    [0x51] = {"Krom2RawAdd", b_krom2rawadd}, [0x54] = {"_get_errno", b_get_errno}, [0x55] = {"_get_error", b_get_error},
    [0x56] = {"GetC0Table", b_get_c0_table}, [0x57] = {"GetB0Table", b_get_b0_table}, [0x58] = {"_card_chan", b_card_chan},
    [0x5B] = {"ChangeClearPad", b_change_clear_pad}, [0x5C] = {"_card_status", b_card_status},
    [0x5D] = {"_card_wait", b_card_wait},
};
static const hle_call s_c[0x20] = {
    [0x00] = {"EnqueueTimerAndVblankIrqs", c_enq_rcnt}, [0x01] = {"EnqueueSyscallHandler", c_enq_syscall},
    [0x02] = {"SysEnqIntRP", c_sys_enq_int}, [0x03] = {"SysDeqIntRP", c_sys_deq_int},
    [0x04] = {"get_free_EvCB_slot", c_free_evcb}, [0x05] = {"get_free_TCB_slot", c_free_tcb},
    [0x06] = {"ExceptionHandler", c_exception_handler}, [0x07] = {"InstallExceptionHandlers", c_install_exc},
    [0x08] = {"SysInitMemory", c_sys_init_memory}, [0x0A] = {"ChangeClearRCnt", c_change_clear_rcnt},
    [0x0B] = {"SystemError", c_system_error}, [0x0C] = {"InitDefInt", c_init_def_int},
    [0x0D] = {"SetIrqAutoAck", c_set_irq_autoack}, [0x12] = {"InstallDevices", a_dummy},
    [0x13] = {"FlushStdInOutPut", a_dummy}, [0x1C] = {"AdjustA0Table", c_adjust_a0},
};
/* clang-format on */

static const hle_call *table_of(int table, int *n)
{
    switch (table) {
    case 0: *n = 0xC0; return s_a;
    case 1: *n = 0x60; return s_b;
    case 2: *n = 0x20; return s_c;
    default: *n = 0; return NULL;
    }
}

int ps1_hle_implemented(int table, int fn, const char **name)
{
    int n;
    const hle_call *t = table_of(table, &n);
    if (!t || fn < 0 || fn >= n || !t[fn].fn)
        return 0;
    if (name)
        *name = t[fn].name;
    return 1;
}

int ps1_hle_unimplemented_count(ps1_machine *m)
{
    return m->hle ? m->hle->unimpl : 0;
}

/* ---- The trap ---- */

static void call_fn(ps1_machine *m, int table, int fn)
{
    int n;
    const hle_call *t = table_of(table, &n);
    if (m->hle->trace)
        fprintf(stderr, "hle: %c(%02Xh) %s (%08X, %08X, %08X, %08X) from %08X\n", "ABC"[table], fn,
                fn < n && t[fn].name ? t[fn].name : "?", A0, A1, A2, A3, RA);
    if (fn < n && t[fn].fn)
        t[fn].fn(m);
    else
        unimplemented(m, table, fn);
}

static void kernel_entry(ps1_machine *m, int fn)
{
    switch (fn) {
    case K_SYSCALL_VERIFIER: syscall_verifier(m); break;
    case K_IRQ_VERIFIER: irq_verifier(m); break;
    case K_T0_VERIFIER: case K_T1_VERIFIER: case K_T2_VERIFIER: case K_T3_VERIFIER:
        timer_verifier(m, fn - K_T0_VERIFIER);
        break;
    case K_T0_HANDLER: case K_T1_HANDLER: case K_T2_HANDLER: case K_T3_HANDLER:
        timer_handler(m, fn - K_T0_HANDLER);
        break;
    case K_SIO0_VERIFIER: sio0_verifier(m); break;
    case K_SIO0_HANDLER: sio0_handler(m); break;
    case K_EXC_ENTER: exc_enter(m); break;
    case K_EXC_LEAVE: do_longjmp(m, rd32(m, K_VARS), 1); break;
    case K_GUEST_RET:
        if (m->hle->depth > 0) {
            m->hle->returned = true;
            m->hle->pc_set = true; /* the caller restores everything */
        }
        break;
    case K_EXEC_RET: exec_ret(m); break;
    default: break;
    }
}

void ps1_hle_trap(ps1_machine *m, uint32_t code)
{
    ps1_hle *h = m->hle;
    int table = (int)(code >> 8) & 0xFF, fn = (int)code & 0xFF;
    h->pc_set = false;
    m->cycles += 8;
    if (table == 4) {
        /* the A0 / B0 / C0 vectors: the entry the game's table holds for t1 */
        int t = fn;
        uint32_t idx = m->r[9] & 0xFF, n = t == 0 ? 0xC0 : t == 1 ? 0x60 : 0x20;
        uint32_t base = t == 0 ? PS1_HLE_ATAB : t == 1 ? PS1_HLE_BTAB : PS1_HLE_CTAB;
        if (m->r[9] >= n) {
            unimplemented(m, t, (int)(m->r[9] & 0xFF));
            set_pc(m, RA);
            return;
        }
        uint32_t entry = rd32(m, base + idx * 4);
        uint32_t ep = entry & 0x1FFFFFFFu;
        m->r[8] = 0xA0u + 0x10u * (uint32_t)t; /* t0, as the retail dispatchers leave it */
        if (ep >= PS1_HLE_STUBS && ep < PS1_HLE_STUBS + 0x400u * 3 && (ep & 3) == 0) {
            /* one of ours (perhaps moved to another slot): run it */
            uint32_t s = (ep - PS1_HLE_STUBS) / 4;
            table = (int)(s >> 8);
            fn = (int)(s & 0xFF);
            m->cur_pc = entry; /* a blocking call re-runs its stub */
        } else {
            set_pc(m, entry); /* the game's own function */
            return;
        }
    }
    if (table < 3)
        call_fn(m, table, fn);
    else
        kernel_entry(m, fn);
    if (h->depth == 0)
        h->escaped = false;
    if (!h->pc_set)
        set_pc(m, RA);
}

/* ---- Set-up and boot ---- */

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
{
    return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFFu);
}

static void write_exception_handler(ps1_machine *m)
{
    enum { ZERO = 0, V0 = 2, A0R = 4, S0 = 16, S1 = 17, S3 = 19, S4 = 20, S6 = 22, RAR = 31 };
    const uint32_t LW = 0x23, BEQ = 0x04, BNE = 0x05, ADDIU = 0x09;
    uint32_t code[] = {
        /* 0 */ PS1_HLE_TRAP | 0x300 | K_EXC_ENTER,
        /* 1 PL */ enc_i(LW, S3, S6, 0),
        /* 2 */ 0,
        /* 3 */ enc_i(BEQ, S6, ZERO, 21 - 4),
        /* 4 */ 0,
        /* 5 HL */ enc_i(LW, S6, S1, 8),
        /* 6 */ enc_i(LW, S6, S0, 4),
        /* 7 */ enc_i(BEQ, S1, ZERO, 17 - 8),
        /* 8 */ 0,
        /* 9 */ S1 << 21 | RAR << 11 | 0x09, /* jalr s1 */
        /* 10 */ 0,
        /* 11 */ enc_i(BEQ, V0, ZERO, 17 - 12),
        /* 12 */ 0,
        /* 13 */ enc_i(BEQ, S0, ZERO, 17 - 14),
        /* 14 */ V0 << 21 | ZERO << 16 | A0R << 11 | 0x21, /* move a0, v0 */
        /* 15 */ S0 << 21 | RAR << 11 | 0x09, /* jalr s0 */
        /* 16 */ 0,
        /* 17 NH */ enc_i(LW, S6, S6, 0),
        /* 18 */ 0,
        /* 19 */ enc_i(BNE, S6, ZERO, (uint32_t)(5 - 20)),
        /* 20 */ 0,
        /* 21 NP */ enc_i(ADDIU, S3, S3, 8),
        /* 22 */ enc_i(BNE, S4, S3, (uint32_t)(1 - 23)),
        /* 23 */ 0,
        /* 24 */ PS1_HLE_TRAP | 0x300 | K_EXC_LEAVE,
    };
    for (size_t i = 0; i < sizeof code / sizeof code[0]; i++)
        wr32(m, K_EXC + 4u * (uint32_t)i, code[i]);
}

static void kernel_setup(ps1_machine *m, uint32_t tcbs, uint32_t events)
{
    ps1_hle *h = m->hle;
    for (uint32_t a = 0; a < 0x10000u; a += 4)
        wr32(m, a, 0);
    wr32(m, K_RAMSIZE, 2);
    wr32(m, K_RAMSIZE + 4, 0);
    wr32(m, K_RAMSIZE + 8, 0xFF);
    /* the exception vector: lui k0, 0; addiu k0, k0, K_EXC; jr k0; nop */
    wr32(m, 0x80, 0x3C1A0000u);
    wr32(m, 0x84, 0x275A0000u | K_EXC);
    wr32(m, 0x88, 0x03400008u);
    wr32(m, 0x8C, 0);
    /* the A0 / B0 / C0 vectors: a dispatch trap (table 4), then nops */
    for (uint32_t t = 0; t < 3; t++)
        wr32(m, 0xA0 + 0x10 * t, PS1_HLE_TRAP | 0x400 | t);
    /* the stubs and the tables */
    for (int t = 0; t < 4; t++)
        for (int f = 0; f < 0x100; f++)
            wr32(m, stub_addr(t, f), PS1_HLE_TRAP | (uint32_t)(t << 8) | (uint32_t)f);
    for (int f = 0; f < 0xC0; f++)
        wr32(m, PS1_HLE_ATAB + 4u * (uint32_t)f, stub_addr(0, f));
    for (int f = 0; f < 0x60; f++)
        wr32(m, PS1_HLE_BTAB + 4u * (uint32_t)f, stub_addr(1, f));
    for (int f = 0; f < 0x20; f++)
        wr32(m, PS1_HLE_CTAB + 4u * (uint32_t)f, stub_addr(2, f));
    write_exception_handler(m);
    wr32(m, K_IDLE, 0x1000FFFFu); /* b . */
    wr32(m, K_IDLE + 4, 0);
    /* the default exception jmp_buf: ReturnFromException on the exception stack */
    wr32(m, K_JMPBUF, stub_addr(1, 0x17));
    wr32(m, K_JMPBUF + 4, K_EXSTACK);
    wr32(m, K_VARS, 0x80000000u | K_JMPBUF);
    /* the built-in handlers */
    static const struct { int hi, verifier, handler; } hs[HI_COUNT] = {
        {HI_SYSCALL, K_SYSCALL_VERIFIER, -1}, {HI_IRQ, K_IRQ_VERIFIER, -1},
        {HI_T0, K_T0_VERIFIER, K_T0_HANDLER}, {HI_T1, K_T1_VERIFIER, K_T1_HANDLER},
        {HI_T2, K_T2_VERIFIER, K_T2_HANDLER}, {HI_T3, K_T3_VERIFIER, K_T3_HANDLER},
        {HI_SIO0, K_SIO0_VERIFIER, K_SIO0_HANDLER},
    };
    for (int i = 0; i < HI_COUNT; i++) {
        uint32_t a = hinfo(hs[i].hi);
        wr32(m, a + 4, hs[i].handler >= 0 ? stub_addr(3, hs[i].handler) : 0);
        wr32(m, a + 8, stub_addr(3, hs[i].verifier));
    }
    /* the table of tables, the handlers, the kernel objects (OpenBIOS's kernelSetup) */
    wr32(m, K_TOT, 0x80000000u | K_HARRAY);
    wr32(m, K_TOT + 4, 4 * 8);
    heap_init(m, &h->kheap_start, &h->kheap_end, 0x80000000u | K_KHEAP, K_KHEAP_SIZE);
    enq_int(m, 0, hinfo(HI_SYSCALL));
    for (int i = 0; i < 11; i++)
        h->irq_autoack[i] = false;
    enq_int(m, 3, hinfo(HI_IRQ));
    init_events(m, events);
    init_threads(m, tcbs);
    for (int i = 0; i < 4; i++) {
        h->timer_autoack[i] = true;
        enq_int(m, 1, hinfo(HI_T0 + i));
    }
    h->conf_tcb = h->tcb_count;
    h->conf_ev = h->evcb_count;
    h->conf_stack = 0x801FFF00u;
    h->rand_next = 1;
    h->files[0].dev = DEV_TTY;
    h->files[1].dev = DEV_TTY;
    m->i_mask = 0;
    m->i_stat = 0;
    m->cop0[C0_SR] = 0x40000000u; /* COP2 on, interrupts off: where OpenBIOS hands over to a program */
    ps1_cpu_update_irq(m);
}

/* SYSTEM.CNF's word item (TCB / EVENT / STACK, hex) and BOOT path. */
static bool cnf_word(const char *cnf, const char *key, uint32_t *v)
{
    size_t kl = strlen(key);
    for (const char *p = cnf; *p; p++) {
        if ((p == cnf || p[-1] == '\n' || p[-1] == '\r') && !strncmp(p, key, kl)) {
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != '=')
                continue;
            q++;
            *v = (uint32_t)strtoul(q, NULL, 16);
            return true;
        }
    }
    return false;
}

static bool cnf_boot(const char *cnf, char *out, size_t n, char *args, size_t an)
{
    for (const char *p = cnf; *p; p++) {
        if ((p == cnf || p[-1] == '\n' || p[-1] == '\r') && !strncmp(p, "BOOT", 4)) {
            const char *q = p + 4;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != '=')
                continue;
            q++;
            while (*q == ' ' || *q == '\t')
                q++;
            size_t k = 0;
            while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n' && k + 1 < n)
                out[k++] = *q++;
            out[k] = 0;
            k = 0;
            while (*q == ' ' || *q == '\t')
                q++;
            while (*q && *q != '\r' && *q != '\n' && k + 1 < an)
                args[k++] = *q++;
            args[k] = 0;
            return out[0] != 0;
        }
    }
    return false;
}

/* Reads a whole cdrom: file into a fresh buffer. */
static uint8_t *cd_read_file(ps1_machine *m, const char *path, size_t *len)
{
    uint32_t lba, size;
    bool dir;
    if (cd_find(m, path, &lba, &size, &dir) != 0 || dir || size > 4u * 1024 * 1024)
        return NULL;
    uint8_t *b = malloc(size + 1);
    if (!b)
        return NULL;
    for (uint32_t off = 0; off < size; off += 2048) {
        uint8_t sec[2048];
        if (cd_sector(m, lba + off / 2048, sec) != 0) {
            free(b);
            return NULL;
        }
        memcpy(b + off, sec, size - off < 2048 ? size - off : 2048);
    }
    b[size] = 0;
    *len = size;
    return b;
}

void ps1_hle_boot(ps1_machine *m)
{
    uint32_t tcbs = 4, events = 16, stack = 0x801FFF00u;
    bool have_stack = false;
    if (!m->exe && m->disc) {
        size_t n = 0;
        char boot[128] = "", args[128] = "";
        uint8_t *cnf = cd_read_file(m, "SYSTEM.CNF", &n);
        if (cnf) {
            cnf_word((const char *)cnf, "TCB", &tcbs);
            cnf_word((const char *)cnf, "EVENT", &events);
            have_stack = cnf_word((const char *)cnf, "STACK", &stack);
            if (!cnf_boot((const char *)cnf, boot, sizeof boot, args, sizeof args))
                boot[0] = 0;
            free(cnf);
        }
        const char *path = boot[0] ? boot : "PSX.EXE";
        const char *colon = strchr(path, ':');
        if (colon)
            path = colon + 1;
        uint8_t *exe = cd_read_file(m, path, &n);
        if (exe && n >= 0x800 && !memcmp(exe, "PS-X EXE", 8)) {
            m->exe = exe;
            m->exe_size = n;
        } else {
            free(exe);
            fprintf(stderr, "ps1: HLE BIOS: no boot file on the disc (%s)\n", path);
        }
        kernel_setup(m, tcbs, events);
        for (size_t i = 0; args[i] && i < 0x7F; i++)
            wr8(m, K_CMDLINE + (uint32_t)i, (uint8_t)args[i]);
    } else {
        kernel_setup(m, tcbs, events);
    }
    m->shell_skipped = true;
    if (m->exe) {
        m->r[29] = m->r[30] = have_stack ? stack : 0x801FFF00u;
        ps1_boot_exe(m);
    } else {
        m->pc = 0x80000000u | K_IDLE;
        m->next_pc = m->pc + 4;
        m->exe_started = true;
    }
}

int ps1_hle_create(ps1_machine *m)
{
    m->hle = calloc(1, sizeof *m->hle);
    if (!m->hle)
        return -1;
    const char *t = getenv("PS1_HLE_TRACE");
    m->hle->trace = t && *t && *t != '0';
    return 0;
}

void ps1_hle_destroy(ps1_machine *m)
{
    free(m->hle);
    m->hle = NULL;
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

/* The HLE BIOS's own state (its kernel objects, files, heaps, libc state); 0 bytes with a BIOS image. */
size_t ps1_hle_state_size(ps1_machine *m)
{
    return m->hle ? sizeof(struct ps1_hle) : 0;
}

int ps1_hle_state_busy(ps1_machine *m)
{
    return m->hle && m->hle->depth != 0;
}

void ps1_hle_state_save(ps1_machine *m, uint8_t *out)
{
    if (!m->hle)
        return;
    struct ps1_hle c = *m->hle;
    c.trace = false;
    memcpy(out, &c, sizeof c);
}

/* No guest call from C in progress (save refuses to take one), the kernel object counts within the tables, each open
 * file's device (anything else is read as a card file), a card file's first block and nextfile's next block within
 * the card's 15 (card frames are reached from them), the firstfile pattern terminated. */
int ps1_hle_state_valid(ps1_machine *m, const uint8_t *in, size_t n)
{
    if (n != ps1_hle_state_size(m))
        return -1;
    if (!m->hle)
        return 0;
    struct ps1_hle h;
    memcpy(&h, in, sizeof h);
    if (h.depth != 0 || h.tcb_count > TCB_MAX || h.evcb_count > EVCB_MAX)
        return -2;
    for (int i = 0; i < NFILES; i++)
        if (h.files[i].dev < DEV_NONE || h.files[i].dev > DEV_BU || (h.files[i].dev == DEV_BU && h.files[i].first > 14))
            return -2;
    if (h.find_dev < DEV_NONE || h.find_dev > DEV_BU || (h.find_dev == DEV_BU && h.find_pos > 15) ||
        !memchr(h.find_pat, 0, sizeof h.find_pat))
        return -2;
    return 0;
}

int ps1_hle_state_load(ps1_machine *m, const uint8_t *in, size_t n)
{
    if (n != ps1_hle_state_size(m))
        return -1;
    if (!m->hle)
        return 0;
    bool trace = m->hle->trace;
    memcpy(m->hle, in, sizeof *m->hle);
    m->hle->trace = trace;
    return 0;
}
