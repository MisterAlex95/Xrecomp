/* PS1 overlay B (XBA-149): same load address and entry as overlay A, different code. Our own code.
 * ovl_entry(x, cb) = (cb(x) + cb(x << 4)) | 0xB0000000. */
#include <stdint.h>

typedef uint32_t (*fn_t)(uint32_t);

__attribute__((section(".text.entry"), used)) uint32_t ovl_entry(uint32_t x, fn_t cb)
{
    uint32_t r = cb(x) + cb(x << 4);
    return r | 0xB0000000u;
}
