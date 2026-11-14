/* PS1 GTE unit tests (XBA-164): hand-checked vectors (worked out from psx-spx's formulas) for RTPS and the UNR
 * division, NCLIP, AVSZ3, SQR saturation, OP, GPF and the FLAG bit 31 mask, DPCS, MVMVA, the divide overflow, and the
 * register quirks (sign extension, H, IRGB / ORGB, LZCS / LZCR, SXYP, FLAG writes). The exhaustive check is
 * JaCzekanski's gte/test-all (ps1_jcz_gte_test_all, 1150 vectors from hardware). */
#include "ps1_internal.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

#define SF (1u << 19)
#define LM (1u << 10)
#define D(r) ps1_gte_read_data(&g, r)
#define C(r) ps1_gte_read_ctrl(&g, r)
#define WD(r, v) ps1_gte_write_data(&g, r, v)
#define WC(r, v) ps1_gte_write_ctrl(&g, r, v)

static void identity(ps1_gte *gp)
{
    ps1_gte_write_ctrl(gp, 0, 0x00001000u); /* RT11 = 1.0, RT12 = 0 */
    ps1_gte_write_ctrl(gp, 1, 0);
    ps1_gte_write_ctrl(gp, 2, 0x00001000u); /* RT22 */
    ps1_gte_write_ctrl(gp, 3, 0);
    ps1_gte_write_ctrl(gp, 4, 0x1000u);     /* RT33 */
}

