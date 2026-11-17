/* aesnd: the GameCube sound test for libogc's AESND library (XBA-266). Its own DSP microcode mixes 2 ms buffers at
 * the DSP's rate (~48 kHz) into the audio DMA; the program plays the same five one-second scenes as asnd (so
 * consoles/gc/tests/audio_check.py checks both with one scene list), timed by VI fields from the first one:
 *
 *   1  1000 Hz sine, mono 16-bit, left only (volume 255 / 0)
 *   2   500 Hz sine, mono 8-bit at a quarter of the rate, right only (0 / 255)
 *   3  stereo 16-bit: 1000 Hz on the left, 1500 Hz on the right (255 / 255)
 *   4  scene 1's voice at 128 / 128 and scene 2's at 128 / 128 together (both channels, both tones)
 *   5  silence (every voice stopped)
 *
 * Each tone is a looped table at amplitude 16000 played at the DSP's own rate (DSP_DEFAULT_FREQ, or a quarter of
 * it), so one table sample is one output sample (or four): the tones are exact at whatever rate the audio interface
 * really runs. The headless test records the machine's 48 kHz output (gc_run --wav). The screen shows the scene; the
 * same in `report` (magic "XBAAESN1"). Only our own code and libogc: nothing of Nintendo's. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type" /* libogc's header casts its callbacks */
#include <aesndlib.h>
#pragma GCC diagnostic pop
#include <gccore.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct report {
    char magic[8] __attribute__((nonstring));
    uint32_t scene;       /* 1..5, 6 when done */
    uint32_t fields;      /* fields since the first scene started */
    uint32_t dsp_time;    /* AESND_GetDSPProcessTime() at the end */
};
volatile struct report report __attribute__((aligned(32))) = {.magic = "XBAAESN1"};

#define AMP 16000.0
/* libaesnd streams a voice through a ring of 2 x DSP_STREAMBUFFER_SIZE (1152) bytes in ARAM and pads whatever is
 * short of a ring half with silence: a looped table here is a whole number of periods filling 2304 bytes */
#define TABLE_BYTES (2 * DSP_STREAMBUFFER_SIZE)
static int16_t sine_1k[TABLE_BYTES / 2] __attribute__((aligned(32))); /* 1000 Hz at 48 kHz: 24 periods */
static int8_t sine_8bit[TABLE_BYTES] __attribute__((aligned(32)));    /* 500 Hz at a quarter rate: 96 periods */
static int16_t stereo[TABLE_BYTES / 2] __attribute__((aligned(32)));  /* L 1000 Hz (12 periods), R 1500 Hz (18) */

static AESNDPB *voice[3];
static void *xfb;
static GXRModeObj *rmode;

static void tables(void)
{
    for (int i = 0; i < TABLE_BYTES / 2; i++)
        sine_1k[i] = (int16_t)lrint(AMP * sin(2 * M_PI * i / 48.0));
    for (int i = 0; i < TABLE_BYTES; i++)
        sine_8bit[i] = (int8_t)lrint(AMP / 256.0 * sin(2 * M_PI * i / 24.0));
    for (int i = 0; i < TABLE_BYTES / 4; i++) {
        stereo[2 * i] = (int16_t)lrint(AMP * sin(2 * M_PI * i / 48.0));
        stereo[2 * i + 1] = (int16_t)lrint(AMP * sin(2 * M_PI * i / 32.0));
    }
    DCFlushRange(sine_1k, sizeof sine_1k);
    DCFlushRange(sine_8bit, sizeof sine_8bit);
    DCFlushRange(stereo, sizeof stereo);
}

static void play(int v, u32 fmt, const void *buf, u32 len, f32 freq, u16 l, u16 r)
{
    AESND_SetVoiceVolume(voice[v], l, r);
    AESND_PlayVoice(voice[v], fmt, buf, len, freq, 0, true);
}

static void scene(int n)
{
    for (int v = 0; v < 3; v++)
        AESND_SetVoiceStop(voice[v], true);
    switch (n) {
    case 1:
        play(0, VOICE_MONO16, sine_1k, sizeof sine_1k, DSP_DEFAULT_FREQ, 255, 0);
        break;
    case 2:
        play(1, VOICE_MONO8, sine_8bit, sizeof sine_8bit, DSP_DEFAULT_FREQ / 4, 0, 255);
        break;
    case 3:
        play(2, VOICE_STEREO16, stereo, sizeof stereo, DSP_DEFAULT_FREQ, 255, 255);
        break;
    case 4:
        play(0, VOICE_MONO16, sine_1k, sizeof sine_1k, DSP_DEFAULT_FREQ, 128, 128);
        play(1, VOICE_MONO8, sine_8bit, sizeof sine_8bit, DSP_DEFAULT_FREQ / 4, 128, 128);
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
    AESND_Init();
    for (int v = 0; v < 3; v++)
        voice[v] = AESND_AllocateVoice(NULL);
    AESND_Pause(false);
    printf("\x1b[2;0HXRecomp GameCube AESND test (XBA-266)\n");

    /* half a second of silence first, so the test sees where scene 1 starts */
    for (int f = 0; f < 30; f++)
        VIDEO_WaitVSync();
    for (int n = 1; n <= 5; n++) {
        scene(n);
        printf("\x1b[5;0Hscene %d  ", n);
        for (int f = 0; f < 60; f++) {
            VIDEO_WaitVSync();
            report.fields++;
        }
    }
    report.dsp_time = AESND_GetDSPProcessTime();
    report.scene = 6;
    printf("\x1b[6;0Hdone, DSP time %u\n", (unsigned)report.dsp_time);
    for (;;)
        VIDEO_WaitVSync();
    return 0;
}
