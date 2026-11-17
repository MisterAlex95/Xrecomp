/* ps1_cdrom.c -- the PlayStation CD-ROM controller (P-9, XBA-167): the command / response interface the CPU sees
 * at 0x1F801800..3, the drive behind it (seek, read, play), data sector buffering for DMA3, XA-ADPCM decoding and
 * CD-DA playback into a 44100 Hz stream with the CD audio volume matrix. Behaviour from psx-spx « CDROM Drive »;
 * our own code (ideas from ares, ISC).
 *
 * Interface. Register 0 (index / status) selects one of four banks for registers 1..3. Writes: command (1.0),
 * parameter FIFO (2.0, 16 bytes), request (3.0: bit 7 loads the current sector into the data FIFO, 0 clears it),
 * interrupt enable (2.1), interrupt flag acknowledge (3.1: bits 0..4 clear the flag, bit 6 clears the parameter
 * FIFO), the four CD-to-SPU volumes (2.2 L->L, 3.2 L->R, 1.3 R->R, 2.3 R->L) applied by 3.3 bit 5 (3.3 bit 0 mutes
 * XA-ADPCM), 1.1 / 1.2 (sound map, ignored). Reads: response FIFO (1.x, 16 bytes, wraps), data FIFO (2.x), IE (3.0 /
 * 3.2) and IF (3.1 / 3.3), bits 5..7 read as 1.
 *
 * Sequencing. A command's first response (INT3, or INT5 on an error) comes `first` cycles after it is written; some
 * commands schedule a second response (INT2, or INT5) later, and reads / play produce INT1 (data ready / report)
 * and INT4 (data end). Responses queue: a new one is only delivered once the CPU has acknowledged the previous one
 * (IF & 7 == 0), and it replaces the response FIFO. The IRQ line (I_STAT bit 2) is raised on a delivery whose type
 * is enabled in IE, or when IE is written enabling a pending one.
 *
 * Times (CPU cycles at 33.8688 MHz). Sector: 451584 (1x, 75 sectors/s) or 225792 (2x). First responses, GetID and
 * Stop second responses: the averages psx-spx « Response Timings » measured (see the defines); Init's and Pause's
 * second responses from the hardware log of ps1-tests cdrom/timing (Pause about 1.01 M cycles at 1x, 1.04 M at 2x,
 * not psx-spx's figures); MotorOn 0.1 s when spinning up; ReadTOC / SetSession 0.5 s. Seeks: a settling time (160000
 * cycles at 1x, 470000 at 2x) + 20000 + 1/4 sector time per 75 sectors of distance, capped at 0.5 s: fitted to the
 * same log's ReadN-to-first-sector times after a 100-sector seek (about 0.78 M cycles at 1x, 0.87 M at 2x); real seek
 * times depend on where the head is, so this stays an approximation.
 *
 * GetID: a data disc always answers as a licensed one (INT2: 02 00 20 00 'S' 'C' 'E' x), x from the licence text
 * at LBA 4 ("...Inc" I, "...Amer" A, "...Euro" E), 'A' when there is none (our homebrew): what a console with a
 * modchip returns, so a BIOS boots any disc; the core's fast boot does not need it. An audio disc gives INT5
 * (0A 90 00 00 ...), no disc INT5 (08 40 00 00 ...).
 *
 * Audio. CD-DA (Play, or ReadN with Setmode bit 0 on an audio track) and XA-ADPCM real-time sectors (Setmode bit 6;
 * Setfilter's file / channel when Setmode bit 3) go through the volume matrix into a ring of 44100 Hz stereo frames
 * the SPU pulls. XA is resampled from 37800 Hz with psx-spx's 25-point zigzag interpolation (18900 Hz: each sample fed
 * twice), always at normal speed. CD-DA follows the drive: at 2x (Setmode bit 7) sectors come twice as fast and play
 * at double pitch (each pair of frames averaged into one), as on the hardware. Report mode sends INT1 every 10
 * sectors (absolute time on even tens, relative time with bit 7 of the seconds on odd tens) with the peak level of
 * the samples played since the last report (bits 0..14 the highest magnitude, bit 15 the channel: left on absolute
 * reports, right on relative ones).
 *
 * Position (sub-channel Q). GetlocP and the reports describe the sector last under the head: its track (the track
 * whose INDEX 00 it follows, so a pregap belongs to the track it precedes; AAh past the end, the lead-out), index (00
 * in a pregap, where the relative time counts down to INDEX 01), relative and absolute times. GetQ (1Dh adr point)
 * answers the lead-in's TOC entries: a track's start, A0h / A1h the first / last track, A2h the lead-out.
 *
 * Lid (ps1_cdrom_set_lid, ps1_cdrom_set_disc: multi-disc games). Opening it stops the drive and the motor, sets stat
 * bit 4 and sends INT5 (01h, 08h: the ps1-tests cdrom/disc-swap hardware log); commands that need a disc then fail
 * (INT5 stat | 01h, 80h). Closing it spins the motor up on the new disc; bit 4 stays set until a Getstat reads it, and
 * GetlocL fails until a sector is read. After a seek past the end (INT5 04h), GetlocL / GetlocP fail too (the
 * cdrom/getloc log) until the next seek or Init.
 *
 * Not modelled: the drive's sector buffers (the hardware keeps more than the two sectors here: a sector the CPU did
 * not take before the next one arrives is lost, as on a real overrun, but no older sector can be re-read), the seek
 * of the head after a Pause (it stays on the last sector), audio sub-channels (P, R..W) and CD+G. */
#include "ps1_devices.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_1X 451584u /* PS1_CPU_HZ * 930h / 4 / 44100 */
/* psx-spx « CDROM - Response Timings » (averages measured on a PAL PSone) */
#define FIRST_DELAY 0xC4E1u         /* first response, motor on */
#define FIRST_DELAY_STOPPED 0x5CF4u /* first response, motor off */
#define INIT_DELAY 0x13CCEu         /* Init's first response */
#define INIT_SECOND 476300u         /* Init's second response (ps1-tests cdrom/timing hardware log, average) */
#define GETID_DELAY 0x4A00u         /* GetID's second response */
/* Pause while reading / playing: psx-spx gives 21181Ch / 10BD93h, the hardware log of ps1-tests cdrom/timing averages
 * 1011910 / 1036808 cycles (its 16-bit timer counts 8-cycle ticks and one overflow: below 1 M cycles readings are
 * exact, 10BD93h would read as 572 K, which no run shows). */
#define PAUSE_DELAY_1X 1011910u
#define PAUSE_DELAY_2X 1036808u
#define PAUSE_DELAY_IDLE 0x1DF2u
#define STOP_DELAY_1X 0xD38ACAu     /* Stop while spinning */
#define STOP_DELAY_2X 0x18A6076u
#define STOP_DELAY_IDLE 0x1D7Bu
#define TOC_DELAY (PS1_CPU_HZ / 2)  /* ReadTOC / SetSession: "about 1 second" in psx-spx, shortened */
#define SPINUP_DELAY (PS1_CPU_HZ / 10)
#define SEEK_MAX (PS1_CPU_HZ / 2)
#define SEEK_SETTLE_1X 160000u  /* the head settling on the track (fitted to the cdrom/timing hardware log) */
#define SEEK_SETTLE_2X 470000u

