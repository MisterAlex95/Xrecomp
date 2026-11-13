/* PS1 overlay A (XBA-149): loaded from the disc to 0x80100000 by main.c. Our own code.
 * ovl_entry(x, cb) = (ops[x & 1](x) + cb(x)) ^ 0xA0A00000, ops = { 2x, 3x } (absolute pointers). */
#include <stdint.h>

typedef uint32_t (*fn_t)(uint32_t);

__attribute__((noinline)) static uint32_t twice(uint32_t v) { return v * 2; }
__attribute__((noinline)) static uint32_t thrice(uint32_t v) { return v * 3; }
static fn_t const ops[2] = { twice, thrice };

__attribute__((section(".text.entry"), used)) uint32_t ovl_entry(uint32_t x, fn_t cb)
{
    uint32_t r = ops[x & 1](x) + cb(x);
    return r ^ 0xA0A00000u;
}
