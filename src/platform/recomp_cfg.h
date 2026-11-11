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

/* A title's forced options (title.json "env_force", docs/runtime/
 * environment-variables.md « Forced by a title »): unlike recomp.cfg they win
 * over everything already set (the environment, recomp.cfg, the launcher's
 * settings), because the title cannot run right otherwise (Burnout 3 runs in
 * slow motion under frame generation's 30 fps cap).
 *
 * Allowed: a key RECOMP_[A-Z0-9_]+ shorter than RECOMP_CFG_FORCE_KEY_MAX, a
 * value of printable ASCII shorter than RECOMP_CFG_FORCE_VALUE_MAX
 * (tools/prepare/pack.py env_force_of applies the same rule). */
#define RECOMP_CFG_FORCE_KEY_MAX   64
#define RECOMP_CFG_FORCE_VALUE_MAX 256

/* 1 when key / value is allowed in env_force, else 0. */
int recomp_cfg_force_key_ok(const char *key);
int recomp_cfg_force_value_ok(const char *value);

/* Sets key=value over any existing value and prints
 * "[TITLE] <tid> forces KEY=VALUE (title profile)" on stdout. Returns 0, or
 * -1 (one line on stderr, nothing set) when key or value is not allowed. */
int recomp_cfg_force(const char *tid, const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_CFG_H */
