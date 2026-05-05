/*  Downplay - a fork of RetroArch.
 *  Copyright (C) 2026 - Downplay contributors.
 *
 *  Downplay is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Downplay is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Downplay. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DOWNPLAY_METADATA_H
#define DOWNPLAY_METADATA_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Map (display_name, core_ident) → libretro-thumbnails system name
 * (e.g. "Nintendo - Super Nintendo Entertainment System").
 *
 * Resolution order:
 *   1. Disambiguation table (curated; covers common abbreviations like
 *      "SNES", "Genesis", "PSX").  Case-insensitive exact match on the
 *      user's display_name half of "Display Name (core_ident)".
 *   2. Fall back to the first entry of the core's `database =` field
 *      (parsed from core_info via core_ident).  Multi-system cores
 *      (mgba, genesis_plus_gx, ...) are still useful via this path
 *      when the user picks the alphabetically-first child system.
 *
 * Returns a pointer to static / core_info-owned storage; caller must
 * NOT free.  Returns NULL when neither path resolves a name (no
 * thumbnail repo exists for this folder; art lookup will be skipped). */
const char *downplay_metadata_resolve_db_name(
      const char *display_name, const char *core_ident);

/* ---------- per-system art-state index ----------
 *
 * Persists `art_state` (OK / MISSING / UNKNOWN) per (system, basename)
 * tuple to disk so the row drawer can pre-truncate row labels for
 * known-art rows on system enter, before the thumbnail manager has
 * had a chance to look anything up.  Pure cache: tearing down the
 * file is always safe; the next session repopulates from
 * `downplay_thumbs_request` outcomes.
 *
 * Was historically a richer module (CRC/serial RDB matching for
 * canonical labels) — removed once the pastime.gg-backed thumbnail
 * pipeline took over both label resolution and art lookup. */
typedef struct downplay_index downplay_index_t;

enum downplay_art_state
{
   DP_ART_UNKNOWN = 0,
   DP_ART_OK,
   DP_ART_MISSING
};

/* Snapshot of an index entry.  Lifetime contract: the snapshot is
 * value-typed and stable across subsequent calls. */
typedef struct
{
   enum downplay_art_state art_state;
} downplay_index_record_t;

/* Open (or create) the index for a system folder.  `db_name` is
 * accepted for forward-compatibility (recorded in the JSON for
 * diagnostics) but no longer drives any matching logic.  Returns NULL
 * on allocation failure or if the index root path can't be resolved. */
downplay_index_t *downplay_index_open(const char *system_folder_name,
      const char *system_root_path, const char *db_name);

/* Close the index, flushing dirty state to disk.  Safe to call with
 * NULL. */
void downplay_index_close(downplay_index_t *idx);

/* Look up an entry by basename.  Validates against (mtime, size); on
 * mismatch returns false (caller treats as UNKNOWN).  Pure read; no
 * I/O, no blocking. */
bool downplay_index_lookup(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size,
      downplay_index_record_t *out);

/* Note that an entry exists in the filesystem.  Idempotent.  Creates
 * a stub if unknown; if (mtime, size) differs invalidates the cached
 * art_state.  Call once per ROM at scan time. */
void downplay_index_note_present(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size);

/* End-of-scan reconciliation: prune any cached entries that weren't
 * note_present()'d this scan.  Call once after the per-ROM
 * note_present loop. */
void downplay_index_finish_scan(downplay_index_t *idx);

/* Set the art state for an entry.  Idempotent; marks dirty only if
 * the value actually changed. */
void downplay_index_set_art_state(downplay_index_t *idx,
      const char *basename, enum downplay_art_state state);

/* Force-flush dirty state to disk via atomic temp+rename.  Otherwise
 * auto-flushed on close.  Cheap when clean (no-op). */
void downplay_index_flush(downplay_index_t *idx);

RETRO_END_DECLS

#endif
