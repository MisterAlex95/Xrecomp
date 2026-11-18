/* N64 run-time benchmark of the recompiler (XBA-277): game-shaped CPU work in a loop that never ends, so that
 * `n64_run bench.z64 --frames N` measures lifted code (and the bus it reaches) per frame. Integer state machines,
 * entity updates through RDRAM (32-bit and 16-bit fields, cached), byte buffers, 64-bit maths, float vectors,
 * calls through a function table, a VI interrupt. Deterministic: the checksum of each round goes to the result
 * block's checksum word (consoles/n64/tests/homebrew/common/xba_result.h), its round count to `count`. Our own code,
 * MIT like the rest of the repository; built by build_rom.sh (libdragon, image xba-n64-homebrew). */
#include <libdragon.h>
#include <stdint.h>

#include "xba_result.h"

#define NENT 96
#define NBUF 2048

typedef struct entity {
    int32_t x, y, vx, vy;
    int16_t hp, state;
    uint16_t timer, flags;
    float fx, fy, fz, fw;
} entity;

static entity ents[NENT];
static uint8_t buf[NBUF], out[NBUF];
static uint32_t lcg_s = 12345;
static volatile uint32_t vis;

static uint32_t lcg(void)
{
    lcg_s = lcg_s * 1103515245u + 12345u;
    return lcg_s >> 8;
}

static void on_vi(void)
{
    vis++;
}

static __attribute__((noinline)) uint32_t think_idle(entity *e)
{
    e->timer++;
    if ((e->timer & 15) == 0)
        e->state = 1;
    return (uint32_t)e->x ^ e->timer;
}

static __attribute__((noinline)) uint32_t think_walk(entity *e)
{
    e->x += e->vx;
    e->y += e->vy;
    if (e->x < 0 || e->x > (320 << 8)) {
        e->vx = -e->vx;
        e->state = 2;
    }
    return (uint32_t)(e->x + e->y);
}

static __attribute__((noinline)) uint32_t think_hit(entity *e)
{
    e->hp -= 3;
    if (e->hp <= 0) {
        e->hp = 100;
        e->state = 0;
        e->flags ^= 0x8000;
    } else {
        e->state = 1;
    }
    return (uint32_t)e->hp * 7u;
}

static __attribute__((noinline)) uint32_t think_float(entity *e)
{
    float d = e->fx * e->fx + e->fy * e->fy + e->fz * e->fz;
    e->fw = e->fw * 0.98f + d * 0.001f;
    e->fx += e->fw * 0.01f;
    e->fy -= e->fz * 0.02f;
    e->fz = e->fz * 0.5f + e->fy * 0.25f;
    e->state = 0;
    return (uint32_t)(int32_t)(e->fw * 16.0f);
}

static uint32_t (*const think[4])(entity *) = {think_idle, think_walk, think_hit, think_float};

static uint32_t update_entities(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < NENT; i++) {
        entity *e = &ents[i];
        sum += think[e->state & 3](e);
        if ((lcg() & 63) == 0)
            e->state = 3;
    }
    return sum;
}

static uint32_t bytes(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < NBUF; i++) {
        uint8_t b = buf[i];
        out[i] = (uint8_t)((b >> 1) | (b << 7)) ^ (uint8_t)i;
        h = (h ^ out[i]) * 16777619u;
    }
    for (int i = 0; i < NBUF; i += 4)
        buf[i] = out[NBUF - 1 - i];
    return h;
}

static uint64_t wide(uint32_t seed)
{
    uint64_t a = seed | ((uint64_t)seed << 32), b = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 64; i++) {
        a ^= a << 13;
        a ^= a >> 7;
        a ^= a << 17;
        b += a * 3 + (a >> 11);
    }
    return a ^ b;
}

static uint32_t sort_small(void)
{
    int16_t v[32];
    for (int i = 0; i < 32; i++)
        v[i] = (int16_t)lcg();
    for (int i = 1; i < 32; i++) { /* insertion sort */
        int16_t k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > k) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
    return (uint32_t)(uint16_t)v[0] | ((uint32_t)(uint16_t)v[31] << 16);
}

int main(void)
{
    xba_begin(XBA_FOURCC('B', 'N', 'C', 'H'));
    register_VI_handler(on_vi);
    for (int i = 0; i < NENT; i++) {
        ents[i].x = (int32_t)(lcg() & 0xFFFF);
        ents[i].y = (int32_t)(lcg() & 0xFFFF);
        ents[i].vx = (int32_t)(lcg() & 255) - 128;
        ents[i].vy = (int32_t)(lcg() & 255) - 128;
        ents[i].hp = 100;
        ents[i].state = (int16_t)(i & 3);
        ents[i].fx = (float)(i & 7) * 0.5f;
        ents[i].fy = 1.0f;
        ents[i].fz = -0.25f;
        ents[i].fw = 0.0f;
    }
    for (int i = 0; i < NBUF; i++)
        buf[i] = (uint8_t)lcg();
    uint32_t round = 0;
    for (;;) {
        uint32_t sum = update_entities();
        sum += bytes();
        sum += (uint32_t)wide(sum);
        sum += sort_small();
        XBA_RESULT->checksum = sum;
        XBA_RESULT->count = ++round;
        XBA_RESULT->frames = vis;
    }
}
