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

#ifndef PASTIME_METADATA_H
#define PASTIME_METADATA_H

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
 * thumbnail repo exists for this folder; art lookup will be skipped).
 *
 * (Was historically the public face of a richer metadata module that
 * also persisted per-ROM CRC matches and an art_state cache for the
 * row drawer.  Both of those have been retired: CRC matching moved
 * out before the pastime.gg pipeline; the art_state cache became
 * redundant once the thumbnail index started shipping per-entry
 * dimensions.  Only the disambiguation resolver remains.) */
const char *pastime_metadata_resolve_db_name(
      const char *display_name, const char *core_ident);

RETRO_END_DECLS

#endif
