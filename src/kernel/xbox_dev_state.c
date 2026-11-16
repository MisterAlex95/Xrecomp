/*
 * xbox_dev_state.c - the device sections of an Xbox save state (XBA-248). See xbox_dev_state.h.
 *
 * The NV2A section is assembled here from its parts (register aperture, walker, executor); the APU and
 * USB sections are their libraries' own (apu_core.c, ohci.c). Freeze / Thaw hold the three device
 * threads around a capture or a restore.
 */
#include "xbox_dev_state.h"

#include <stdio.h>

#if !defined(_WIN32)
#include "platform/recomp_states.h"   /* recomp_states_note_load: frame generation's history */
#endif

#define NV2A_SECTION_MAGIC 0x41325641u   /* "AV2A" */

/* Whether the NV2A poll thread is held by xbox_DevStateFreeze (else prepare and after_load ran here). */
static int s_poll_held;

int xbox_Nv2aStateCapture(XdsWriter *w)
{
    xds_put_u32(w, NV2A_SECTION_MAGIC);
    xbox_Nv2aApertureSave(w);
    nv2a_pb_scan_state_save(w);
    nv2a_pb_exec_state_save(w);
    return !w->err;
}

int xbox_Nv2aStateRestore(XdsReader *r, uint32_t version)
{
    if (version != XDS_NV2A_VERSION || xds_get_u32(r) != NV2A_SECTION_MAGIC) {
        xds_fail(r);
        return 0;
    }
    if (!xbox_Nv2aApertureLoad(r) || !nv2a_pb_scan_state_load(r) || !nv2a_pb_exec_state_load(r))
        return 0;
    return !r->err;
}

static const XdsSection k_sections[] = {
    { XDS_TAG_NV2A, XDS_NV2A_VERSION, "NV2A", xbox_Nv2aStateCapture, xbox_Nv2aStateRestore },
    { XDS_TAG_APU,  XDS_APU_VERSION,  "APU",  mcpx_apu_state_capture, mcpx_apu_state_restore },
    { XDS_TAG_USB,  XDS_USB_VERSION,  "USB",  xbox_OhciStateCapture, xbox_OhciStateRestore },
};

const XdsSection *xbox_DevStateSections(int *count)
{
    if (count)
        *count = (int)(sizeof k_sections / sizeof k_sections[0]);
    return k_sections;
}

int xbox_DevStateFreeze(void)
{
    /* The NV2A first: its prepare may wait for the GPU, and nothing it does needs the others. A poll
     * thread that is not there (no aperture yet, a test) leaves prepare to this thread. */
    int nv = xbox_Nv2aPollHold(1, 0);

    s_poll_held = nv == 1;
    if (nv < 0)
        nv2a_pb_exec_state_prepare();
    else if (nv == 0)
        fprintf(stderr, "  [STATE] the NV2A poll thread did not stop within 2 s\n");
    mcpx_apu_state_hold(1);
    /* A controller thread that is not running (no TIB for it) holds nothing: not an error. */
    xbox_OhciStateHold(1);
    return nv != 0;
}

void xbox_DevStateThaw(int loaded)
{
    xbox_OhciStateHold(0);
    mcpx_apu_state_hold(0);
    if (s_poll_held)
        xbox_Nv2aPollHold(0, loaded);
    else if (loaded)
        nv2a_pb_exec_state_after_load();
    s_poll_held = 0;
#if !defined(_WIN32)
    if (loaded)
        recomp_states_note_load();     /* the presenter's frame generation history starts again */
#endif
}

int xbox_DevStateCaptureAll(XdsWriter *w)
{
    size_t i;

    for (i = 0; i < sizeof k_sections / sizeof k_sections[0]; i++) {
        size_t at = xds_section_begin(w, k_sections[i].tag, k_sections[i].version);
        if (!k_sections[i].capture(w)) {
            fprintf(stderr, "  [STATE] %s: capture failed\n", k_sections[i].name);
            return 0;
        }
        xds_section_end(w, at);
    }
    return !w->err;
}

int xbox_DevStateRestoreAll(XdsReader *r)
{
    uint32_t tag, version;
    XdsReader payload;

    while (xds_section_next(r, &tag, &version, &payload)) {
        size_t i;
        for (i = 0; i < sizeof k_sections / sizeof k_sections[0]; i++)
            if (k_sections[i].tag == tag)
                break;
        if (i == sizeof k_sections / sizeof k_sections[0])
            continue;                          /* another component's section */
        if (!k_sections[i].restore(&payload, version) || payload.err) {
            fprintf(stderr, "  [STATE] %s: section refused (version %u, %zu bytes)\n", k_sections[i].name,
                    version, payload.len);
            return 0;
        }
    }
    return !r->err;
}