#define AUDIO_RING 32768u /* frames, power of two: 0.74 s */
#define QUEUE 8

enum {
    ST_ERROR = 0x01,
    ST_MOTOR = 0x02,
    ST_SEEKERR = 0x04,
    ST_IDERR = 0x08,
    ST_SHELL = 0x10,
    ST_READ = 0x20,
    ST_SEEK = 0x40,
    ST_PLAY = 0x80,
};

enum { MODE_CDDA = 0x01, MODE_AUTOPAUSE = 0x02, MODE_REPORT = 0x04, MODE_FILTER = 0x08, MODE_SIZE = 0x20,
       MODE_XA = 0x40, MODE_SPEED = 0x80 };

enum { DRIVE_IDLE, DRIVE_SEEK, DRIVE_READ, DRIVE_PLAY };

typedef struct response {
    uint8_t type; /* 1..5 */
    uint8_t len;
    uint8_t data[16];
    uint8_t sector; /* INT1 for a data sector: deliver loads the pending sector */
} response;

typedef struct xa_chan {
    int32_t old, older;
} xa_chan;

struct ps1_cdrom {
    ps1_irq_sink irq;
    ps1_disc *disc;

    uint8_t index;
    uint8_t ie, iflag;
    uint8_t params[16];
    uint8_t nparams;
    uint8_t resp[16];
    uint8_t resp_len, resp_pos;
    uint8_t busy;
    uint8_t request;

    /* command in flight: first response */
    uint8_t cmd;
    uint8_t cmd_params[16];
    uint8_t cmd_nparams;
    uint8_t cmd_pending;
    int64_t cmd_timer;
    /* second response */
    uint8_t second_cmd;
    uint8_t second_pending;
    int64_t second_timer;

    /* queued responses */
    response queue[QUEUE];
    int qhead, qcount;

    /* drive */
    uint8_t stat;
    uint8_t mode;
    uint8_t filter_file, filter_chan;
    uint8_t muted;
    int drive;           /* DRIVE_* */
    int after_seek;      /* DRIVE_READ / DRIVE_PLAY / DRIVE_IDLE once the seek completes */
    int64_t drive_timer;
    uint32_t lba;        /* next sector to read / play */
    uint32_t setloc;     /* target of the last Setloc */
    uint8_t setloc_pending;
    uint8_t play_track;  /* track being played (autopause / report) */
    uint32_t report_count;
    uint8_t last_header[8]; /* header (4) + subheader (4) of the last sector read (GetlocL) */
    uint32_t last_lba;
    uint8_t have_read;
    uint32_t resume_lba; /* where a Read without a new Setloc starts: the last sector received, or the last seek */
    uint8_t license; /* 'A' / 'E' / 'I' */
    uint8_t getq_point; /* GetQ's point, answered by its second response */
    uint8_t seek_failed; /* the last seek went past the disc: GetlocL / GetlocP fail until the next seek or Init */
    uint8_t lid_open; /* the lid is open now (stat bit 4 is the latch: it stays set until a Getstat after closing) */
    uint32_t q_lba;   /* the sector last under the head: GetlocP, reports */
    uint16_t peak[2]; /* CD-DA peak magnitude per channel since the last report */
    uint8_t peak_odd; /* the next report's channel: 0 left, 1 right */
    int32_t cdda_half[2]; /* 2x CD-DA: the first frame of a pair waiting for its second */
    uint8_t cdda_have_half;

    /* sector buffers: pending (read, INT1 not yet delivered) and current (delivered, loaded by the request bit) */
    uint8_t pending_sector[2352];
    uint8_t current_sector[2352];
    uint8_t data[2352];
    uint32_t data_len, data_pos;
    uint8_t sector_fresh; /* a sector was delivered (INT1) since the data FIFO was last loaded */

    /* audio */
    uint8_t vol[4];      /* applied: L->L, L->R, R->R, R->L (0x80 = 100 %) */
    uint8_t vol_next[4];
    uint8_t xa_mute;     /* 3.3 bit 0 */
    xa_chan xa[2];
    int16_t xa_ring[2][32]; /* the last 37800 Hz samples (zigzag interpolation) */
    uint32_t xa_p;
    int xa_sixstep;
    int16_t ring[AUDIO_RING * 2];
    uint32_t ring_head, ring_count;
    uint32_t xa_play_frames; /* frames of XA still in the ring (ADPBUSY) */
};

/* ---- helpers ------------------------------------------------------------------------------------------------ */

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int from_bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static int bcd_ok(uint8_t b) { return (b & 15) < 10 && (b >> 4) < 10; }

static uint32_t sector_cycles(const ps1_cdrom *c) { return (c->mode & MODE_SPEED) ? SECTOR_1X / 2 : SECTOR_1X; }

static uint32_t seek_cycles(const ps1_cdrom *c, uint32_t to)
{
    uint32_t d = to > c->lba ? to - c->lba : c->lba - to;
    uint64_t t = ((c->mode & MODE_SPEED) ? SEEK_SETTLE_2X : SEEK_SETTLE_1X) + 20000u +
                 (uint64_t)d * (SECTOR_1X / 4) / 75u;
    return t > SEEK_MAX ? SEEK_MAX : (uint32_t)t;
}

static void raise_if_pending(ps1_cdrom *c)
{
    if (c->iflag & c->ie & 0x1F)
        c->irq.raise(c->irq.ctx, PS1_IRQ_CDROM);
}

static void push(ps1_cdrom *c, int type, const uint8_t *data, int len, int sector)
{
    if (c->qcount == QUEUE) {
        /* full: drop the oldest INT1 (the drive overran its buffers), else the oldest */
        int drop = 0;
        for (int i = 0; i < c->qcount; i++)
            if (c->queue[(c->qhead + i) % QUEUE].type == 1) {
                drop = i;
                break;
            }
        for (int i = drop; i + 1 < c->qcount; i++)
            c->queue[(c->qhead + i) % QUEUE] = c->queue[(c->qhead + i + 1) % QUEUE];
        c->qcount--;
    }
    response *r = &c->queue[(c->qhead + c->qcount) % QUEUE];
    r->type = (uint8_t)type;
    r->len = (uint8_t)len;
    r->sector = (uint8_t)sector;
    memcpy(r->data, data, (size_t)len);
    c->qcount++;
}

static void push1(ps1_cdrom *c, int type, uint8_t b) { push(c, type, &b, 1, 0); }

static void error(ps1_cdrom *c, uint8_t code)
{
    uint8_t r[2] = {(uint8_t)(c->stat | ST_ERROR), code};
    push(c, 5, r, 2, 0);
}

/* Drops queued INT1s (reading stopped). */
static void drop_int1(ps1_cdrom *c)
{
    int n = 0;
    for (int i = 0; i < c->qcount; i++) {
        response r = c->queue[(c->qhead + i) % QUEUE];
        if (r.type != 1)
            c->queue[(c->qhead + n++) % QUEUE] = r;
    }
    c->qcount = n;
}

