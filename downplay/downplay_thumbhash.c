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

/* Thumbhash decoder.  See header for the contract.
 *
 * Algorithm reference: evanw/thumbhash (MIT license).  The encoded
 * form is a tiny DCT in YPbPr-like color space — three (or four,
 * with alpha) channels of low-frequency coefficients, packed at 4
 * bits each.  Reconstruction sums the basis cosines; we do this in
 * single precision (float), which is plenty for the perceptual
 * fidelity a placeholder needs.
 *
 * Hash byte layout (post-base64-decode):
 *   [0..2]  header24:
 *           bits  0..5  l_dc          (6 bits, 0..63)
 *           bits  6..11 p_dc          (6 bits, encodes -1..1)
 *           bits 12..17 q_dc          (6 bits, encodes -1..1)
 *           bits 18..22 l_scale       (5 bits)
 *           bit  23     hasAlpha
 *   [3..4]  header16:
 *           bits  0..2  lx-or-ly      (3 bits, see lx/ly logic)
 *           bits  3..8  p_scale       (6 bits)
 *           bits  9..14 q_scale       (6 bits)
 *           bit  15     isLandscape
 *   [5]     (only if hasAlpha):
 *           low4 = a_dc, high4 = a_scale
 *   rest:   AC coefficients, packed 4 bits each (low nibble first),
 *           in order: L channel, P channel (3x3), Q channel (3x3),
 *           A channel (5x5, only if hasAlpha).  The AC loop is
 *           triangular: cx*ny < nx*(ny-cy), cy >= 0, cx >= (cy?0:1). */

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <boolean.h>

#include "downplay_thumbhash.h"

#ifndef DP_TH_PI
#define DP_TH_PI 3.14159265358979323846f
#endif

/* Per-channel AC coefficient capacities.  The triangular fill
 * `cx*ny < nx*(ny-cy)` (with the cy?0:1 start) emits a bounded
 * count that's strictly less than nx*ny - 1.  Caps below are loose
 * upper bounds — the `count >= out_cap` guard in dp_th_decode_channel
 * is the actual safety net, but these match the format's stated
 * maxima with a little headroom:
 *   L:  lx/ly cap at 7 in practice (encoder uses 3 bits with the
 *       hasAlpha-dependent fixed values for the off-axis dim).
 *       Triangular 7x7 fill is 24 entries; 96 is generous headroom.
 *   P:  fixed 3x3, triangular fill is 5 entries; 9 is rounded up.
 *   Q:  same as P.
 *   A:  fixed 5x5, triangular fill is 14 entries; 25 is rounded up. */
#define DP_TH_MAX_L_AC  96
#define DP_TH_MAX_P_AC  9
#define DP_TH_MAX_Q_AC  9
#define DP_TH_MAX_A_AC  25

/* Read the next AC channel out of `hash`.  Advances *ac_index_inout
 * by one nibble per coefficient consumed.  Returns false if the
 * channel would overrun the hash; the partial output is left in
 * `out_ac` (caller discards). */
static bool dp_th_decode_channel(const uint8_t *hash, size_t hash_len,
      int nx, int ny, float scale, size_t *ac_index_inout,
      float *out_ac, int out_cap)
{
   int cx;
   int cy;
   int count = 0;
   for (cy = 0; cy < ny; cy++)
   {
      int start = cy ? 0 : 1;
      for (cx = start; cx * ny < nx * (ny - cy); cx++)
      {
         size_t i        = *ac_index_inout;
         size_t byte_idx = i >> 1;
         int    nibble;
         if (byte_idx >= hash_len)
            return false;
         if (count >= out_cap)
            return false;
         nibble = (hash[byte_idx] >> ((i & 1) * 4)) & 15;
         out_ac[count++] = ((float)nibble / 7.5f - 1.0f) * scale;
         (*ac_index_inout)++;
      }
   }
   (void)count;
   return true;
}

static uint8_t dp_th_to_byte(float v)
{
   int n;
   if (v <= 0.0f)      return 0;
   if (v >= 1.0f)      return 255;
   n = (int)(255.0f * v + 0.5f);
   if (n < 0)   return 0;
   if (n > 255) return 255;
   return (uint8_t)n;
}