int main(void)
{
    ps1_gte g;

    /* RTPS: identity rotation, V0 = (100, 50, 1000), H = 1000 -> the UNR reciprocal gives exactly 1.0 (10000h). */
    ps1_gte_reset(&g);
    identity(&g);
    WC(26, 1000);
    WC(27, 0x0100); /* DQA */
    WC(28, 0x01000000); /* DQB */
    WD(0, (50u << 16) | 100u);
    WD(1, 1000);
    ps1_gte_command(&g, SF | 0x01);
    CHECK((int32_t)D(25) == 100 && (int32_t)D(26) == 50 && (int32_t)D(27) == 1000, "RTPS MAC %d %d %d",
          (int32_t)D(25), (int32_t)D(26), (int32_t)D(27));
    CHECK(D(19) == 1000, "RTPS SZ3 %u", D(19));
    CHECK(D(14) == ((50u << 16) | 100u) && D(15) == D(14), "RTPS SXY2 %08X", D(14));
    /* MAC0 = 10000h * DQA + DQB = 0x01000000 + 0x01000000; IR0 = MAC0 >> 12 = 0x2000 -> saturated to 1000h. */
    CHECK((uint32_t)D(24) == 0x02000000u && D(8) == 0x1000, "RTPS MAC0 %08X IR0 %08X", D(24), D(8));
    CHECK(C(31) == 0x00001000u, "RTPS FLAG %08X (IR0 saturated only, no bit 31)", C(31));

    /* Divide overflow: SZ3 = 0 -> 1FFFFh, FLAG bit 17 (+ 31). SX2 = 1FFFFh * 100 >> 16 = 199. */
    ps1_gte_reset(&g);
    identity(&g);
    WC(26, 1000);
    WD(0, 100);
    WD(1, 0);
    ps1_gte_command(&g, SF | 0x01);
    CHECK((C(31) & 0x80020000u) == 0x80020000u, "divide overflow FLAG %08X", C(31));
    CHECK((int16_t)D(14) == 199, "divide overflow SX2 %d", (int16_t)D(14));

    /* RTPT pushes three entries through the FIFOs. */
    ps1_gte_reset(&g);
    identity(&g);
    WC(26, 1000);
    WD(0, 10); WD(1, 1000);
    WD(2, 20); WD(3, 1000);
    WD(4, 30); WD(5, 1000);
    ps1_gte_command(&g, SF | 0x30);
    CHECK(D(12) == 10 && D(13) == 20 && D(14) == 30, "RTPT SXY %08X %08X %08X", D(12), D(13), D(14));
    CHECK(D(17) == 1000 && D(18) == 1000 && D(19) == 1000, "RTPT SZ");

    /* NCLIP: (0,0) (10,0) (0,10) -> 100. */
    ps1_gte_reset(&g);
    WD(12, 0);
    WD(13, 10);
    WD(14, 10u << 16);
    ps1_gte_command(&g, 0x06);
    CHECK((int32_t)D(24) == 100, "NCLIP %d", (int32_t)D(24));

    /* AVSZ3: ZSF3 = 555h, SZ1..3 = 300 -> MAC0 = 1365 * 900 = 1228500, OTZ = 299. */
    ps1_gte_reset(&g);
    WC(29, 0x555);
    WD(17, 300); WD(18, 300); WD(19, 300);
    ps1_gte_command(&g, 0x2D);
    CHECK((int32_t)D(24) == 1228500 && D(7) == 299, "AVSZ3 MAC0 %d OTZ %u", (int32_t)D(24), D(7));

    /* SQR, sf = 0: IR1 = 200h -> MAC1 = 40000h, IR1 saturates to 7FFFh (FLAG 24 + 31). */
    ps1_gte_reset(&g);
    WD(9, 0x200); WD(10, 2); WD(11, 0xFFFD);
    ps1_gte_command(&g, 0x28);
    CHECK((int32_t)D(25) == 0x40000 && D(9) == 0x7FFF && (int32_t)D(26) == 4 && (int32_t)D(27) == 9,
          "SQR MAC1 %08X IR1 %08X", D(25), D(9));
    CHECK(C(31) == 0x81000000u, "SQR FLAG %08X", C(31));

    /* OP, sf = 1: diagonal (1, 2, 3) x 1.0, IR = (1, 2, 3) x 1.0 -> (3*2 - 2*3, 1*3 - 3*1, 2*1 - 1*2) = 0. */
    ps1_gte_reset(&g);
    WC(0, 0x1000); WC(2, 0x2000); WC(4, 0x3000);
    WD(9, 0x2000); WD(10, 0x1000); WD(11, 0x1000); /* IR = (2, 1, 1) */
    ps1_gte_command(&g, SF | 0x0C);
    /* (IR3*D2 - IR2*D3, IR1*D3 - IR3*D1, IR2*D1 - IR1*D2) = (2-3, 6-1, 1-4) */
    CHECK((int32_t)D(25) == -0x1000 && (int32_t)D(26) == 0x5000 && (int32_t)D(27) == -0x3000, "OP %d %d %d",
          (int32_t)D(25), (int32_t)D(26), (int32_t)D(27));

    /* GPF, sf = 1: IR0 = 0.5 -> MAC = (800h, 1000h, 200h); colour G = 100h saturates (FLAG 20, not in bit 31). */
    ps1_gte_reset(&g);
    WD(6, 0x5A000000u); /* code byte */
    WD(8, 0x800); WD(9, 0x1000); WD(10, 0x2000); WD(11, 0x400);
    ps1_gte_command(&g, SF | 0x3D);
    CHECK(D(22) == 0x5A20FF80u, "GPF RGB2 %08X", D(22));
    CHECK(C(31) == 0x00100000u, "GPF FLAG %08X", C(31));

    /* GPL adds to the previous MAC. */
    ps1_gte_command(&g, SF | 0x3E);
    CHECK((int32_t)D(25) == 0xC00, "GPL MAC1 %08X", D(25)); /* 800h + 800h * 0.5 */

    /* DPCS, sf = 1, IR0 = 0: the colour comes back unchanged. */
    ps1_gte_reset(&g);
    WD(6, 0x30204080u);
    WD(8, 0);
    ps1_gte_command(&g, SF | 0x10);
    CHECK(D(22) == 0x30204080u, "DPCS RGB2 %08X", D(22));
    /* IR0 = 1.0 and FC = (10h, 20h, 30h) x 16: the far colour. */
    WC(21, 0x100); WC(22, 0x200); WC(23, 0x300);
    WD(8, 0x1000);
    ps1_gte_command(&g, SF | 0x10);
    CHECK(D(22) == 0x30302010u, "DPCS to the far colour: %08X", D(22));

    /* MVMVA: RT x V1 + TR, sf = 1. */
    ps1_gte_reset(&g);
    identity(&g);
    WC(5, 10); WC(6, -20); WC(7, 30);
    WD(2, (0xFFFFu << 16) | 5u); /* V1 = (5, -1, 7) */
    WD(3, 7);
    ps1_gte_command(&g, SF | 0x12 | (0u << 17) | (1u << 15) | (0u << 13));
    CHECK((int32_t)D(25) == 15 && (int32_t)D(26) == -21 && (int32_t)D(27) == 37, "MVMVA %d %d %d", (int32_t)D(25),
          (int32_t)D(26), (int32_t)D(27));
    /* lm: negative results clamp to 0. */
    ps1_gte_command(&g, SF | LM | 0x12 | (1u << 15));
    CHECK(D(10) == 0 && (C(31) & (1u << 23)), "MVMVA lm IR2 %08X FLAG %08X", D(10), C(31));

    /* Register quirks. */
    ps1_gte_reset(&g);
    WD(1, 0x0000FFFFu);
    CHECK(D(1) == 0xFFFFFFFFu, "VZ0 sign-extended: %08X", D(1));
    WD(16, 0xFFFFFFFFu);
    CHECK(D(16) == 0xFFFF, "SZ0 zero-extended: %08X", D(16));
    WC(26, 0x8000);
    CHECK(C(26) == 0xFFFF8000u, "H reads signed: %08X", C(26));
    WD(28, 0x7FFF);
    CHECK(D(9) == 0xF80 && D(10) == 0xF80 && D(11) == 0xF80, "IRGB -> IR");
    WD(9, 0xFFFFF000u); /* IR1 negative -> 0 */
    WD(10, 0x2000);     /* IR2 large -> 1Fh */
    WD(11, 0x0280);     /* IR3 = 5 */
    CHECK(D(29) == ((5u << 10) | (0x1Fu << 5)) && D(28) == D(29), "ORGB %08X", D(29));
    WD(30, 0x00FF0000u);
    CHECK(D(31) == 8, "LZCR positive %u", D(31));
    WD(30, 0xFFF00000u);
    CHECK(D(31) == 12, "LZCR negative %u", D(31));
    WD(30, 0);
    CHECK(D(31) == 32, "LZCR zero %u", D(31));
    WD(12, 1); WD(13, 2); WD(14, 3);
    WD(15, 4);
    CHECK(D(12) == 2 && D(13) == 3 && D(14) == 4 && D(15) == 4, "SXYP push");
    WC(31, 0xFFFFFFFFu);
    CHECK(C(31) == 0xFFFFF000u, "FLAG write: %08X", C(31));
    WC(31, 0x00001000u);
    CHECK(C(31) == 0x00001000u, "FLAG bit 12 alone has no bit 31: %08X", C(31));
    WC(4, 0x8000);
    CHECK(C(4) == 0xFFFF8000u, "RT33 sign-extended: %08X", C(4));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ps1 gte: ok\n");
    return 0;
}