static void deliver(ps1_cdrom *c)
{
    if (!c->qcount || (c->iflag & 7))
        return;
    response *r = &c->queue[c->qhead];
    c->qhead = (c->qhead + 1) % QUEUE;
    c->qcount--;
    memcpy(c->resp, r->data, r->len);
    memset(c->resp + r->len, 0, 16 - r->len);
    c->resp_len = r->len;
    c->resp_pos = 0;
    if (r->sector) {
        memcpy(c->current_sector, c->pending_sector, sizeof c->current_sector);
        c->sector_fresh = 1;
    }
    c->iflag = (uint8_t)((c->iflag & ~7) | r->type);
    if (r->type == 3 || r->type == 5)
        c->busy = 0;
    raise_if_pending(c);
}

/* Audio ring -------------------------------------------------------------------------------------------------- */

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void ring_put(ps1_cdrom *c, int32_t l, int32_t r)
{
    int16_t ol = clamp16((l * c->vol[0] + r * c->vol[3]) >> 7);
    int16_t or_ = clamp16((l * c->vol[1] + r * c->vol[2]) >> 7);
    if (c->muted)
        ol = or_ = 0;
    if (c->ring_count == AUDIO_RING) {
        c->ring_head = (c->ring_head + 1) & (AUDIO_RING - 1);
        c->ring_count--;
    }
    uint32_t i = (c->ring_head + c->ring_count) & (AUDIO_RING - 1);
    c->ring[2 * i] = ol;
    c->ring[2 * i + 1] = or_;
    c->ring_count++;
}

/* ---- XA-ADPCM ----------------------------------------------------------------------------------------------- */

static const int8_t xa_k0[4] = {0, 60, 115, 98};
static const int8_t xa_k1[4] = {0, 0, -52, -55};

static int16_t xa_sample(xa_chan *ch, int32_t raw, int shift, int filter)
{
    int32_t s = raw >> shift; /* raw: the nibble / byte in the top bits of an int16 */
    s += (ch->old * xa_k0[filter] + ch->older * xa_k1[filter] + 32) >> 6;
    int16_t v = clamp16(s);
    ch->older = ch->old;
    ch->old = v;
    return v;
}

/* Decodes one sector's 18 sound groups to `out` (interleaved when stereo); returns the sample count. */
static int xa_decode(ps1_cdrom *c, const uint8_t *sector, int16_t *out)
{
    uint8_t coding = sector[19];
    int stereo = (coding & 3) == 1;
    int bits8 = ((coding >> 4) & 3) == 1;
    int n = 0;
    for (int g = 0; g < 18; g++) {
        const uint8_t *grp = sector + 24 + g * 128;
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            uint8_t hdr = grp[4 + u];
            int shift = hdr & 15;
            if (shift > 12)
                shift = 9;
            int filter = (hdr >> 4) & 3;
            xa_chan *ch = &c->xa[stereo ? (u & 1) : 0];
            for (int i = 0; i < 28; i++) {
                int32_t raw;
                if (bits8) {
                    raw = (int16_t)(grp[16 + i * 4 + u] << 8);
                } else {
                    uint8_t b = grp[16 + i * 4 + (u >> 1)];
                    raw = (int16_t)(((u & 1) ? (b >> 4) : (b & 15)) << 12);
                }
                int16_t v = xa_sample(ch, raw, shift, filter);
                if (stereo) /* units alternate L, R: the pair (u, u+1) holds 28 frames */
                    out[n + (u >> 1) * 56 + i * 2 + (u & 1)] = v;
                else
                    out[n + u * 28 + i] = v;
            }
        }
        n += units * 28;
    }
    return n;
}

/* psx-spx « 25-point Zigzag Interpolation »: seven tables of 29 taps turn every six 37800 Hz samples into seven
 * 44100 Hz ones (hardware data transcribed from psx-spx; it notes small rounding differences with the real chip). */
static const int16_t zigzag[7][29] = {
    {0, 0, 0, 0, 0, -2, 10, -34, 65, -84, 52, 9, -266, 1024, -2680, 9036, 26516, -6016, 3021, -1571, 848, -365, 107, 10, -16, 17, -8, 3, -1},
    {0, 0, 0, -2, 0, 3, -19, 60, -75, 162, -227, 306, -67, -615, 3229, 29883, -4532, 2488, -1471, 882, -424, 166, -27, 5, 6, -8, 3, -1, 0},
    {0, 0, -1, 3, -2, -5, 31, -74, 179, -402, 689, -926, 1272, -1446, 31033, -1446, 1272, -926, 689, -402, 179, -74, 31, -5, -2, 3, -1, 0, 0},
    {0, -1, 3, -8, 6, 5, -27, 166, -424, 882, -1471, 2488, -4532, 29883, 3229, -615, -67, 306, -227, 162, -75, 60, -19, 3, 0, -2, 0, 0, 0},
    {-1, 3, -8, 17, -16, 10, 107, -365, 848, -1571, 3021, -6016, 26516, 9036, -2680, 1024, -266, 9, 52, -84, 65, -34, 10, -1, 0, 1, 0, 0, 0},
    {2, -8, 16, -35, 43, 26, -235, 635, -1352, 2810, -5882, 21472, 15367, -4681, 2062, -839, 347, -68, -23, 70, -35, 17, -5, 0, 0, 0, 0, 0, 0},
    {-5, 17, -35, 70, -23, -68, 347, -839, 2062, -4681, 15367, 21472, -5882, 2810, -1352, 635, -235, 26, 43, -35, 16, -8, 2, 0, 0, 0, 0, 0, 0},
};

static void xa_out37800(ps1_cdrom *c, int16_t l, int16_t r)
{
    c->xa_ring[0][c->xa_p & 31] = l;
    c->xa_ring[1][c->xa_p & 31] = r;
    c->xa_p++;
    if (--c->xa_sixstep > 0)
        return;
    c->xa_sixstep = 6;
    for (int t = 0; t < 7; t++) {
        int32_t sum[2] = {0, 0};
        for (int ch = 0; ch < 2; ch++)
            for (int i = 1; i <= 29; i++)
                sum[ch] += (c->xa_ring[ch][(c->xa_p - (uint32_t)i) & 31] * zigzag[t][i - 1]) / 0x8000;
        ring_put(c, clamp16(sum[0]), clamp16(sum[1]));
        c->xa_play_frames++;
    }
}

static void xa_play(ps1_cdrom *c, const uint8_t *sector)
{
    int16_t pcm[18 * 8 * 28];
    int n = xa_decode(c, sector, pcm);
    uint8_t coding = sector[19];
    int stereo = (coding & 3) == 1;
    int half = ((coding >> 2) & 3) == 1; /* 18900 Hz: each sample twice through the 37800 Hz path */
    int frames = stereo ? n / 2 : n;
    if (c->xa_mute) {
        /* muted by 3.3 bit 0: decoded (filters keep their history), silent */
        memset(pcm, 0, sizeof pcm);
    }
    for (int i = 0; i < frames; i++) {
        int16_t l = stereo ? pcm[2 * i] : pcm[i];
        int16_t r = stereo ? pcm[2 * i + 1] : pcm[i];
        xa_out37800(c, l, r);
        if (half)
            xa_out37800(c, l, r);
    }
}

