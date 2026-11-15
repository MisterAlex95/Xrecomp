/* n64_rdp_sw_names.h -- forced into n64_rdp.c / n64_rdp_stub.c (-include, consoles/n64/runtime/CMakeLists.txt): the
 * software RDP's public names become n64_rdp_sw_*, so it links next to the Vulkan RDP behind the dispatcher
 * n64_rdp_select.c (n64_rdp_backend.h). The sources themselves stay as they are. */
#ifndef N64_RDP_SW_NAMES_H
#define N64_RDP_SW_NAMES_H

#define n64_rdp n64_rdp_sw
#define n64_rdp_create n64_rdp_sw_create
#define n64_rdp_destroy n64_rdp_sw_destroy
#define n64_rdp_reset n64_rdp_sw_reset
#define n64_rdp_attach_dmem n64_rdp_sw_attach_dmem
#define n64_rdp_read_reg n64_rdp_sw_read_reg
#define n64_rdp_write_reg n64_rdp_sw_write_reg
#define n64_rdp_run n64_rdp_sw_run
#define n64_rdp_set_threads n64_rdp_sw_set_threads
#define n64_rdp_flush n64_rdp_sw_flush
#define n64_rdp_hidden_bits n64_rdp_sw_hidden_bits
#define n64_rdp_get_stats n64_rdp_sw_get_stats
#define n64_rdp_state_extra_size n64_rdp_sw_state_extra_size
#define n64_rdp_state_save_sw n64_rdp_sw_state_save
#define n64_rdp_state_load_sw n64_rdp_sw_state_load
/* n64_rdp.h's back-end selection belongs to the dispatcher: renamed out of the way in the software RDP's view. */
#define n64_rdp_set_backend n64_rdp_sw_unused_set_backend
#define n64_rdp_backend_name n64_rdp_sw_unused_backend_name
#define n64_rdp_backend_parse n64_rdp_sw_unused_backend_parse

#endif
