/* The LSFG bridge as the frame generation pacer's generator (lsfg_generator.c, framegen_pacer.h). */
#ifndef LSFG_GENERATOR_H
#define LSFG_GENERATOR_H

#include "framegen_pacer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* open/generate/reset/close over lsfg_bridge_*, configured from RECOMP_LSFG_DIR / _DLL / _FLOW / _PERF / _FP16. */
extern const FramegenGenerator lsfg_bridge_generator;

/* framegen_pacer_set_generator(&lsfg_bridge_generator): call before framegen_pacer_start(). */
void lsfg_bridge_register(void);

#ifdef __cplusplus
}
#endif

#endif /* LSFG_GENERATOR_H */
