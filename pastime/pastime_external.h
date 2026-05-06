/* Pastime external-emulator launch.
 *
 * A `Roms/<system>/` folder whose name ends in `(ext-<package>)` instead
 * of `(<core_ident>)` resolves to an external Android emulator app rather
 * than a libretro core.  At ROM-pick time we look the package up in a
 * generated preset table (see pastime_external_presets.h, synced from
 * Daijishou) and fire the matching Android Intent.
 *
 * Pure C; the Android intent firing lives in pastime_external_android.c
 * and is reached via pastime_external_launch().  On non-Android builds
 * the launch helper logs and returns false — folders with `(ext-…)`
 * markers still appear in the system list (so a desktop dev can see
 * them) but selecting one is a no-op.
 */

#ifndef PASTIME_EXTERNAL_H
#define PASTIME_EXTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One row in the generated preset table.  All strings are static — they
 * live in pastime_external_presets.h, never freed.  Mirrors the subset of
 * Daijishou's am-start template syntax we actually need:
 *
 *   - extra_key NULL  → setData(uri)            (e.g. Flycast, Drastic)
 *   - extra_key set   → putExtra(extra_key, uri) (e.g. Dolphin, DuckStation)
 *
 * Fields are populated from the platform JSON's `amStartArguments` token
 * stream; see pastime/tools/sync_external_presets.py for the parser.
 */
typedef struct
{
   const char *package;     /* Android package, e.g. "com.flycast.emulator" */
   const char *component;   /* class — "<.Relative>" or "<fully.Qualified>" */
   const char *action;      /* e.g. "android.intent.action.VIEW", or NULL */
   const char *category;    /* e.g. "android.intent.category.DEFAULT", or NULL */
   const char *extra_key;   /* see header comment; NULL = setData(uri) */
   const char *mime_type;   /* MIME on the data URI, or NULL */
   bool        kill_first;  /* forceStop the package before startActivity */
} pastime_external_spec_t;

/* If `inside_parens` starts with "ext-", validate the rest as an Android
 * package name (`[a-zA-Z0-9._]+` containing at least one '.') and return
 * a pointer to the package in `*package_out`.  The pointer aliases into
 * the input — caller owns the buffer's lifetime.
 *
 * Returns false (and leaves *package_out unset) if the marker isn't an
 * `ext-` form, or if the package fails validation.  The caller should
 * fall through to the existing `(<core_ident>)` parsing on false.
 */
bool pastime_external_parse_marker(const char *inside_parens,
      const char **package_out);

/* O(N) scan of the generated preset table for `package` (exact match,
 * case-sensitive — Android package names are case-sensitive identifiers).
 * Returns NULL on miss; the caller surfaces an "unsupported emulator"
 * toast at launch time. */
const pastime_external_spec_t *pastime_external_lookup(const char *package);

/* Fire the configured Intent with `rom_path` as the content payload.
 * Returns false if we couldn't dispatch — typical causes: package not
 * installed, or non-Android build (the cross-platform stub).  Caller
 * surfaces a user-visible error on false. */
bool pastime_external_launch(const pastime_external_spec_t *spec,
      const char *rom_path);

#ifdef __cplusplus
}
#endif

#endif