/* ---- drive -------------------------------------------------------------------------------------------------- */

static int find_track(ps1_cdrom *c, uint32_t lba, uint32_t *start, int *audio)
{
    int n = ps1_disc_track_count(c->disc), t = 1;
    for (int i = 1; i <= n; i++) {
        uint32_t s;
        int a;
        if (ps1_disc_track(c->disc, i, &s, &a) == 0 && s <= lba)
            t = i;
    }
    ps1_disc_track(c->disc, t, start, audio);
    return t;
}

static void start_seek(ps1_cdrom *c, int then)
{
    uint32_t to = c->setloc_pending ? c->setloc : c->lba;
    c->drive = DRIVE_SEEK;
    c->after_seek = then;
    c->drive_timer = seek_cycles(c, to);
    c->lba = to;
    c->resume_lba = to;
    c->setloc_pending = 0;
    c->seek_failed = 0;
    c->stat = (uint8_t)((c->stat & ~(ST_READ | ST_PLAY)) | ST_SEEK | ST_MOTOR);
}

static void stop_drive(ps1_cdrom *c)
{
    c->drive = DRIVE_IDLE;
    c->stat &= (uint8_t)~(ST_READ | ST_PLAY | ST_SEEK);
}

/* Sub-channel Q of sector lba: track (BCD, AAh in the lead-out), index, relative and absolute times. */
typedef struct qpos {
    uint8_t track, index;
    uint32_t rel, abs;
} qpos;

static qpos q_position(ps1_cdrom *c, uint32_t lba)
{
    qpos q = {1, 1, lba, lba + 150};
    int n = ps1_disc_track_count(c->disc);
    uint32_t size = ps1_disc_size(c->disc);
    if (n <= 0)
        return q;
    if (lba >= size) {
        q.track = 0xAA;
        q.rel = lba - size;
        return q;
    }
    int t = 1;
    for (int i = 1; i <= n; i++) {
        uint32_t b;
        if (ps1_disc_track_begin(c->disc, i, &b) == 0 && b <= lba)
            t = i;
    }
    uint32_t start = 0;
    ps1_disc_track(c->disc, t, &start, NULL);
    q.track = bcd((unsigned)t);
    q.index = lba >= start ? 1 : 0;
    q.rel = lba >= start ? lba - start : start - lba;
    return q;
}

static void report(ps1_cdrom *c)
{
    uint32_t lba = c->q_lba;
    uint32_t a = lba + 150;
    int sect = (int)(a % 75);
    if (sect % 10)
        return;
    qpos q = q_position(c, lba);
    uint8_t r[8];
    r[0] = c->stat;
    r[1] = q.track;
    r[2] = q.index;
    int rel = (sect / 10) % 2 != 0;
    uint32_t t = rel ? q.rel : a;
    r[3] = bcd(t / 4500);
    r[4] = (uint8_t)(bcd((t / 75) % 60) | (rel ? 0x80 : 0));
    r[5] = bcd(t % 75);
    /* peak: left on absolute reports, right (bit 15) on relative ones; both start over */
    uint16_t pk = (uint16_t)((c->peak[rel] > 0x7FFF ? 0x7FFF : c->peak[rel]) | (rel ? 0x8000 : 0));
    r[6] = (uint8_t)pk;
    r[7] = (uint8_t)(pk >> 8);
    c->peak[0] = c->peak[1] = 0;
    push(c, 1, r, 8, 0);
}

static void cdda_frame(ps1_cdrom *c, int32_t l, int32_t r)
{
    uint16_t ml = (uint16_t)(l < 0 ? -l : l), mr = (uint16_t)(r < 0 ? -r : r);
    if (ml > c->peak[0])
        c->peak[0] = ml;
    if (mr > c->peak[1])
        c->peak[1] = mr;
    ring_put(c, l, r);
}

/* One CD-DA sector into the ring: 588 frames, or at 2x 294 (each pair averaged: double speed, double pitch). */
static void cdda_sector(ps1_cdrom *c, const uint8_t *s)
{
    int fast = (c->mode & MODE_SPEED) != 0;
    for (int i = 0; i < 588; i++) {
        int32_t l = (int16_t)(s[4 * i] | (s[4 * i + 1] << 8));
        int32_t r = (int16_t)(s[4 * i + 2] | (s[4 * i + 3] << 8));
        if (!fast) {
            cdda_frame(c, l, r);
        } else if (!c->cdda_have_half) {
            c->cdda_half[0] = l;
            c->cdda_half[1] = r;
            c->cdda_have_half = 1;
        } else {
            cdda_frame(c, (c->cdda_half[0] + l) >> 1, (c->cdda_half[1] + r) >> 1);
            c->cdda_have_half = 0;
        }
    }
}

/* One sector period of reading or playing. */
static void drive_step(ps1_cdrom *c)
{
    uint8_t s[2352];
    if (!c->disc || c->lba >= ps1_disc_size(c->disc) || ps1_disc_read_sector(c->disc, c->lba, s) != 0) {
        /* end of the disc: data end, the motor stops after Play */
        if (c->drive == DRIVE_PLAY)
            c->stat &= (uint8_t)~ST_MOTOR;
        stop_drive(c);
        push1(c, 4, c->stat);
        return;
    }
    uint32_t tstart;
    int audio;
    int track = find_track(c, c->lba, &tstart, &audio);
    if (c->drive == DRIVE_PLAY) {
        if (c->play_track && track != c->play_track && (c->mode & MODE_AUTOPAUSE)) {
            stop_drive(c);
            push1(c, 4, c->stat);
            return;
        }
        c->q_lba = c->lba;
        if (audio)
            cdda_sector(c, s);
        if (c->mode & MODE_REPORT)
            report(c);
        c->lba++;
        c->drive_timer += sector_cycles(c);
        return;
    }
    /* reading */
    c->drive_timer += sector_cycles(c);
    c->q_lba = c->lba;
    if (audio) {
        if (c->mode & MODE_CDDA) {
            cdda_sector(c, s);
            if (c->mode & MODE_REPORT)
                report(c);
            c->lba++;
            return;
        }
    } else {
        memcpy(c->last_header, s + 12, 8);
        c->last_lba = c->lba;
        c->resume_lba = c->lba;
        c->have_read = 1;
        /* psx-spx « Data/ADPCM Sector Filtering/Delivery »: first to the XA decoder, else to the CPU */
        int realtime_audio = s[15] == 2 && (s[18] & 0x44) == 0x44;
        int match = !(c->mode & MODE_FILTER) || (s[16] == c->filter_file && s[17] == c->filter_chan);
        if (realtime_audio && (c->mode & MODE_XA) && match) {
            xa_play(c, s);
            c->lba++;
            return;
        }
        if (realtime_audio && (c->mode & (MODE_FILTER | MODE_XA))) {
            /* filtered out (another channel), or XA on: not delivered as data either */
            c->lba++;
            return;
        }
    }
    c->lba++;
    memcpy(c->pending_sector, s, sizeof s);
    /* a sector waiting for its INT1 is replaced (buffer overrun): only one INT1 sits in the queue */
    for (int i = 0; i < c->qcount; i++) {
        response *r = &c->queue[(c->qhead + i) % QUEUE];
        if (r->type == 1 && r->sector) {
            r->data[0] = c->stat;
            return;
        }
    }
    uint8_t st = c->stat;
    push(c, 1, &st, 1, 1);
}

