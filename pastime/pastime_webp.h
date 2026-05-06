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

/* Standalone WebP loader for the thumbnail pipeline.
 *
 * Bypasses RA's `formats/rwebp` and `image_transfer` paths entirely.
 * Reads a .webp file, decodes via the vendored libwebp decoder
 * (`deps/libwebp/`), and uploads the result as a GPU texture via
 * `video_driver_texture_load`.  Returns the texture handle + dims.
 *
 * Pixel format matches the driver: queries `VIDEO_FLAG_USE_RGBA`
 * once per call and selects WebPDecodeRGBA vs WebPDecodeBGRA.
 *
 * Returns true on success.  On failure `*out_id` is left untouched
 * (caller-owned) and the file is *not* deleted from disk — the
 * caller can decide whether a corrupt cache entry is worth retrying. */

#ifndef PASTIME_WEBP_H
#define PASTIME_WEBP_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

bool pastime_webp_load_texture(
      const char *path,
      uintptr_t  *out_id,
      unsigned   *out_width,
      unsigned   *out_height);

RETRO_END_DECLS

#endif
