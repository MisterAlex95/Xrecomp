/* n64_vk_driver.h -- a Vulkan driver the player supplied (Mesa Turnip...) for the N64 Vulkan RDP (n64_rdp_vk.cpp).
 *
 * The app (ShellActivity) puts the selection in the environment before the runtime starts
 * (docs/runtime/environment-variables.md, « Custom Vulkan driver »):
 *   RECOMP_VK_DRIVER_DIR  absolute directory of the driver, app-private: <files>/vk_drivers/<id>/ (the trailing '/'
 *                         is added when missing); unset or empty = the system driver
 *   RECOMP_VK_DRIVER_LIB  the driver's library file in that directory (meta.json "libraryName"), no '/'
 *   RECOMP_VK_DRIVER_ID   the driver's id (the directory's name), for the log and the trial marker; default: the
 *                         directory's last component
 *   RECOMP_VK_HOOK_DIR    the app's native library directory (ApplicationInfo.nativeLibraryDir), where adrenotools'
 *                         hook libraries are; default: the directory libmain.so was loaded from
 *   RECOMP_VK_DRIVER      "system" forces the system driver whatever the others say
 *
 * The driver is opened through libxba_vkdriver.so (android/app/src/main/cpp/vkdriver.c, libadrenotools), dlopened
 * here only when a driver is selected. Off Android that library does not exist: the open fails and is logged, the
 * same path as a driver that cannot be opened on a device.
 *
 * Trial marker, against a driver that crashes instead of failing: before the open, "<id>\n" is written to
 * <driver dir>/../trying (<files>/vk_drivers/trying). n64_vk_driver_done removes it. If it is there at the next start
 * with the same id, the driver is skipped (logged) and the marker stays until the app clears it (the player picks a
 * driver again). The outcome of each start is written to <files>/vk_drivers/last_start as one line "<id> <status>",
 * status one of: ok (the RDP runs on the custom driver), failed (it could not be opened, or paraLLEl-RDP could not run
 * on it: the system driver was used), not_loaded (adrenotools fell back to the device's own driver), crashed (the
 * marker of a previous start was found: skipped). Both files are written only when the driver directory's parent is
 * named vk_drivers (never a file outside that layout). */
#ifndef N64_VK_DRIVER_H
#define N64_VK_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when a custom driver is selected (RECOMP_VK_DRIVER_DIR set and RECOMP_VK_DRIVER not "system"). */
int n64_vk_driver_selected(void);

/* The selected driver's vkGetInstanceProcAddr (a PFN_vkGetInstanceProcAddr), or NULL: none selected, forced system,
 * skipped by the trial marker, or the open failed (each logged on stderr except the first). Called once per RDP
 * create; when it returns non-NULL the marker is written and the caller must call n64_vk_driver_done. */
void *n64_vk_driver_open(void);

/* The selected driver's id ("" when none). */
const char *n64_vk_driver_id(void);

/* The outcome of the start that n64_vk_driver_open began: removes the marker, writes last_start
 * ("ok", "failed" or "not_loaded"). The adrenotools handle is never closed. */
void n64_vk_driver_done(const char *status);

#ifdef __cplusplus
}
#endif

#endif /* N64_VK_DRIVER_H */