static void drive_tick(ps1_cdrom *c, uint32_t cycles)
{
    if (c->drive == DRIVE_IDLE)
        return;
    c->drive_timer -= cycles;
    while (c->drive != DRIVE_IDLE && c->drive_timer <= 0) {
        if (c->drive == DRIVE_SEEK) {
            c->stat &= (uint8_t)~ST_SEEK;
            int then = c->after_seek;
            if (then == DRIVE_READ) {
                c->drive = DRIVE_READ;
                c->stat |= ST_READ;
                c->drive_timer += sector_cycles(c);
            } else if (then == DRIVE_PLAY) {
                c->drive = DRIVE_PLAY;
                c->stat |= ST_PLAY;
                c->drive_timer += sector_cycles(c);
            } else {
                c->drive = DRIVE_IDLE;
                c->q_lba = c->lba;
                /* SeekL / SeekP complete: GetlocL sees the header of the target sector (a data track) */
                uint8_t sec[2352];
                int audio = 1;
                find_track(c, c->lba, NULL, &audio);
                if (!audio && ps1_disc_read_sector(c->disc, c->lba, sec) == 0) {
                    memcpy(c->last_header, sec + 12, 8);
                    c->have_read = 1;
                }
                if (c->second_cmd == 0x15 || c->second_cmd == 0x16) {
                    c->second_pending = 0;
                    push1(c, 2, c->stat);
                }
            }
        } else {
            drive_step(c);
        }
    }
}

/* ---- commands ----------------------------------------------------------------------------------------------- */

static int disc_present(const ps1_cdrom *c) { return c->disc != NULL && !c->lid_open; }

static void schedule_second(ps1_cdrom *c, uint8_t cmd, uint32_t delay)
{
    c->second_cmd = cmd;
    c->second_pending = 1;
    c->second_timer = delay;
}

static int param_count(uint8_t cmd, int *min, int *max)
{
    *min = *max = 0;
    switch (cmd) {
    case 0x02: *min = *max = 3; return 1;
    case 0x03: *min = 0, *max = 1; return 1;
    case 0x0D: case 0x1D: *min = *max = 2; return 1;
    case 0x0E: case 0x12: case 0x14: case 0x19: *min = *max = 1; return 1;
    case 0x01: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
    case 0x0F: case 0x10: case 0x11: case 0x13: case 0x15: case 0x16: case 0x1A: case 0x1B: case 0x1E:
        return 1;
    default: return 0;
    }
}

