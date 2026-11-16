/*
 * The APU model's save state (XBA-248): apu_savestate.c. The frame thread must be idle (or the model not
 * running) around both calls.
 */
#ifndef APU_SAVESTATE_H
#define APU_SAVESTATE_H

#include "kernel/xbox_dev_state.h"

typedef struct MCPXAPUState MCPXAPUState;

void mcpx_apu_model_save(const MCPXAPUState *d, XdsWriter *w);
int  mcpx_apu_model_load(MCPXAPUState *d, XdsReader *r);

/* apu_dsp.c: the GP / EP DSP memories in the MCPX aperture (0x30000-0x7FFFF, plain memory the title
 * writes) and the DSP handshake's state; load also clears the reverb's delay lines. */
void mcpx_apu_dsp_state_save(XdsWriter *w);
int  mcpx_apu_dsp_state_load(XdsReader *r);

#endif /* APU_SAVESTATE_H */
