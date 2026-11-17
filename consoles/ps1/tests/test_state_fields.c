/* Offsets of private device fields in their save state sections, for test_state.c (XBA-182): compiled once per device
 * (STATE_GPU, STATE_CDROM, STATE_SPU, STATE_MDEC, STATE_HLE) with that device's source included, as the device's
 * whole object: the test links this copy instead of the library's (same code, so the archive member is not pulled).
 * The sources cannot share one unit (their static helpers share names). */
#if defined(STATE_GPU)
#include "ps1_gpu.c"
size_t state_off_gpu_cmd_len(void) { return offsetof(ps1_gpu, cmd_len); }
#elif defined(STATE_CDROM)
#include "ps1_cdrom.c"
size_t state_off_cdrom_qhead(void) { return offsetof(ps1_cdrom, qhead); }
#elif defined(STATE_SPU)
#include "ps1_spu.c"
size_t state_off_spu_counter0(void) { return offsetof(ps1_spu, v) + offsetof(voice, counter); }
#elif defined(STATE_MDEC)
#include "ps1_mdec.c"
size_t state_off_mdec_blk_index(void) { return offsetof(ps1_mdec, blk_index); }
#elif defined(STATE_HLE)
#include "ps1_hle.c"
/* files[i].dev / .first */
static size_t hle_file_at(int i) { return offsetof(struct ps1_hle, files) + (size_t)i * sizeof(hle_file); }
size_t state_off_hle_file_dev(int i) { return hle_file_at(i) + offsetof(hle_file, dev); }
size_t state_off_hle_file_first(int i) { return hle_file_at(i) + offsetof(hle_file, first); }
#endif
