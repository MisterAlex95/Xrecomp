/* GBA pad (XBA-157): an interactive check of the controls and of saves on a real host (desktop window, the Android
 * app), not a headless test. Mode 0 text: the ten keys, lit while held; a marker moved by the D-pad; A pressed counts
 * up in SRAM (the "SRAM_V" library string below makes save detection pick 32 KB SRAM), so the count survives a quit
 * and a restart once the host writes the save. Writes its result block (PASS) once the screen is set up, then keeps
 * running. Our own code. */
#include <stdio.h>
#include <tonc.h>

#include "../common/xba_result.h"

/* The library ID string save detection looks for (the GBA SDK's SRAM library puts the same one in a game). */
__attribute__((used)) static const char save_id[] = "SRAM_V113";

#define SAVE_MAGIC 0x50 /* 'P': the counter below is ours */

static volatile u32 vblanks;
static void on_vblank(void)
{
    vblanks++;
    XBA_RESULT->frames = vblanks;
}

/* SRAM is 8 bits wide: byte accesses only. */
static u32 save_read(void)
{
    if (sram_mem[0] != SAVE_MAGIC)
        return 0;
    return sram_mem[1] | sram_mem[2] << 8;
}

static void save_write(u32 n)
{
    sram_mem[1] = (u8)n;
    sram_mem[2] = (u8)(n >> 8);
    sram_mem[0] = SAVE_MAGIC;
}

static const struct {
    u32 key;
    const char *name;
} keys[10] = {
    {KEY_A, "A"},           {KEY_B, "B"},       {KEY_L, "L"},       {KEY_R, "R"},       {KEY_START, "START"},
    {KEY_SELECT, "SELECT"}, {KEY_UP, "UP"},     {KEY_DOWN, "DOWN"}, {KEY_LEFT, "LEFT"}, {KEY_RIGHT, "RIGHT"},
};

int main(void)
{
    xba_begin(XBA_FOURCC('P', 'A', 'D', ' '));
    irq_init(NULL);
    irq_add(II_VBLANK, on_vblank);

    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;
    tte_init_se_default(0, BG_CBB(0) | BG_SBB(31));
    tte_write("#{P:8,4}XBA PAD + SAVE");
    tte_write("#{P:8,132}A: count +1 (saved)");
    tte_write("#{P:8,144}D-pad: move the *");

    u32 count = save_read();
    int mx = 160, my = 60, ox = -1, oy = -1;
    char line[40];
    xba_check(0, (u32)save_id[0], 'S');
    XBA_RESULT->status = XBA_PASS; /* set up: the headless corpus run stops here */

    for (;;) {
        VBlankIntrWait();
        key_poll();
        if (key_hit(KEY_A)) {
            count++;
            save_write(count);
        }
        for (int i = 0; i < 10; i++) {
            int held = key_is_down(keys[i].key) != 0;
            snprintf(line, sizeof line, "#{P:8,%d}%s %-6s", 20 + i * 10, held ? ">" : " ", held ? keys[i].name : "-");
            tte_write(line);
        }
        mx += key_tri_horz() * 2;
        my += key_tri_vert() * 2;
        mx = mx < 96 ? 96 : mx > 224 ? 224 : mx;
        my = my < 20 ? 20 : my > 120 ? 120 : my;
        if (mx != ox || my != oy) {
            if (ox >= 0) {
                snprintf(line, sizeof line, "#{P:%d,%d} ", ox, oy);
                tte_write(line);
            }
            snprintf(line, sizeof line, "#{P:%d,%d}*", mx, my);
            tte_write(line);
            ox = mx;
            oy = my;
        }
        snprintf(line, sizeof line, "#{P:184,4}F%-6lu", (unsigned long)(vblanks % 1000000));
        tte_write(line);
        snprintf(line, sizeof line, "#{P:168,132}%-5lu", (unsigned long)count);
        tte_write(line);
    }
}