static void execute(ps1_cdrom *c)
{
    uint8_t cmd = c->cmd;
    const uint8_t *p = c->cmd_params;
    int mn, mx;
    if (!param_count(cmd, &mn, &mx)) {
        /* unsupported: the parameters stay in the FIFO for the next command (psx-spx) */
        memcpy(c->params, c->cmd_params, sizeof c->params);
        c->nparams = c->cmd_nparams;
        error(c, 0x40);
        return;
    }
    if (c->cmd_nparams < mn || c->cmd_nparams > mx) {
        error(c, 0x20);
        return;
    }
    /* commands that need a disc */
    int needs_disc = !(cmd == 0x01 || cmd == 0x0A || cmd == 0x0B || cmd == 0x0C || cmd == 0x0D || cmd == 0x0E ||
                       cmd == 0x0F || cmd == 0x19 || cmd == 0x1A || cmd == 0x02 || cmd == 0x08 || cmd == 0x09);
    if (needs_disc && !disc_present(c)) {
        error(c, 0x80);
        return;
    }
    switch (cmd) {
    case 0x01: /* Getstat */
        push1(c, 3, c->stat);
        if (!c->lid_open)
            c->stat &= (uint8_t)~ST_SHELL; /* the lid is closed again: the latch clears once read */
        break;
    case 0x02: /* Setloc */
        if (!bcd_ok(p[0]) || !bcd_ok(p[1]) || !bcd_ok(p[2]) || from_bcd(p[1]) >= 60 || from_bcd(p[2]) >= 75) {
            error(c, 0x10);
            break;
        }
        {
            uint32_t a = (uint32_t)((from_bcd(p[0]) * 60 + from_bcd(p[1])) * 75 + from_bcd(p[2]));
            c->setloc = a >= 150 ? a - 150 : 0;
            c->setloc_pending = 1;
        }
        push1(c, 3, c->stat);
        break;
    case 0x03: /* Play [track] */
        drop_int1(c);
        if (c->cmd_nparams == 1 && p[0]) {
            int t = from_bcd(p[0]);
            uint32_t s;
            if (t > ps1_disc_track_count(c->disc)) /* N+1..99h restart the current track */
                t = find_track(c, c->lba, NULL, NULL);
            ps1_disc_track(c->disc, t, &s, NULL);
            c->setloc = s;
            c->setloc_pending = 1;
        }
        c->play_track = (uint8_t)find_track(c, c->setloc_pending ? c->setloc : c->lba, NULL, NULL);
        push1(c, 3, c->stat);
        if (c->setloc_pending) {
            start_seek(c, DRIVE_PLAY);
        } else {
            c->drive = DRIVE_PLAY;
            c->stat = (uint8_t)((c->stat & ~(ST_READ | ST_SEEK)) | ST_PLAY | ST_MOTOR);
            c->drive_timer = sector_cycles(c);
        }
        break;
    case 0x04: /* Forward */
    case 0x05: /* Backward */
        if (c->drive != DRIVE_PLAY) {
            error(c, 0x80);
            break;
        }
        push1(c, 3, c->stat);
        if (cmd == 0x04)
            c->lba += 75 * 2;
        else
            c->lba = c->lba > 75 * 2 ? c->lba - 75 * 2 : 0;
        break;
    case 0x06: /* ReadN */
    case 0x1B: /* ReadS */
        drop_int1(c);
        push1(c, 3, c->stat);
        if (c->setloc_pending || c->drive == DRIVE_PLAY) {
            start_seek(c, DRIVE_READ);
        } else if (c->drive == DRIVE_IDLE) {
            /* paused: reading resumes at the most recently received sector, or where SeekL / SeekP went */
            c->lba = c->resume_lba;
            start_seek(c, DRIVE_READ);
        } else if (c->drive == DRIVE_SEEK) {
            c->after_seek = DRIVE_READ;
        }
        break;
    case 0x07: /* MotorOn: only when the motor is off */
        if (c->stat & ST_MOTOR) {
            error(c, 0x20);
            break;
        }
        push1(c, 3, c->stat);
        schedule_second(c, cmd, SPINUP_DELAY);
        c->stat |= ST_MOTOR;
        break;
    case 0x08: /* Stop */
    {
        int spinning = (c->stat & ST_MOTOR) != 0;
        drop_int1(c);
        stop_drive(c);
        push1(c, 3, c->stat);
        c->stat &= (uint8_t)~ST_MOTOR;
        schedule_second(c, cmd, !spinning ? STOP_DELAY_IDLE : (c->mode & MODE_SPEED) ? STOP_DELAY_2X : STOP_DELAY_1X);
        break;
    }
    case 0x09: /* Pause */
    {
        int active = c->drive != DRIVE_IDLE;
        drop_int1(c);
        push1(c, 3, c->stat);
        stop_drive(c);
        schedule_second(c, cmd, !active ? PAUSE_DELAY_IDLE : (c->mode & MODE_SPEED) ? PAUSE_DELAY_2X : PAUSE_DELAY_1X);
        break;
    }
    case 0x0A: /* Init */
        drop_int1(c);
        stop_drive(c);
        c->seek_failed = 0;
        c->mode = MODE_SIZE;
        c->setloc_pending = 0;
        if (c->disc)
            c->stat |= ST_MOTOR;
        push1(c, 3, c->stat);
        schedule_second(c, cmd, INIT_SECOND);
        break;
    case 0x0B: /* Mute */
        c->muted = 1;
        push1(c, 3, c->stat);
        break;
    case 0x0C: /* Demute */
        c->muted = 0;
        push1(c, 3, c->stat);
        break;
    case 0x0D: /* Setfilter */
        c->filter_file = p[0];
        c->filter_chan = p[1];
        push1(c, 3, c->stat);
        break;
    case 0x0E: /* Setmode */
        c->mode = p[0];
        push1(c, 3, c->stat);
        break;
    case 0x0F: /* Getparam */
    {
        uint8_t r[5] = {c->stat, c->mode, 0, c->filter_file, c->filter_chan};
        push(c, 3, r, 5, 0);
        break;
    }
    case 0x10: /* GetlocL: header + subheader of the newest data sector; none while seeking or on audio */
    {
        int audio = 0;
        find_track(c, c->lba, NULL, &audio);
        if (c->seek_failed) {
            uint8_t r[2] = {(uint8_t)(c->stat | ST_ERROR | ST_SEEKERR), 0x80};
            push(c, 5, r, 2, 0);
            break;
        }
        if (c->drive == DRIVE_SEEK || c->drive == DRIVE_PLAY || audio || !c->have_read) {
            error(c, 0x80);
            break;
        }
        push(c, 3, c->last_header, 8, 0);
        break;
    }
    case 0x11: /* GetlocP: sub-channel Q of the sector last under the head (the one playing, the seek's target) */
    {
        if (c->seek_failed) { /* the hardware log of ps1-tests cdrom/getloc: fails after a seek past the end */
            uint8_t e[2] = {(uint8_t)(c->stat | ST_ERROR | ST_SEEKERR), 0x80};
            push(c, 5, e, 2, 0);
            break;
        }
        qpos q = q_position(c, c->q_lba);
        uint8_t r[8] = {q.track, q.index, bcd(q.rel / 4500), bcd((q.rel / 75) % 60), bcd(q.rel % 75),
                        bcd(q.abs / 4500), bcd((q.abs / 75) % 60), bcd(q.abs % 75)};
        push(c, 3, r, 8, 0);
        break;
    }
    case 0x12: /* SetSession */
        if (p[0] == 0) {
            error(c, 0x10);
            break;
        }
        push1(c, 3, c->stat);
        if (p[0] == 1) {
            schedule_second(c, cmd, TOC_DELAY);
        } else {
            /* single-session images: no session 2 */
            c->second_cmd = 0x12;
            c->second_pending = 2;
            c->second_timer = TOC_DELAY;
        }
        break;
    case 0x13: /* GetTN */
    {
        uint8_t r[3] = {c->stat, 1, bcd((unsigned)ps1_disc_track_count(c->disc))};
        push(c, 3, r, 3, 0);
        break;
    }
    case 0x14: /* GetTD */
    {
        int t = from_bcd(p[0]), n = ps1_disc_track_count(c->disc);
        uint32_t lba;
        if (!bcd_ok(p[0]) || t > n) {
            error(c, 0x10);
            break;
        }
        if (t == 0)
            lba = ps1_disc_size(c->disc);
        else
            ps1_disc_track(c->disc, t, &lba, NULL);
        lba += 150;
        uint8_t r[3] = {c->stat, bcd(lba / 4500), bcd((lba / 75) % 60)};
        push(c, 3, r, 3, 0);
        break;
    }
    case 0x15: /* SeekL */
    case 0x16: /* SeekP */
        drop_int1(c);
        if (c->setloc_pending && c->setloc >= ps1_disc_size(c->disc)) {
            c->setloc_pending = 0;
            stop_drive(c);
            push1(c, 3, c->stat);
            c->second_cmd = cmd;
            c->second_pending = 3; /* seek error */
            c->second_timer = FIRST_DELAY;
            break;
        }
        push1(c, 3, c->stat);
        start_seek(c, DRIVE_IDLE);
        c->second_cmd = cmd;
        c->second_pending = 0; /* the seek's end delivers INT2 */
        break;
    case 0x19: /* Test */
        switch (p[0]) {
        case 0x20: {
            static const uint8_t ver[4] = {0x97, 0x01, 0x10, 0xC2}; /* PSX (PU-18, us/eur), 10 Jan 1997, vC2 */
            push(c, 3, ver, 4, 0);
            break;
        }
        case 0x04: /* start SCEx counters */
            push1(c, 3, c->stat);
            break;
        case 0x05: /* read SCEx counters */
        {
            uint8_t r[2] = {0, 0};
            push(c, 3, r, 2, 0);
            break;
        }
        case 0x22: { /* region */
            static const char reg[] = "for U/C";
            push(c, 3, (const uint8_t *)reg, 7, 0);
            break;
        }
        default: error(c, 0x10); break;
        }
        break;
    case 0x1A: /* GetID */
        push1(c, 3, c->stat);
        schedule_second(c, cmd, GETID_DELAY);
        break;
    case 0x1D: /* GetQ adr point: a TOC entry of the lead-in, INT2 with 10 bytes */
        if (!bcd_ok(p[1]) && p[1] < 0xA0) {
            error(c, 0x10);
            break;
        }
        c->getq_point = p[1];
        push1(c, 3, c->stat);
        schedule_second(c, cmd, GETID_DELAY);
        break;
    case 0x1E: /* ReadTOC */
        drop_int1(c);
        stop_drive(c);
        push1(c, 3, c->stat);
        schedule_second(c, cmd, TOC_DELAY);
        break;
    }
}