bool downplay_thumbhash_decode(
      const uint8_t *hash, size_t hash_len,
      unsigned target_w, unsigned target_h,
      uint8_t *out_bgra)
{
   uint32_t header24;
   uint32_t header16;
   float    l_dc;
   float    p_dc;
   float    q_dc;
   float    l_scale;
   float    p_scale;
   float    q_scale;
   float    a_dc;
   float    a_scale;
   int      hasAlpha;
   int      isLandscape;
   int      lx;
   int      ly;
   int      hxy;
   size_t   ac_index;
   float    l_ac[DP_TH_MAX_L_AC];
   float    p_ac[DP_TH_MAX_P_AC];
   float    q_ac[DP_TH_MAX_Q_AC];
   /* Zero-init A: the pixel loop's `if (hasAlpha)` guard prevents
    * reads when the channel was skipped, but coupling that guard to
    * an uninitialized array across ~100 LOC is a footgun.  ~25 stores
    * at decode time, irrelevant cost. */
   float    a_ac[DP_TH_MAX_A_AC] = { 0 };
   unsigned x;
   unsigned y;
   uint8_t *p;

   if (!hash || hash_len < 5 || target_w == 0 || target_h == 0
         || !out_bgra)
      return false;

   header24 = (uint32_t)hash[0]
            | ((uint32_t)hash[1] << 8)
            | ((uint32_t)hash[2] << 16);
   header16 = (uint32_t)hash[3]
            | ((uint32_t)hash[4] << 8);

   l_dc        =  (float)(header24 & 63)         / 63.0f;
   p_dc        =  (float)((header24 >>  6) & 63) / 31.5f - 1.0f;
   q_dc        =  (float)((header24 >> 12) & 63) / 31.5f - 1.0f;
   l_scale     =  (float)((header24 >> 18) & 31) / 31.0f;
   hasAlpha    =  (int)((header24 >> 23) & 1);
   p_scale     =  (float)((header16 >>  3) & 63) / 63.0f;
   q_scale     =  (float)((header16 >>  9) & 63) / 63.0f;
   isLandscape =  (int)((header16 >> 15) & 1);

   hxy = (int)(header16 & 7);
   lx  = isLandscape ? (hasAlpha ? 5 : 7) : hxy;
   ly  = isLandscape ? hxy : (hasAlpha ? 5 : 7);
   if (lx < 3) lx = 3;
   if (ly < 3) ly = 3;

   if (hasAlpha)
   {
      if (hash_len < 6)
         return false;
      a_dc     = (float)(hash[5] & 15) / 15.0f;
      a_scale  = (float)(hash[5] >> 4) / 15.0f;
      ac_index = 12; /* 6 header bytes consumed × 2 nibbles */
   }
   else
   {
      a_dc     = 1.0f;
      a_scale  = 0.0f;
      ac_index = 10; /* 5 header bytes consumed × 2 nibbles */
   }

   if (!dp_th_decode_channel(hash, hash_len, lx, ly, l_scale,
            &ac_index, l_ac, DP_TH_MAX_L_AC))
      return false;
   if (!dp_th_decode_channel(hash, hash_len, 3, 3, p_scale * 1.25f,
            &ac_index, p_ac, DP_TH_MAX_P_AC))
      return false;
   if (!dp_th_decode_channel(hash, hash_len, 3, 3, q_scale * 1.25f,
            &ac_index, q_ac, DP_TH_MAX_Q_AC))
      return false;
   if (hasAlpha)
   {
      if (!dp_th_decode_channel(hash, hash_len, 5, 5, a_scale,
               &ac_index, a_ac, DP_TH_MAX_A_AC))
         return false;
   }

   /* Render pixels.  Inner loop sums basis-cosine products for each
    * channel.  The cy=0 / cx=0 iteration is the DC term and is
    * already in *_dc, so the AC loop starts at (cy=0, cx=1) and
    * skips the (cy>0, cx=0) cells that would also represent DC
    * along their axis — captured by the cy?0:1 start pattern.
    *
    * Per-pixel cost: O(L_AC + P_AC + Q_AC + A_AC).  At lx=ly=7
    * that's ~24 + 4 + 4 + 0 = ~32 cosf pairs per pixel.  Target
    * 32x32 → ~32K cosf calls, sub-millisecond on Android arm64. */
   p = out_bgra;
   for (y = 0; y < target_h; y++)
   {
      for (x = 0; x < target_w; x++)
      {
         float fx = ((float)x + 0.5f) / (float)target_w;
         float fy = ((float)y + 0.5f) / (float)target_h;
         float l  = l_dc;
         float pv = p_dc;
         float qv = q_dc;
         float av = a_dc;
         int   i;
         int   cx;
         int   cy;
         float b;
         float r;
         float g;

         /* L channel (lx by ly, triangular). */
         i = 0;
         for (cy = 0; cy < ly; cy++)
         {
            int start = cy ? 0 : 1;
            for (cx = start; cx * ly < lx * (ly - cy); cx++)
               l += l_ac[i++]
                     * cosf(DP_TH_PI * (float)cx * fx)
                     * cosf(DP_TH_PI * (float)cy * fy);
         }

         /* P channel (3x3, triangular). */
         i = 0;
         for (cy = 0; cy < 3; cy++)
         {
            int start = cy ? 0 : 1;
            for (cx = start; cx * 3 < 3 * (3 - cy); cx++)
               pv += p_ac[i++]
                     * cosf(DP_TH_PI * (float)cx * fx)
                     * cosf(DP_TH_PI * (float)cy * fy);
         }

         /* Q channel (3x3, triangular). */
         i = 0;
         for (cy = 0; cy < 3; cy++)
         {
            int start = cy ? 0 : 1;
            for (cx = start; cx * 3 < 3 * (3 - cy); cx++)
               qv += q_ac[i++]
                     * cosf(DP_TH_PI * (float)cx * fx)
                     * cosf(DP_TH_PI * (float)cy * fy);
         }

         /* A channel (5x5, only if hasAlpha). */
         if (hasAlpha)
         {
            i = 0;
            for (cy = 0; cy < 5; cy++)
            {
               int start = cy ? 0 : 1;
               for (cx = start; cx * 5 < 5 * (5 - cy); cx++)
                  av += a_ac[i++]
                        * cosf(DP_TH_PI * (float)cx * fx)
                        * cosf(DP_TH_PI * (float)cy * fy);
            }
         }

         /* YPbPr-like → RGB.  See the encoder for the inverse
          * transform; this matches evanw/thumbhash's reference. */
         b = l - 2.0f / 3.0f * pv;
         r = (3.0f * l - b + qv) * 0.5f;
         g = r - qv;

         /* Output BGRA byte order (matches downplay_webp.c).  Vulkan
          * wants B8G8R8A8; supports_rgba=false on texture upload. */
         *p++ = dp_th_to_byte(b);
         *p++ = dp_th_to_byte(g);
         *p++ = dp_th_to_byte(r);
         *p++ = dp_th_to_byte(av);
      }
   }
   return true;
}
