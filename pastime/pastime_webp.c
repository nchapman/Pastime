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

#include <stdint.h>
#include <stdlib.h>

#include <boolean.h>
#include <streams/file_stream.h>
#include <formats/image.h>

#include "pastime_webp.h"
#include "../gfx/video_driver.h"
#include "../verbosity.h"

#include "../deps/libwebp/src/webp/decode.h"

/* Hard payload cap: an attacker who can serve us a thumbnail can't
 * blow up the heap.  Real boxart .webp files are tens-to-hundreds of
 * KB; a few MB is plenty of headroom for the largest pre-rendered
 * thumbnails we'd reasonably receive. */
#define DP_WEBP_MAX_BYTES (4u * 1024u * 1024u)

/* Output-dimension cap.  The file-size cap above bounds the *input*,
 * but a crafted lossless WebP can compress a 16K×16K image into a
 * tiny bitstream — decoded RGBA would be ~1 GB and OOM the device.
 * 4096 is generous: real boxart is ~300×400. */
#define DP_WEBP_MAX_DIM 4096

bool pastime_webp_load_texture(
      const char *path,
      uintptr_t  *out_id,
      unsigned   *out_width,
      unsigned   *out_height)
{
   void                *buf = NULL;
   int64_t              len = 0;
   int                  w   = 0;
   int                  h   = 0;
   uint8_t             *bgra;
   struct texture_image ti;

   if (!path || !*path || !out_id)
      return false;

   if (!filestream_read_file(path, &buf, &len) || !buf || len <= 0)
   {
      free(buf);
      return false;
   }
   if ((size_t)len > DP_WEBP_MAX_BYTES)
   {
      RARCH_WARN("[Pastime] webp: %s exceeds %u-byte cap (%lld bytes)\n",
            path, (unsigned)DP_WEBP_MAX_BYTES, (long long)len);
      free(buf);
      return false;
   }

   /* Header-only dimension check before allocating the decode buffer. */
   if (!WebPGetInfo((const uint8_t*)buf, (size_t)len, &w, &h)
         || w <= 0 || h <= 0
         || w > DP_WEBP_MAX_DIM || h > DP_WEBP_MAX_DIM)
   {
      RARCH_WARN("[Pastime] webp: %s rejected (dims %dx%d)\n",
            path, w, h);
      free(buf);
      return false;
   }

   /* Always decode BGRA + supports_rgba=false.  This matches what
    * upstream rpng/rjpeg produce (see libretro-common/formats/
    * image_texture.c) and what every active driver expects:
    *   - Vulkan (Pastime's Android default): hard-codes
    *     VK_FORMAT_B8G8R8A8_UNORM, ignores supports_rgba entirely.
    *   - GL2: re-reads VIDEO_FLAG_USE_RGBA in its uploader and
    *     accepts either byte order via GL_RGBA + GL_UNSIGNED_BYTE.
    * Branching here on the global flag was load-bearing on an
    * unstated invariant about Vulkan's flag state — drop it. */
   bgra = WebPDecodeBGRA((const uint8_t*)buf, (size_t)len, &w, &h);
   free(buf);

   if (!bgra || w <= 0 || h <= 0)
   {
      WebPFree(bgra);
      return false;
   }

   ti.pixels        = (uint32_t*)bgra;
   ti.width         = (unsigned)w;
   ti.height        = (unsigned)h;
   ti.supports_rgba = false;

   /* TEXTURE_FILTER_LINEAR — match the JPG path (gfx_thumbnail.c).
    * MIPMAP_LINEAR would trigger a full mipchain build (glGenerateMipmap)
    * on every upload, ~33% extra VRAM, no visible benefit at thumbnail
    * resolutions. */
   if (!video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, out_id))
   {
      WebPFree(bgra);
      return false;
   }
   WebPFree(bgra);

   if (out_width)  *out_width  = (unsigned)w;
   if (out_height) *out_height = (unsigned)h;
   return true;
}
