/*  Pastime - a fork of RetroArch.
 *  Copyright (C) 2026 - Pastime contributors.
 *
 *  Pastime is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Pastime is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Pastime. If not, see <http://www.gnu.org/licenses/>.
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

#ifndef PASTIME_THUMBS_H
#define PASTIME_THUMBS_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

#define DP_THUMBS_PATH_MAX 1024

typedef struct pastime_thumbs pastime_thumbs_t;

enum pastime_thumb_status
{
   DP_THUMB_UNKNOWN = 0, /* Index not loaded yet, or fetch in flight. */
   DP_THUMB_OK,          /* Image is on disk at `local_path`. */
   DP_THUMB_MISSING      /* Index loaded; title is not in it. */
};

/* Per-row lookup result.
 *
 * `image_w` / `image_h` / `thumbhash` are populated whenever the
 * match cascade hits the index, regardless of whether the image
 * file is on disk yet.  Dims=0 / thumbhash=NULL means either:
 *   (a) status == DP_THUMB_UNKNOWN  → index not yet loaded; layout
 *       should fall back to no-art (full-width text), and re-query
 *       next frame.
 *   (b) status == DP_THUMB_MISSING  → definitive miss; no art.
 *   (c) older server build that didn't include dims/thumbhash —
 *       treat as no-art for layout purposes.
 *
 * Lifetime of `thumbhash`: valid until the next pastime_thumbs_close()
 * (or pastime_thumbs_recents_close()), OR until the next
 * pastime_thumbs_pump() call that may swap an in-memory index (cold
 * → loaded transition).  Callers that need to retain it across pump
 * boundaries must copy the bytes.  Dims (`image_w` / `image_h`) are
 * value-typed and have no lifetime concerns. */
typedef struct
{
   enum pastime_thumb_status status;
   char                       local_path[DP_THUMBS_PATH_MAX];
   uint16_t                   image_w;
   uint16_t                   image_h;
   const uint8_t             *thumbhash;
   size_t                     thumbhash_len;
} pastime_thumb_result_t;

/* Open the manager for a canonical system name (e.g.
 * "Nintendo - Game Boy" — exactly the form
 * `pastime_metadata_resolve_db_name` returns).  Triggers an async
 * index fetch when the on-disk cache is missing or older than the
 * TTL; cheap when cached.  Returns NULL on system==NULL or empty. */
pastime_thumbs_t *pastime_thumbs_open(const char *system);

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
void pastime_thumbs_prefetch_indexes(
      const char * const *systems, size_t count);

/* Close + free.  Safe with NULL. */
void pastime_thumbs_close(pastime_thumbs_t *t);

/* Per-row lookup.  `rom_basename` is the bare filename (with or without
 * extension — the cascade strips it).  Fills `out` with the current
 * status + on-disk path.
 *
 * Calling repeatedly on the same row is cheap (status flipping
 * UNKNOWN→OK happens once the JPEG arrives; subsequent calls just
 * stat the cache path).  Out-of-flight fetches are de-duplicated by
 * the underlying HTTP task queue. */
void pastime_thumbs_request(pastime_thumbs_t *t,
      const char *rom_basename,
      pastime_thumb_result_t *out);

/* Hint: these basenames are likely-needed soon (e.g. rows ±5 from
 * selection).  Queued at lower priority than active requests; a
 * future _request on the same basename promotes it.  Safe to call
 * with count==0. */
void pastime_thumbs_prefetch(pastime_thumbs_t *t,
      const char * const *basenames, size_t count);

/* Side-effect-free metadata lookup.  Runs the match cascade and, on
 * hit, fills `image_w` / `image_h` / `thumbhash` (lifetime: same as
 * pastime_thumb_result_t.thumbhash).  Does NOT queue an HTTP fetch
 * and does NOT stat the cache directory.  Returns true on hit, false
 * on miss / index-not-loaded / null inputs.
 *
 * Used at SYSTEM-view enter to warm per-row layout dims for every
 * ROM in the folder, before the user has hovered any of them.  Cheap
 * enough (~50 ns/call + cache-cold memory) to call N times per system
 * enter for N ~ 20k ROMs without producing visible jank. */
bool pastime_thumbs_peek(pastime_thumbs_t *t,
      const char *rom_basename,
      uint16_t *out_w, uint16_t *out_h,
      const uint8_t **out_thumbhash, size_t *out_thumbhash_len);