static void second(ps1_cdrom *c)
{
    uint8_t cmd = c->second_cmd;
    int kind = c->second_pending;
    c->second_pending = 0;
    c->second_cmd = 0;
    switch (cmd) {
    case 0x1A: /* GetID */
        if (!disc_present(c)) {
            uint8_t r[8] = {0x08, 0x40, 0, 0, 0, 0, 0, 0};
            push(c, 5, r, 8, 0);
        } else {
            uint32_t s;
            int audio = 0;
            ps1_disc_track(c->disc, 1, &s, &audio);
            if (audio) {
                uint8_t r[8] = {0x0A, 0x90, 0, 0, 0, 0, 0, 0};
                push(c, 5, r, 8, 0);
            } else {
                uint8_t r[8] = {c->stat, 0x00, 0x20, 0x00, 'S', 'C', 'E', c->license};
                push(c, 2, r, 8, 0);
            }
        }
        break;
    case 0x1D: /* GetQ: control / adr, track 00, point, the lead-in's running time, 00, the point's time */
    {
        uint8_t pt = c->getq_point;
        int n = ps1_disc_track_count(c->disc);
        uint32_t lba = 0;
        int audio = 0, ok = 1, first_audio = 0;
        uint8_t pm = 0, ps = 0, pf = 0;
        ps1_disc_track(c->disc, 1, NULL, &first_audio);
        if (pt == 0xA0 || pt == 0xA1) {
            pm = bcd((unsigned)(pt == 0xA0 ? 1 : n));
            ps = first_audio ? 0x00 : 0x20; /* disc type: CD-DA, else CD-ROM XA */
            ps1_disc_track(c->disc, pt == 0xA0 ? 1 : n, NULL, &audio);
        } else if (pt == 0xA2) {
            lba = ps1_disc_size(c->disc) + 150;
            pm = bcd(lba / 4500), ps = bcd((lba / 75) % 60), pf = bcd(lba % 75);
            ps1_disc_track(c->disc, n, NULL, &audio);
        } else if (pt >= 1 && from_bcd(pt) <= n && ps1_disc_track(c->disc, from_bcd(pt), &lba, &audio) == 0) {
            lba += 150;
            pm = bcd(lba / 4500), ps = bcd((lba / 75) % 60), pf = bcd(lba % 75);
        } else {
            ok = 0;
        }
        if (!disc_present(c) || !ok) {
            uint8_t r[2] = {(uint8_t)(c->stat | ST_ERROR), 0x80};
            push(c, 5, r, 2, 0);
            break;
        }
        uint8_t r[10] = {(uint8_t)((audio ? 0x00 : 0x40) | 0x01), 0x00, pt, 0x00, 0x00, 0x00, 0x00, pm, ps, pf};
        push(c, 2, r, 10, 0);
        break;
    }
    case 0x12:
        if (kind == 2) {
            uint8_t r[2] = {(uint8_t)(c->stat | ST_ERROR), 0x40};
            push(c, 5, r, 2, 0);
            break;
        }
        push1(c, 2, c->stat);
        break;
    case 0x15:
    case 0x16:
        if (kind == 3) {
            c->stat &= (uint8_t)~ST_MOTOR; /* the hardware answers stat 04h: motor stopped */
            c->seek_failed = 1;
            uint8_t r[2] = {(uint8_t)(c->stat | ST_SEEKERR), 0x04};
            push(c, 5, r, 2, 0);
        }
        break;
    default:
        push1(c, 2, c->stat);
        break;
    }
}

static uint8_t detect_license(ps1_disc *d)
{
    uint8_t s[2352];
    if (!d || ps1_disc_read_sector(d, 4, s) != 0)
        return 'A';
    const uint8_t *p = s + 24;
    for (int i = 0; i + 4 <= 2048; i++) {
        if (!memcmp(p + i, "Inc", 3))
            return 'I';
        if (!memcmp(p + i, "Amer", 4))
            return 'A';
        if (!memcmp(p + i, "Euro", 4))
            return 'E';
    }
    return 'A';
}

/* ---- public ------------------------------------------------------------------------------------------------- */

static void reset(ps1_cdrom *c)
{
    ps1_irq_sink irq = c->irq;
    ps1_disc *d = c->disc;
    memset(c, 0, sizeof *c);
    c->irq = irq;
    c->disc = d;
    c->stat = d ? ST_MOTOR : 0;
    c->vol[0] = c->vol[2] = 0x80;
    memcpy(c->vol_next, c->vol, 4);
    c->license = detect_license(d);
    c->xa_sixstep = 6;
}

/* The lid opens: the drive and the motor stop, reads are dropped, INT5 tells the CPU: 01h 08h, as the hardware log of
 * ps1-tests cdrom/disc-swap shows (the motor stopped, bit 4 not set yet in that response; Getstat has it after). */
static void lid_open(ps1_cdrom *c)
{
    if (c->lid_open)
        return;
    c->lid_open = 1;
    drop_int1(c);
    stop_drive(c);
    c->second_pending = 0;
    c->second_cmd = 0;
    c->setloc_pending = 0;
    c->have_read = 0;
    c->stat = (uint8_t)(c->stat & ~(ST_MOTOR | ST_READ | ST_SEEK | ST_PLAY | ST_SHELL));
    error(c, 0x08);
    c->stat |= ST_SHELL;
}

/* The lid closes: the motor spins up on the disc there (if any); stat bit 4 stays until a Getstat. */
static void lid_close(ps1_cdrom *c)
{
    if (!c->lid_open)
        return;
    c->lid_open = 0;
    c->seek_failed = 0;
    c->license = detect_license(c->disc);
    c->lba = c->resume_lba = c->q_lba = 0;
    c->have_read = 0;
    if (c->disc)
        c->stat |= ST_MOTOR;
}

ps1_cdrom *ps1_cdrom_create(ps1_irq_sink irq, ps1_disc *disc)
{
    ps1_cdrom *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->irq = irq;
    c->disc = disc;
    reset(c);
    return c;
}

void ps1_cdrom_destroy(ps1_cdrom *c) { free(c); }

void ps1_cdrom_set_disc(ps1_cdrom *c, ps1_disc *disc)
{
    if (c->lid_open) {
        c->disc = disc;
        return;
    }
    lid_open(c);
    c->disc = disc;
    lid_close(c);
}

void ps1_cdrom_set_lid(ps1_cdrom *c, int open)
{
    if (open)
        lid_open(c);
    else
        lid_close(c);
}

uint8_t ps1_cdrom_read(ps1_cdrom *c, uint32_t reg)
{
    switch (reg & 3) {
    case 0: {
        uint8_t v = c->index;
        if (c->xa_play_frames)
            v |= 0x04;
        if (c->nparams == 0)
            v |= 0x08;
        if (c->nparams < 16)
            v |= 0x10;
        if (c->resp_pos < c->resp_len)
            v |= 0x20;
        if (c->data_pos < c->data_len)
            v |= 0x40;
        if (c->busy)
            v |= 0x80;
        return v;
    }
    case 1: {
        uint8_t v = c->resp[c->resp_pos & 15]; /* past the end it wraps around the 16 bytes */
        if (c->resp_pos < 255)
            c->resp_pos++;
        return v;
    }
    case 2:
        if (c->data_pos < c->data_len)
            return c->data[c->data_pos++];
        /* past the end: the byte at 800h-8 / 924h-4 repeats */
        if (c->data_len == 0x800)
            return c->data[0x800 - 8];
        return c->data_len ? c->data[c->data_len - 4] : 0;
    default:
        if (c->index & 1)
            return (uint8_t)(c->iflag | 0xE0);
        return (uint8_t)(c->ie | 0xE0);
    }
}

