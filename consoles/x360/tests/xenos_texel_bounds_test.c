/* texel() reads one texel's bytes and no more (XBA-318): Warhammer - Battle March crashed in xenos_gl_draw when the last
 * texel of a 1024 x 768 k_8 texture ended its buffer on a page boundary and texel read 3 bytes past it. Each 8 and
 * 16 bpp format is decoded from the last bytes of a page followed by an inaccessible one. */
#include "../runtime/xenos_gl.c"

#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    uint8_t *m = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + page, (size_t)page, PROT_NONE)) {
        perror("mmap");
        return 1;
    }
    static const struct { uint32_t format; unsigned bytes; } fmts[] = {{2, 1}, {8, 1}, {9, 1}, {10, 2}, {3, 2}, {4, 2}, {5, 2}, {15, 2}};
    uint8_t out[4];
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        const texfmt *tf = tex_format(fmts[i].format);
        if (!tf || tf->bytes != fmts[i].bytes) {
            fprintf(stderr, "format %u: %u bytes per texel expected\n", fmts[i].format, fmts[i].bytes);
            return 1;
        }
        uint8_t *s = m + page - fmts[i].bytes;
        memset(s, 0xFF, fmts[i].bytes);
        texel(fmts[i].format, s, out); /* a read past s faults */
    }
    m[page - 1] = 0x7F;
    texel(2, m + page - 1, out);
    if (out[0] != 0x7F || out[3] != 0x7F) {
        fprintf(stderr, "k_8: %02x %02x %02x %02x\n", out[0], out[1], out[2], out[3]);
        return 1;
    }
    puts("texel bounds: ok");
    return 0;
}
