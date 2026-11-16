/* padtest: the GameCube pad test (XBA-259). libogc's console over the XFB and its PAD library: every field, the four
 * controller ports as PAD_Read gives them, drawn in place:
 *
 *   XRecomp GameCube pad test (XBA-259)
 *   Video NTSC 60 Hz   field 123
 *   Port 1  ok    buttons 0110  A Z
 *     stick   +000 +000   C stick +000 +000
 *     L 000   R 000       rumble off
 *   Port 2  none
 *   ...
 *
 * Holding Z on a port rumbles its controller (PAD_ControlMotor). The same values go into `report` (magic
 * "XBAPADT1"), which the headless test finds in memory (consoles/gc/tests/gc_padtest.c). Only our own code and
 * libogc: nothing of Nintendo's. */
#include <gccore.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* What the test reads back. Field order and sizes are part of the test (gc_padtest.c's struct). */
struct report {
    char magic[8] __attribute__((nonstring));
    uint32_t fields;   /* fields drawn */
    uint32_t tv_mode;  /* VIDEO_GetCurrentTvMode() */
    int32_t err[4];    /* PADStatus.err: 0 ok, -1 no controller, -2 not ready, -3 transfer */
    uint16_t button[4];
    int8_t stick_x[4], stick_y[4], substick_x[4], substick_y[4];
    uint8_t trigger_l[4], trigger_r[4];
    uint32_t rumble;   /* bit n: port n's motor on */
};
volatile struct report report __attribute__((aligned(32))) = {.magic = "XBAPADT1"};

static void *xfb;
static GXRModeObj *rmode;

static const char *const k_names[] = {"<", ">", "v", "^", "Z", "R", "L", "", "A", "B", "X", "Y", "START"};

static void buttons_text(uint16_t b, char *out, size_t n)
{
    size_t k = 0;
    out[0] = 0;
    for (unsigned i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if ((b & (1u << i)) && k_names[i][0] && k + 7 < n)
            k += (size_t)snprintf(out + k, n - k, "%s ", k_names[i]);
}

int main(void)
{
    VIDEO_Init();
    PAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    uint32_t tv = VIDEO_GetCurrentTvMode();
    report.tv_mode = tv;
    printf("\x1b[2;0HXRecomp GameCube pad test (XBA-259)");
    for (;;) {
        PADStatus st[PAD_CHANMAX];
        uint32_t rumble = 0;
        PAD_Read(st);
        printf("\x1b[4;0HVideo %s %s   field %lu   ", tv == VI_PAL ? "PAL" : tv == VI_MPAL ? "MPAL" : "NTSC",
               tv == VI_PAL ? "50 Hz" : "60 Hz", (unsigned long)report.fields);
        for (int p = 0; p < PAD_CHANMAX; p++) {
            int row = 6 + p * 4;
            report.err[p] = st[p].err;
            if (st[p].err == PAD_ERR_NONE) {
                char names[64];
                bool z = (st[p].button & PAD_TRIGGER_Z) != 0;
                PAD_ControlMotor(p, z ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
                if (z)
                    rumble |= 1u << p;
                report.button[p] = st[p].button;
                report.stick_x[p] = st[p].stickX;
                report.stick_y[p] = st[p].stickY;
                report.substick_x[p] = st[p].substickX;
                report.substick_y[p] = st[p].substickY;
                report.trigger_l[p] = st[p].triggerL;
                report.trigger_r[p] = st[p].triggerR;
                buttons_text(st[p].button, names, sizeof names);
                printf("\x1b[%d;0HPort %d  ok    buttons %04X  %-40s", row, p + 1, st[p].button, names);
                printf("\x1b[%d;0H  stick   %+04d %+04d   C stick %+04d %+04d   ", row + 1, st[p].stickX,
                       st[p].stickY, st[p].substickX, st[p].substickY);
                printf("\x1b[%d;0H  L %03u   R %03u       rumble %s ", row + 2, st[p].triggerL, st[p].triggerR,
                       z ? "on " : "off");
            } else {
                printf("\x1b[%d;0HPort %d  %-6s%-54s", row, p + 1,
                       st[p].err == PAD_ERR_NO_CONTROLLER ? "none" : st[p].err == PAD_ERR_NOT_READY ? "wait" : "error",
                       "");
                printf("\x1b[%d;0H%-60s", row + 1, "");
                printf("\x1b[%d;0H%-60s", row + 2, "");
                if (st[p].err == PAD_ERR_NO_CONTROLLER && report.fields % 60 == 0)
                    PAD_Reset(PAD_CHAN0_BIT >> p); /* a controller plugged in later is found */
            }
        }
        report.rumble = rumble;
        report.fields++;
        VIDEO_WaitVSync();
    }
    return 0;
}
