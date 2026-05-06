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
   const char *shortname;   /* derived from Daijishou's `name`, sanitised
                             * to [a-z0-9]+, e.g. "duckstation" — or NULL
                             * if no `name` was present.  The user-facing
                             * marker form: (ext-duckstation). */
   const char *component;   /* class — "<.Relative>" or "<fully.Qualified>" */
   const char *action;      /* e.g. "android.intent.action.VIEW", or NULL */
   const char *category;    /* e.g. "android.intent.category.DEFAULT", or NULL */
   const char *extra_key;   /* see header comment; NULL = setData(uri) */
   const char *mime_type;   /* MIME on the data URI, or NULL */
   bool        kill_first;  /* forceStop the package before startActivity */
} pastime_external_spec_t;

/* If `inside_parens` starts with "ext-", parse the payload as either:
 *   - a full Android package: `[a-zA-Z0-9._]+` containing at least one '.'
 *     (e.g. "com.github.stenzek.duckstation"), or
 *   - a shortname: `[a-z0-9]+` with no '.' (e.g. "duckstation").
 * The dot is the disambiguator.  Returns true on either valid form and
 * sets `*payload_out` to the substring after "ext-" (caller owns the
 * buffer; the pointer aliases into it).  The caller dispatches to
 * pastime_external_lookup or pastime_external_lookup_shortname based on
 * which form was parsed.
 *
 * Returns false (and leaves *payload_out unset) if the marker isn't an
 * `ext-` form or the payload fails validation.  Caller should fall
 * through to libretro `(<core_ident>)` parsing on false.
 */
bool pastime_external_parse_marker(const char *inside_parens,
      const char **payload_out);

/* True iff `payload` looks like a shortname rather than a full package —
 * i.e. it has no `.`.  Cheap helper so callers don't have to re-scan. */
bool pastime_external_payload_is_shortname(const char *payload);

/* O(N) scan of the generated preset table for `package` (exact match,
 * case-sensitive — Android package names are case-sensitive identifiers).
 * Returns NULL on miss; the caller surfaces an "unsupported emulator"
 * toast at launch time. */
const pastime_external_spec_t *pastime_external_lookup(const char *package);

/* O(N) scan for the first preset whose `shortname` exactly equals
 * `shortname` (case-sensitive — the sync script lowercases at generation
 * time so any uppercase input from the user is a parse error upstream).
 * Returns NULL on miss.  If multiple presets share a shortname (today:
 * none — the catalog is collision-free) this returns the first in
 * package-sorted order; runtime install-check disambiguation is a future
 * extension if a collision ever materialises. */
const pastime_external_spec_t *pastime_external_lookup_shortname(
      const char *shortname);

/* True iff the named Android package is currently installed on this
 * device.  Always returns true on non-Android builds (we don't want
 * desktop dev to lose visibility of external folders).  Used by the
 * menu driver to hide system folders whose external app the user
 * hasn't installed — same UX as libretro folders whose core isn't on
 * the buildbot.  Synchronous JNI hop; cheap (sub-ms per call). */
bool pastime_external_is_installed(const char *package);

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
