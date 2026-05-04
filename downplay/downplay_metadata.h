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

/* ---------- per-system index ---------- */

/* Opaque per-system metadata index.  Owns:
 *  - the JSON file at <RA_config>/downplay/index/<system_folder>.json
 *  - an open libretrodb_t* for the resolved db_name's RDB (or NULL)
 *  - a dynamic array of (basename, mtime, size, label, ...) entries
 *  - a queue of basenames pending CRC/serial match
 *  - a dirty flag for debounced flush
 *
 * Lifetime: open on system view enter, close on view exit.  Holding the
 * libretrodb handle across a session amortizes file-open cost; per-file
 * matching cost drops from ~50ms to ~10ms.  See plan
 * (.../this-looks-great-turn-functional-lollipop.md) for the full
 * reuse map and rationale. */
typedef struct downplay_index downplay_index_t;

enum downplay_match_kind
{
   DP_MATCH_NONE = 0,
   DP_MATCH_CRC,
   DP_MATCH_SERIAL
};

enum downplay_art_state
{
   DP_ART_UNKNOWN = 0,
   DP_ART_OK,
   DP_ART_MISSING
};

/* Snapshot of an index entry.  The `const char*` pointers reference
 * storage owned by the index; read-only, never free.
 *
 * Lifetime contract: pointers are valid only within the same call
 * frame as the lookup that returned them.  Do NOT store across a
 * subsequent downplay_index_pump / note_present / set_art_state /
 * close call — the apply path can free and replace the underlying
 * label/match_value strings.  In practice all current callers (the
 * menu driver's per-row render) read once and discard, which is
 * fine. */
typedef struct
{
   const char              *label;        /* may be NULL — caller falls back to filename */
   const char              *match_value;  /* may be NULL — hex CRC or serial string */
   enum downplay_match_kind match_kind;
   enum downplay_art_state  art_state;
} downplay_index_record_t;

/* Open (or create) the index for a system folder.
 *
 * system_folder_name is the "Display Name (core_ident)" string used as
 * the JSON filename stem.  system_root_path is the absolute on-disk
 * path of the folder (used to locate ROM files for matching).  db_name
 * is the resolved libretro-thumbnails system name (used to locate the
 * .rdb).  db_name may be NULL/empty: matching is then disabled, but
 * the index still works for art-state caching.
 *
 * Returns NULL on allocation failure or if the index root path can't
 * be resolved. */
downplay_index_t *downplay_index_open(const char *system_folder_name,
      const char *system_root_path, const char *db_name);

/* Close the index, flushing any dirty state to disk.  Safe to call with
 * NULL. */
void downplay_index_close(downplay_index_t *idx);

/* Look up an entry by basename.  Validates against (mtime, size); on
 * mismatch the cached record is invalidated and the entry re-enqueued.
 * Returns true if the entry is known to the index AND the validity
 * check passes; false otherwise (caller renders filename, no art).
 *
 * Pure read; never blocks, never does I/O. */
bool downplay_index_lookup(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size,
      downplay_index_record_t *out);

/* Note that an entry exists in the filesystem.  Idempotent.  If the
 * entry is unknown, creates a stub and enqueues it for matching.  If
 * (mtime, size) differs from the cached entry, invalidates and re-
 * enqueues.  Call once per ROM at scan time. */
void downplay_index_note_present(downplay_index_t *idx,
      const char *basename, time_t mtime, int64_t size);

/* End-of-scan reconciliation: prune any cached entries that weren't
 * note_present()'d this scan.  Marks the index dirty if anything was
 * removed.  Call once after the per-ROM note_present loop. */
void downplay_index_finish_scan(downplay_index_t *idx);

/* Set the art state for an entry.  Idempotent; marks dirty only if the
 * value actually changed.  Stamps art_checked_at = now() for future
 * "refresh missing art after N days" support. */
void downplay_index_set_art_state(downplay_index_t *idx,
      const char *basename, enum downplay_art_state state);

/* Process up to max_ops queued match operations.  Each op:
 *   - dispatches CRC32 or serial extraction by file extension
 *     (mirrors tasks/task_database.c:716 task_database_iterate_playlist)
 *   - opens a cursor on the cached libretrodb handle with a CRC or
 *     serial query (verbatim format from task_database.c:1199-1201,
 *     :1374-1378)
 *   - reads the first matched record's `name` field as the canonical
 *     label
 *
 * Returns the number of ops actually performed (0 when queue empty).
 * Call from the menu driver's per-frame callback with a small budget
 * (1-2 ops/frame at 60fps gives ~10-30 enrichments per second). */
int downplay_index_pump(downplay_index_t *idx, int max_ops);

/* Force-flush dirty state to disk via atomic temp+rename.  Otherwise
 * auto-flushed on close.  Cheap when clean (no-op). */
void downplay_index_flush(downplay_index_t *idx);

/* ---------- box-art download ---------- */

/* Asynchronously fetch the boxart PNG for (system, label) from
 * libretro-thumbnails into the user's directory_thumbnails location.
 * No-op if the local file already exists or required state is missing.
 *
 * We have to roll this ourselves because RA's
 * task_push_pl_entry_thumbnail_download hard-requires a playlist_t with
 * a backing config path, and gfx_thumbnail_request_stream's on-demand
 * download path bails when given playlist=NULL (gfx_thumbnail.c:406).
 *
 * Implementation mirrors task_pl_thumbnail_download.c:241-243 (URL
 * format) + :358 (task_push_http_transfer_file) — same wire format,
 * same destination layout, just without the playlist plumbing. */
void downplay_metadata_request_boxart(const char *system, const char *label);

RETRO_END_DECLS

#endif