static void load_data(ps1_cdrom *c)
{
    if (c->mode & MODE_SIZE) {
        memcpy(c->data, c->current_sector + 12, 0x924);
        c->data_len = 0x924;
    } else {
        memcpy(c->data, c->current_sector + 24, 0x800);
        c->data_len = 0x800;
    }
    c->data_pos = 0;
    c->sector_fresh = 0;
}

void ps1_cdrom_write(ps1_cdrom *c, uint32_t reg, uint8_t v)
{
    reg &= 3;
    if (reg == 0) {
        c->index = v & 3;
        return;
    }
    switch (reg * 4 + c->index) {
    case 1 * 4 + 0: /* command */
        if (v == 0x0A && c->second_pending && c->second_cmd == 0x0A)
            break; /* Init while an Init is in progress: dropped, no response */
        c->cmd = v;
        memcpy(c->cmd_params, c->params, sizeof c->params);
        c->cmd_nparams = c->nparams;
        c->nparams = 0;
        c->cmd_pending = 1;
        c->cmd_timer = (v == 0x0A) ? INIT_DELAY : (c->stat & ST_MOTOR) ? FIRST_DELAY : FIRST_DELAY_STOPPED;
        c->busy = 1;
        break;
    case 2 * 4 + 0: /* parameter */
        if (c->nparams < 16)
            c->params[c->nparams++] = v;
        break;
    case 3 * 4 + 0: /* request */
        c->request = v;
        if (v & 0x80) {
            /* BFRD: the sector of the last INT1 (a FIFO left half read is replaced by a newer sector) */
            if (c->sector_fresh || c->data_pos >= c->data_len)
                load_data(c);
        } else {
            c->data_len = c->data_pos = 0;
        }
        break;
    case 2 * 4 + 1: /* interrupt enable */
        c->ie = v & 0x1F;
        raise_if_pending(c);
        break;
    case 3 * 4 + 1: /* interrupt flag acknowledge */
        /* the response stays readable after the acknowledge (PSn00bSDK acknowledges first, then reads it); the
         * next delivery replaces it */
        c->iflag &= (uint8_t)~(v & 0x1F);
        if (v & 0x40)
            c->nparams = 0;
        break;
    case 2 * 4 + 2: c->vol_next[0] = v; break; /* L -> L */
    case 3 * 4 + 2: c->vol_next[1] = v; break; /* L -> R */
    case 1 * 4 + 3: c->vol_next[2] = v; break; /* R -> R */
    case 2 * 4 + 3: c->vol_next[3] = v; break; /* R -> L */
    case 3 * 4 + 3:
        c->xa_mute = v & 1;
        if (v & 0x20)
            memcpy(c->vol, c->vol_next, 4);
        break;
    default: break; /* sound map data / coding info */
    }
}

void ps1_cdrom_tick(ps1_cdrom *c, uint32_t cycles)
{
    if (c->cmd_pending) {
        c->cmd_timer -= cycles;
        if (c->cmd_timer <= 0) {
            c->cmd_pending = 0;
            execute(c);
        }
    }
    if (c->second_pending) {
        c->second_timer -= cycles;
        if (c->second_timer <= 0)
            second(c);
    }
    drive_tick(c, cycles);
    deliver(c);
}

size_t ps1_cdrom_dma_read(ps1_cdrom *c, uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++)
            w |= (uint32_t)ps1_cdrom_read(c, 2) << (8 * b);
        words[i] = w;
    }
    return n;
}

size_t ps1_cdrom_audio_pull(ps1_cdrom *c, int16_t *stereo, size_t frames)
{
    size_t n = frames < c->ring_count ? frames : c->ring_count;
    for (size_t i = 0; i < n; i++) {
        stereo[2 * i] = c->ring[2 * c->ring_head];
        stereo[2 * i + 1] = c->ring[2 * c->ring_head + 1];
        c->ring_head = (c->ring_head + 1) & (AUDIO_RING - 1);
    }
    c->ring_count -= (uint32_t)n;
    c->xa_play_frames = c->xa_play_frames > n ? c->xa_play_frames - (uint32_t)n : 0;
    if (!c->ring_count)
        c->xa_play_frames = 0;
    return n;
}

/* ---- Save states (XBA-182) ------------------------------------------------------------------------------------ */

/* The controller whole (registers, FIFOs, the command and responses in flight, the drive's position, sector buffers,
 * XA / CD-DA state and its audio ring); the disc stays the one in the drive now. */
size_t ps1_cdrom_state_size(ps1_cdrom *c)
{
    (void)c;
    return sizeof(ps1_cdrom);
}

void ps1_cdrom_state_save(ps1_cdrom *c, uint8_t *out)
{
    size_t ni = sizeof c->irq, nd = sizeof c->disc;
    memcpy(out, c, sizeof *c);
    memset(out + offsetof(ps1_cdrom, irq), 0, ni);
    memset(out + offsetof(ps1_cdrom, disc), 0, nd);
}

/* The fields the controller indexes with: the response queue (head, count, each response's length copied into
 * resp[16]), the data FIFO (a sector's 800h or 924h bytes, the read position within), the audio ring. */
int ps1_cdrom_state_valid(const uint8_t *in, size_t n)
{
    if (n != sizeof(ps1_cdrom))
        return -1;
    int qhead, qcount;
    uint32_t data_len, data_pos, ring_head, ring_count;
    PS1_STATE_GET(qhead, in, ps1_cdrom, qhead);
    PS1_STATE_GET(qcount, in, ps1_cdrom, qcount);
    PS1_STATE_GET(data_len, in, ps1_cdrom, data_len);
    PS1_STATE_GET(data_pos, in, ps1_cdrom, data_pos);
    PS1_STATE_GET(ring_head, in, ps1_cdrom, ring_head);
    PS1_STATE_GET(ring_count, in, ps1_cdrom, ring_count);
    if (qhead < 0 || qhead >= QUEUE || qcount < 0 || qcount > QUEUE)
        return -2;
    for (int i = 0; i < QUEUE; i++) { /* every slot: one is filled before it counts */
        uint8_t len = in[offsetof(ps1_cdrom, queue) + (size_t)i * sizeof(response) + offsetof(response, len)];
        if (len > sizeof ((response *)0)->data)
            return -2;
    }
    if ((data_len != 0 && data_len != 0x800 && data_len != 0x924) || data_pos > data_len)
        return -2;
    if (ring_head >= AUDIO_RING || ring_count > AUDIO_RING)
        return -2;
    return 0;
}

int ps1_cdrom_state_load(ps1_cdrom *c, const uint8_t *in, size_t n)
{
    if (n != sizeof *c)
        return -1;
    ps1_irq_sink irq = c->irq;
    ps1_disc *disc = c->disc;
    memcpy(c, in, sizeof *c);
    c->irq = irq;
    c->disc = disc;
    return 0;
}
