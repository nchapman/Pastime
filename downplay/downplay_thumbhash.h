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

/* Thumbhash decoder.
 *
 * Pure-C port of the algorithm in evanw/thumbhash (MIT licensed).
 * Takes the binary form of a thumbhash (post-base64-decode, 5..32
 * bytes per the upstream spec) and renders it as a small BGRA image
 * at the requested target dimensions.  No external dependencies
 * beyond <math.h>; safe to call on the menu thread (sub-millisecond
 * for the 32x32 sizes we use here).
 *
 * Output is BGRA byte order to match downplay_webp_load_texture and
 * the rest of the thumbnail pipeline — `video_driver_texture_load`
 * is configured with `supports_rgba=false`.
 *
 * The decoder is the only thumbhash code in Downplay; we don't
 * encode (the server publishes encoded thumbhashes inside the
 * per-system index.json that downplay_thumbs_index.c parses). */

#ifndef DOWNPLAY_THUMBHASH_H
#define DOWNPLAY_THUMBHASH_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Decode `hash` (binary thumbhash, length `hash_len`) into a BGRA
 * pixel buffer at `target_w` x `target_h`.  Caller owns `out_bgra`
 * and must size it to at least `target_w * target_h * 4` bytes.
 *
 * Returns false on malformed input (too short, AC stream truncated)
 * or null args.  Output dims are independent of the thumbhash's
 * internal resolution — the algorithm reconstructs at whatever
 * resolution you ask for.  For our use case (placeholder before the
 * real image lands) target dims of 32 on the long edge are enough;
 * the GPU scales to the eventual image rect via the existing
 * gfx_display_draw_quad path. */
bool downplay_thumbhash_decode(
      const uint8_t *hash, size_t hash_len,
      unsigned target_w, unsigned target_h,
      uint8_t *out_bgra);

RETRO_END_DECLS

#endif