/* Pump the manager.  Called from the menu driver per-frame; advances
 * post-fetch state (re-stat the index after its HTTP task finishes,
 * drain prefetch queue when in-flight count is below the cap).  Cheap
 * when steady-state. */
void pastime_thumbs_pump(pastime_thumbs_t *t);

/* ---------- recents resolver (read-only, multi-system) ----------
 *
 * The recents view spans whatever systems the user has launched
 * games from.  We deliberately do not kick HTTP fetches here — if a
 * system's index isn't on disk yet, the row simply shows no art
 * until the user opens that system view (which populates the cache
 * via the manager).  Self-heals on first run; no extra work in the
 * recents code path.
 *
 * Designed for the recents view's lifecycle:
 *   - `_open` once on view enter (cheap; no I/O)
 *   - `_resolve` per row per frame (lazy-loads each system's index
 *     once on first request, caches the result)
 *   - `_close` on view exit (frees all cached indexes) */

typedef struct pastime_thumbs_recents pastime_thumbs_recents_t;

pastime_thumbs_recents_t *pastime_thumbs_recents_open(void);

/* Pre-create a slot for `system` so the resolver knows about it
 * before the first resolve hits.  Cheap (no I/O); idempotent —
 * re-seeding is a no-op.  Used by the menu driver at recents-view
 * enter to enumerate every distinct db_name across the row set so
 * `pastime_thumbs_recents_pump` can drive their loads in order. */
void pastime_thumbs_recents_seed(pastime_thumbs_recents_t *r,
      const char *system);

/* Pre-warm one seeded-but-not-yet-loaded slot per call.  Returns
 * true if a load was attempted (file read attempted) this call,
 * false when every seeded slot is settled.  Call from the menu's
 * per-frame drive function to bound blocking I/O to one .idx read
 * per frame regardless of how the user scrolls. */
bool pastime_thumbs_recents_pump(pastime_thumbs_recents_t *r);

/* Resolve one row.  Returns true and writes the on-disk image path
 * into `out` iff the system's binary index is on disk, the cascade
 * matches `rom_basename`, AND the resolved image is cached.  No HTTP
 * is kicked; a NULL return means "no art for this row right now". */
bool pastime_thumbs_recents_resolve(pastime_thumbs_recents_t *r,
      const char *system, const char *rom_basename,
      char *out, size_t out_size);

/* Side-effect-free metadata lookup.  Returns true and fills the
 * out-params iff the system's binary index is loaded AND the cascade
 * matches `rom_basename`.  Crucially, this succeeds even when the
 * cached image isn't on disk yet — the menu driver calls it per-row
 * to set up text-width before the image lands.  Returns false on
 * null inputs, missing slot, slot not yet loaded, or cascade miss.
 * Does NOT trigger I/O — driver should pump first.
 *
 * Any out-param may be NULL to skip that fetch.  Thumbhash pointer
 * lifetime: same contract as pastime_thumb_result_t.thumbhash. */
bool pastime_thumbs_recents_peek(pastime_thumbs_recents_t *r,
      const char *system, const char *rom_basename,
      uint16_t *out_w, uint16_t *out_h,
      const uint8_t **out_thumbhash, size_t *out_thumbhash_len);

void pastime_thumbs_recents_close(pastime_thumbs_recents_t *r);

/* ---------- match cascade (public for unit tests) ---------- */

/* Opaque test-friendly index loaded from a JSON buffer.  Owns its
 * entry storage; closed via `pastime_thumbs_index_free`. */
typedef struct pastime_thumbs_index pastime_thumbs_index_t;

/* Build an in-memory index from a JSON buffer.  Returns NULL on parse
 * failure.  For tests; the manager calls this internally too. */
pastime_thumbs_index_t *pastime_thumbs_index_parse(
      const char *json, size_t json_len);

void pastime_thumbs_index_free(pastime_thumbs_index_t *idx);

/* Run the matching cascade against `rom_basename` (extension optional).
 * On hit, returns an internal pointer to the canonical key string —
 * valid for the lifetime of `idx`, do not free.  On miss returns NULL.
 * Pure: no I/O, no allocation beyond stack. */
const char *pastime_thumbs_index_match(
      const pastime_thumbs_index_t *idx,
      const char *rom_basename);

/* Accessor for tests: how many entries in the index. */
size_t pastime_thumbs_index_count(const pastime_thumbs_index_t *idx);

RETRO_END_DECLS

#endif
