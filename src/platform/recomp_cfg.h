/* RECOMP_* options from a file (XBA-26).
 *
 * The runtime reads its switches with getenv("RECOMP_..."). On Android there
 * is no shell to set them, so the app reads a recomp.cfg file and puts each
 * line in the environment before any library starts. Works on every host.
 *
 * Format: one KEY=VALUE per line. Blank lines and lines starting with '#' are
 * skipped; spaces around the key and the value are trimmed; CRLF is fine.
 * A variable already in the environment wins over the file (an explicit
 * override from a shell stays one). */
#ifndef XBOXRECOMP_RECOMP_CFG_H
#define XBOXRECOMP_RECOMP_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Reads `path` and sets the options it holds. Returns the number of options
 * found (set, or kept from the environment), 0 when the file does not exist (not an error, nothing printed), -1 if
 * it exists but cannot be read. Prints one summary line on stdout
 * ("recomp.cfg: N option(s) from PATH: K=V, ...") and one line on stderr per
 * malformed line. */
int recomp_cfg_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_CFG_H */
