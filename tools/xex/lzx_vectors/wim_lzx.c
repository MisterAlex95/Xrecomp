/* wim_lzx IN OUT CHUNK_SIZE LEVEL -- compress IN as one LZX chunk with wimlib's compressor (XBA-270 test vectors).
 * Built and run only inside gen.sh's throwaway container against libwim-dev; nothing of wimlib is in the repository. */
#include <stdio.h>
#include <stdlib.h>
#include <wimlib.h>

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    static unsigned char in[1 << 22], out[1 << 22];
    size_t n = fread(in, 1, sizeof in, f);
    fclose(f);
    struct wimlib_compressor *c;
    if (wimlib_create_compressor(WIMLIB_COMPRESSION_TYPE_LZX, (size_t)atol(argv[3]), (unsigned)atoi(argv[4]), &c))
        return 3;
    size_t m = wimlib_compress(in, n, out, sizeof out, c);
    if (!m) {
        fprintf(stderr, "wim_lzx: %s did not compress\n", argv[1]);
        return 4;
    }
    f = fopen(argv[2], "wb");
    fwrite(out, 1, m, f);
    fclose(f);
    printf("%s: %zu -> %zu\n", argv[2], n, m);
    return 0;
}
