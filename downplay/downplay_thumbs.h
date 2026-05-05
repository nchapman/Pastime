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

/* Per-system thumbnail manager backed by our own pastime.gg mirror.
 *
 * Replaces the libretro-thumbnails fetch path: instead of guessing a
 * URL and discovering misses by 404, we fetch a per-system index.json
 * once (https://thumbnails.pastime.gg/<system>/Named_Boxarts/index.json)
 * which authoritatively lists every available title.  Lookups are
 * then deterministic in-memory hits — no roundtrip to learn that a
 * title doesn't exist.
 *
 * Matching is filename-driven, deterministic, two-phase:
 *
 *   1. Exact-canonical hit (user named the ROM with the verbatim
 *      No-Intro / Redump key).
 *   2. Heavy-normalize the basename and bsearch on `by_heavy`:
 *        - strip (...) and [...] blocks (region/disc/rev/lang flags)
 *        - fold a small set of Latin-1 diacritics (é → e, ñ → n)
 *        - "&" → " and "
 *        - lowercase, drop articles, roman→arabic for II..IX
 *        - strip remaining non-alphanumerics
 *      This collapses the common spelling variants (case, spacing,
 *      punctuation, accent, "Megaman" vs "Mega Man") to a single
 *      shared key.
 *   3. If the equal-`heavy` range has multiple candidates, pick by:
 *        - reject bad dumps (Beta/Proto/Demo/...) unless that's all
 *          we have
 *        - prefer disc match if user named one
 *        - lowest region_score (USA > Europe > World > Japan)
 *        - highest rev_num
 *        - lex-smallest canonical (full determinism)
 *
 * No fuzzy scoring, no edit distance: the index is authoritative,
 * so a miss after the cascade is a real miss.  We can layer fuzzy
 * recall later as a separate fallback if telemetry shows it's
 * needed.
 *
 * Lifetime: open on system view enter, close on view exit.  Index
 * is cached on disk and refreshed on a generous TTL.  Image fetches
 * are kicked from the per-frame `_request` call; drain is driven by
 * the existing RA HTTP task queue. */

#ifndef DOWNPLAY_THUMBS_H
#define DOWNPLAY_THUMBS_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define DP_THUMBS_PATH_MAX 1024

typedef struct downplay_thumbs downplay_thumbs_t;

enum downplay_thumb_status
{
   DP_THUMB_UNKNOWN = 0, /* Index not loaded yet, or fetch in flight. */
   DP_THUMB_OK,          /* Image is on disk at `local_path`. */
   DP_THUMB_MISSING      /* Index loaded; title is not in it. */
};

typedef struct
{
   enum downplay_thumb_status status;
   char                       local_path[DP_THUMBS_PATH_MAX];
} downplay_thumb_result_t;

/* Open the manager for a canonical system name (e.g.
 * "Nintendo - Game Boy" — exactly the form
 * `downplay_metadata_resolve_db_name` returns).  Triggers an async
 * index fetch when the on-disk cache is missing or older than the
 * TTL; cheap when cached.  Returns NULL on system==NULL or empty. */
downplay_thumbs_t *downplay_thumbs_open(const char *system);

/* Boot-time fan-out: queue async index.json fetches for every
 * canonical system name in `systems` whose on-disk cache is missing
 * or older than the TTL.  Bounded concurrency; fire-and-forget.
 *
 * Indexes simply land on disk for a future `_open` to find.  Per-URL
 * dedup against any in-flight fetches issued by `_open` — opening a
 * system whose prefetch is mid-flight does NOT issue a duplicate
 * fetch; the open's pump-loop discovers the file when it lands.
 *
 * Safe with count==0 or systems==NULL. */
void downplay_thumbs_prefetch_indexes(
      const char * const *systems, size_t count);

/* Close + free.  Safe with NULL. */
void downplay_thumbs_close(downplay_thumbs_t *t);

/* Per-row lookup.  `rom_basename` is the bare filename (with or without
 * extension — the cascade strips it).  Fills `out` with the current
 * status + on-disk path.
 *
 * Calling repeatedly on the same row is cheap (status flipping
 * UNKNOWN→OK happens once the JPEG arrives; subsequent calls just
 * stat the cache path).  Out-of-flight fetches are de-duplicated by
 * the underlying HTTP task queue. */
void downplay_thumbs_request(downplay_thumbs_t *t,
      const char *rom_basename,
      downplay_thumb_result_t *out);

/* Hint: these basenames are likely-needed soon (e.g. rows ±5 from
 * selection).  Queued at lower priority than active requests; a
 * future _request on the same basename promotes it.  Safe to call
 * with count==0. */
void downplay_thumbs_prefetch(downplay_thumbs_t *t,
      const char * const *basenames, size_t count);

/* Pump the manager.  Called from the menu driver per-frame; advances
 * post-fetch state (re-stat the index after its HTTP task finishes,
 * drain prefetch queue when in-flight count is below the cap).  Cheap
 * when steady-state. */
void downplay_thumbs_pump(downplay_thumbs_t *t);

/* ---------- match cascade (public for unit tests) ---------- */

/* Opaque test-friendly index loaded from a JSON buffer.  Owns its
 * entry storage; closed via `downplay_thumbs_index_free`. */
typedef struct downplay_thumbs_index downplay_thumbs_index_t;

/* Build an in-memory index from a JSON buffer.  Returns NULL on parse
 * failure.  For tests; the manager calls this internally too. */
downplay_thumbs_index_t *downplay_thumbs_index_parse(
      const char *json, size_t json_len);

void downplay_thumbs_index_free(downplay_thumbs_index_t *idx);

/* Run the matching cascade against `rom_basename` (extension optional).
 * On hit, returns an internal pointer to the canonical key string —
 * valid for the lifetime of `idx`, do not free.  On miss returns NULL.
 * Pure: no I/O, no allocation beyond stack. */
const char *downplay_thumbs_index_match(
      const downplay_thumbs_index_t *idx,
      const char *rom_basename);

/* Accessor for tests: how many entries in the index. */
size_t downplay_thumbs_index_count(const downplay_thumbs_index_t *idx);

RETRO_END_DECLS

#endif
