/* Minimal single-buffer 320x240 display for the PS1 homebrew corpus (XBA-149). Our own code. */
#ifndef XBA_PS1_SCREEN_H
#define XBA_PS1_SCREEN_H

#include <psxgpu.h>

static DISPENV xba_disp;
static DRAWENV xba_draw;

/* Resets the GPU, shows VRAM (0,0)-(320,240) and clears it to the given colour. */
static inline void screen_init(int r, int g, int b)
{
    ResetGraph(0);
    SetDefDispEnv(&xba_disp, 0, 0, 320, 240);
    SetDefDrawEnv(&xba_draw, 0, 0, 320, 240);
    setRGB0(&xba_draw, r, g, b);
    xba_draw.isbg = 1;
    xba_draw.dtd = 0; /* no dithering: pixels stay exact for comparisons */
    PutDispEnv(&xba_disp);
    PutDrawEnv(&xba_draw);
    DrawSync(0);
    SetDispMask(1);
}

/* Green screen on pass, red on fail. */
static inline void screen_verdict(int pass)
{
    if (pass)
        screen_init(0, 160, 0);
    else
        screen_init(160, 0, 0);
}

#endif
