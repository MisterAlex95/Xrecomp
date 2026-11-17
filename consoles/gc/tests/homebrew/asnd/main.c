/* asnd: the GameCube sound test (XBA-263). libogc's ASND library (its own DSP microcode, mixing at 48 kHz into the
 * audio DMA) plays five one-second scenes, timed by VI fields from the first one:
 *
 *   1  1000 Hz sine, mono 16-bit at 48000 Hz, left only (volume 255 / 0)
 *   2   500 Hz sine, mono 8-bit at 12000 Hz, right only (0 / 255)
 *   3  stereo 16-bit at 48000 Hz: 1000 Hz on the left, 1500 Hz on the right (255 / 255)
 *   4  scene 1's voice at 128 / 128 and scene 2's at 128 / 128 together (both channels, both tones)
 *   5  silence (every voice stopped)
 *
 * Every tone is a looped table (ASND_SetInfiniteVoice), amplitude 16000. The headless test records the machine's
 * 48 kHz output (gc_run --wav) and compares each scene with consoles/gc/tests/audio_check.py's reference (levels per
 * channel, dominant frequency). The screen shows the scene and libasnd's state; the same in `report` (magic
 * "XBAASND1"). Only our own code and libogc: nothing of Nintendo's. */
#include <asndlib.h>
#include <gccore.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct report {
    char magic[8] __attribute__((nonstring));
    uint32_t scene;     /* 1..5, 6 when done */
    uint32_t rate;      /* ASND_GetAudioRate() */
    uint32_t fields;    /* fields since the first scene started */
    int32_t status[4];  /* ASND_StatusVoice of voices 0..3 during scene 4 */
    uint32_t time_ms;   /* ASND_GetTime() at the end */
};
volatile struct report report __attribute__((aligned(32))) = {.magic = "XBAASND1"};

#define AMP 16000.0
static int16_t sine_1k[48] __attribute__((aligned(32)));        /* one period at 48000 Hz: 1000 Hz */
static int8_t sine_8bit[96] __attribute__((aligned(32)));       /* four periods of 24: 500 Hz at 12000 Hz */
static int16_t stereo[96 * 2] __attribute__((aligned(32)));     /* L 1000 Hz (2 periods), R 1500 Hz (3) at 48000 */

static void *xfb;
static GXRModeObj *rmode;

static void tables(void)
{
    for (int i = 0; i < 48; i++)
        sine_1k[i] = (int16_t)lrint(AMP * sin(2 * M_PI * i / 48.0));
    for (int i = 0; i < 96; i++)
        sine_8bit[i] = (int8_t)lrint(AMP / 256.0 * sin(2 * M_PI * i / 24.0));
    for (int i = 0; i < 96; i++) {
        stereo[2 * i] = (int16_t)lrint(AMP * sin(2 * M_PI * i / 48.0));
        stereo[2 * i + 1] = (int16_t)lrint(AMP * sin(2 * M_PI * i / 32.0));
    }
    DCFlushRange(sine_1k, sizeof sine_1k);
    DCFlushRange(sine_8bit, sizeof sine_8bit);
    DCFlushRange(stereo, sizeof stereo);
}

static void scene(int n)
{
    for (int v = 0; v < 4; v++)
        ASND_StopVoice(v);
    switch (n) {
    case 1:
        ASND_SetInfiniteVoice(0, VOICE_MONO_16BIT, 48000, 0, sine_1k, sizeof sine_1k, 255, 0);
        break;
    case 2:
        ASND_SetInfiniteVoice(1, VOICE_MONO_8BIT, 12000, 0, sine_8bit, sizeof sine_8bit, 0, 255);
        break;
    case 3:
        ASND_SetInfiniteVoice(2, VOICE_STEREO_16BIT, 48000, 0, stereo, sizeof stereo, 255, 255);
        break;
    case 4:
        ASND_SetInfiniteVoice(0, VOICE_MONO_16BIT, 48000, 0, sine_1k, sizeof sine_1k, 128, 128);
        ASND_SetInfiniteVoice(1, VOICE_MONO_8BIT, 12000, 0, sine_8bit, sizeof sine_8bit, 128, 128);
        break;
    default:
        break;
    }
    report.scene = (uint32_t)n;
}

int main(void)
{
    VIDEO_Init();
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

    tables();
    ASND_Init();
    ASND_Pause(0);
    report.rate = (uint32_t)ASND_GetAudioRate();
    printf("\x1b[2;0HXRecomp GameCube sound test (XBA-263)\n");
    printf("ASND at %u Hz\n", (unsigned)report.rate);

    /* half a second of silence first, so the test sees where scene 1 starts */
    for (int f = 0; f < 30; f++)
        VIDEO_WaitVSync();
    for (int n = 1; n <= 5; n++) {
        scene(n);
        printf("\x1b[5;0Hscene %d  ", n);
        for (int f = 0; f < 60; f++) {
            VIDEO_WaitVSync();
            report.fields++;
            if (n == 4 && f == 30)
                for (int v = 0; v < 4; v++)
                    report.status[v] = ASND_StatusVoice(v);
        }
    }
    report.time_ms = ASND_GetTime();
    report.scene = 6;
    printf("\x1b[6;0Hdone, ASND time %u ms\n", (unsigned)report.time_ms);
    for (;;)
        VIDEO_WaitVSync();
    return 0;
}
