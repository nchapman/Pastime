/* Pastime ROM-row label disambiguation.
 *
 * The launcher's "system overlay" model can produce a system whose
 * ROM list contains multiple rows that clean to the same display name
 * — either because two ROM filenames in one folder differ only by a
 * stripped tag ("Ape Escape (USA).chd" vs "Ape Escape (USA) (Demo
 * 1).chd") or because two folders sharing a system display name each
 * contain a copy of the same game ("Crash Bandicoot.chd" in a
 * pcsx_rearmed source AND in an ext-duckstation source).  Either way,
 * the user would otherwise see indistinguishable rows.
 *
 * This module rewrites the colliding rows' labels with the *minimum*
 * extra information needed to tell them apart:
 *
 *   - intra-source collisions (same source folder): per-row tail of
 *     the original tag past the longest common bracket-block prefix,
 *     so "Ape Escape (USA)" + "Ape Escape (USA) (Demo 1)" become
 *     "Ape Escape" and "Ape Escape (Demo 1)" — the bare row reads as
 *     "the default", siblings carry only what differs.
 *
 *   - cross-source collisions: per-row source short label
 *     ("Crash Bandicoot (pcsx_rearmed)" vs "(duckstation)").
 *
 *   - mixed runs (cross-source AND multiple rows sharing a source):
 *     source label first, then within each same-source sub-run a
 *     second-level tag-tail differential so same-source siblings stay
 *     distinguishable.
 *
 * Pure C, no allocation other than the new label buffer (the old
 * display_name pointer is freed and replaced).  Pre-sort contract:
 * the caller must arrange `rows` so that equal *display_name values
 * are contiguous — the launcher already qsort's by sort_key, so
 * passing the post-sort list satisfies this.  Single-occurrence rows
 * are not modified.
 *
 * Not coupled to RetroArch types — the menu driver builds a small
 * pastime_disambig_row_t array referencing its own ROM structs, and
 * provides a callback that maps source_idx back to a short label
 * (core_ident for libretro sources, preset shortname for external).
 */

#ifndef PASTIME_DISAMBIG_H
#define PASTIME_DISAMBIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
   /* Caller-owned. The disambig pass may free *display_name and
    * replace it with a freshly malloc'd label; the slot itself
    * persists across the call. */
   char       **display_name;
   /* The original parenthetical tag stripped during display-name
    * cleaning, e.g. "(USA) (Rev 1)".  NULL when the source filename
    * had no trailing tag.  Read-only. */
   const char  *tag;
   /* Index used to look up the source short label via the resolver
    * callback.  Capped at 255 by convention to match the launcher's
    * pastime_rom_t.source_idx (uint8_t); this struct uses uint8_t
    * for the same invariant. */
   uint8_t      source_idx;
} pastime_disambig_row_t;

/* Resolves a source_idx to the user-facing short label used in
 * cross-source qualifiers ("pcsx_rearmed", "duckstation").  Return
 * NULL when no label applies — the disambig pass leaves rows whose
 * source resolves to NULL without a source qualifier. */
typedef const char *(*pastime_disambig_source_label_fn)(
      uint8_t source_idx, void *user);

/* Walk `rows` (length `n`, pre-sorted so equal *display_name values
 * are contiguous), find runs of duplicates, and rewrite each row's
 * *display_name with the minimum disambiguating qualifier per the
 * file-header rules.  `resolve_source_label` is called only for rows
 * inside a cross-source run; pass NULL if the caller never has
 * cross-source overlays (the pass then degrades to intra-source
 * tag-tail only).  Safe with n == 0 or rows == NULL. */
void pastime_disambig_run(pastime_disambig_row_t *rows, size_t n,
      pastime_disambig_source_label_fn resolve_source_label,
      void *user);

#ifdef __cplusplus
}
#endif

#endif /* PASTIME_DISAMBIG_H */
