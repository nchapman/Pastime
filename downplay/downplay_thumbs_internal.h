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

#ifndef _DOWNPLAY_THUMBS_INTERNAL_H
#define _DOWNPLAY_THUMBS_INTERNAL_H

/* Cross-TU surface between downplay_thumbs_index.c (pure parse + match
 * cascade) and downplay_thumbs.c (HTTP/IO manager).  Not part of the
 * public API — intentionally kept minimal so the split stays clean. */

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "downplay_thumbs.h"

RETRO_BEGIN_DECLS

/* Parse a server-format JSON index into the on-disk binary buffer
 * (DPTH magic, see downplay_thumbs_index.c for the layout).  On
 * success the caller owns *out_buf and frees with free(). */
bool dp_idx_parse_json_to_buffer(const char *json, size_t json_len,
      uint8_t **out_buf, size_t *out_len);

/* Open a previously-emitted binary buffer.  Takes ownership of `buf`:
 * frees it on validation failure, otherwise returns an opaque index
 * handle (free with downplay_thumbs_index_free). */
downplay_thumbs_index_t *dp_idx_open(uint8_t *buf, size_t buf_len);

/* Canonical title string for entry `e` (0..count-1) — pointer into
 * the index's internal string pool, valid until index_free. */
const char *dp_idx_canonical(const downplay_thumbs_index_t *idx,
      uint32_t e);

/* Run the match cascade and return the entry index, or (size_t)-1 if
 * no match.  downplay_thumbs_index_match() is the public string-
 * returning wrapper around this. */
size_t dp_idx_match(const downplay_thumbs_index_t *idx,
      const char *rom_basename);

RETRO_END_DECLS

#endif /* _DOWNPLAY_THUMBS_INTERNAL_H */
