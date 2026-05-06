/* Pastime display-name + sort-key helpers.
 *
 * Pure string transforms — no I/O, no allocation.  The launcher feeds
 * raw filenames (or DB labels) through here at scan time to produce:
 *
 *   - display_name: what the user reads on a row.  Today: strip every
 *     trailing "(...)" and "[...]" block from No-Intro / Redump style
 *     names, then trim trailing whitespace and a hanging punctuation
 *     character.  No "the"-stripping, no case folding.
 *
 *   - sort_key: what qsort compares on.  Lowercased copy of the
 *     display name with a leading "the "/"a "/"an " article stripped so
 *     "The Legend of Zelda" sorts under L, not T.
 *
 * Designed to be the single hook for future cleanup smarts (preserve
 * (Disc N), pull from the DB label, collapse roman numerals, etc.) so
 * callers don't grow their own private rules. */

#ifndef PASTIME_DISPLAY_NAME_H
#define PASTIME_DISPLAY_NAME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a clean display name from `raw`.  Writes a NUL-terminated
 * string to `out`.  out_size must be >= 1.  Safe with raw == NULL or
 * empty: produces "".  Idempotent — passing an already-clean name
 * leaves it unchanged. */
void pastime_display_name_clean(const char *raw,
      char *out, size_t out_size);

/* Build a sort key for `display`.  Lowercased; leading article
 * ("the ", "a ", "an ") removed.  Result is for ordering only — never
 * shown to the user.  Safe with display == NULL or empty. */
void pastime_display_name_sort_key(const char *display,
      char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
