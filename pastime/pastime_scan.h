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

#ifndef PASTIME_SCAN_H
#define PASTIME_SCAN_H

#include <stddef.h>
#include <string.h>
#include <boolean.h>
#include <retro_inline.h>

#include "pastime_rommap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* .disabled predicate                                                  */
/* ------------------------------------------------------------------ */

static INLINE bool pastime_name_is_disabled(const char *name, size_t len)
{
   return len > 9 && memcmp(name + len - 9, ".disabled", 9) == 0;
}

/* ------------------------------------------------------------------ */
/* Baked map cache                                                      */
/* ------------------------------------------------------------------ */

typedef struct
{
   char              fn[64];
   pastime_rommap_t *map;
} pastime_baked_cache_t;

void pastime_baked_cache_init(pastime_baked_cache_t *c);

/* Returns the rommap for `core_ident` (NULL if no route exists or
 * assets_dir is NULL/empty).  Caches internally — repeat calls with
 * the same routing are free. */
pastime_rommap_t *pastime_baked_cache_get(pastime_baked_cache_t *c,
      const char *core_ident, const char *assets_dir);

void pastime_baked_cache_free(pastime_baked_cache_t *c);

/* ------------------------------------------------------------------ */
/* ROM name resolution                                                 */
/* ------------------------------------------------------------------ */

#define PASTIME_RESOLVE_KEEP_TAG  (1u << 0)

typedef struct
{
   const char *display;
   const char *sort_key;
   const char *tag;
   bool        hidden;
   bool        skip;
   bool        mapped;
} pastime_rom_resolved_t;

/* Resolve a ROM filename to its display name + sort key.
 *
 * rom_basename: filename with extension ("mslug.zip")
 * user_map:     per-folder user override map (may be NULL)
 * core_ident:   for baked map routing (may be NULL)
 * baked:        baked map cache (caller-owned, persistent across calls)
 * assets_dir:   settings->paths.directory_assets (NULL-safe)
 * flags:        PASTIME_RESOLVE_KEEP_TAG to populate tag field
 *
 * Writes into caller-owned buffers.  On success, result.display and
 * result.sort_key point into display_buf and sort_buf respectively.
 * tag_buf is populated only when KEEP_TAG is set and a tag exists.
 *
 * Returns result with hidden=true or skip=true when the ROM should
 * be excluded from the list. */
pastime_rom_resolved_t pastime_resolve_rom_name(
      const char *rom_basename,
      const pastime_rommap_t *user_map,
      const char *core_ident,
      pastime_baked_cache_t *baked,
      const char *assets_dir,
      unsigned flags,
      char *display_buf, size_t display_size,
      char *sort_buf, size_t sort_size,
      char *tag_buf, size_t tag_size);

#ifdef __cplusplus
}
#endif

#endif /* PASTIME_SCAN_H */
