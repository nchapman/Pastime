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

#include "pastime_scan.h"
#include "pastime_display_name.h"

#include <string.h>

#ifndef PASTIME_SCAN_TEST_BUILD
#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <compat/strl.h>
#else
#ifndef PATH_MAX_LENGTH
#define PATH_MAX_LENGTH 4096
#endif
/* Test stubs for path utilities */
static void fill_pathname_join_special(char *out,
      const char *dir, const char *file, size_t size)
{
   size_t dlen = strlen(dir);
   if (dlen + 1 + strlen(file) + 1 > size)
   {
      out[0] = '\0';
      return;
   }
   strcpy(out, dir);
   out[dlen] = '/';
   strcpy(out + dlen + 1, file);
}
static bool string_is_equal(const char *a, const char *b)
{
   if (!a || !b) return a == b;
   return strcmp(a, b) == 0;
}
#if defined(__APPLE__)
#define PASTIME_HAVE_STRLCPY 1
#elif defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 38)
#define PASTIME_HAVE_STRLCPY 1
#endif
#endif
#if !defined(strlcpy) && !defined(PASTIME_HAVE_STRLCPY)
static size_t strlcpy(char *dst, const char *src, size_t size)
{
   size_t len = strlen(src);
   if (size > 0)
   {
      size_t cp = len < size - 1 ? len : size - 1;
      memcpy(dst, src, cp);
      dst[cp] = '\0';
   }
   return len;
}
#endif
#endif

/* ------------------------------------------------------------------ */
/* Baked map cache                                                      */
/* ------------------------------------------------------------------ */

void pastime_baked_cache_init(pastime_baked_cache_t *c)
{
   c->fn[0] = '\0';
   c->map   = NULL;
}

pastime_rommap_t *pastime_baked_cache_get(pastime_baked_cache_t *c,
      const char *core_ident, const char *assets_dir)
{
   const char *baked_fn;

   if (!core_ident || !*core_ident)
      return NULL;

   baked_fn = pastime_rommap_route(core_ident);
   if (!baked_fn)
      return NULL;

   if (!string_is_equal(baked_fn, c->fn))
   {
      char map_path[PATH_MAX_LENGTH];
      char maps_dir[PATH_MAX_LENGTH];

      pastime_rommap_free(c->map);
      c->map = NULL;

      /* Store fn even when load fails so we don't re-attempt on every
       * subsequent call for the same core within this scan pass. */
      strlcpy(c->fn, baked_fn, sizeof(c->fn));

      if (!assets_dir || !*assets_dir)
         return NULL;

      fill_pathname_join_special(maps_dir, assets_dir,
            "pastime/maps", sizeof(maps_dir));
      fill_pathname_join_special(map_path, maps_dir,
            baked_fn, sizeof(map_path));
      c->map = pastime_rommap_load(map_path);
   }

   return c->map;
}

void pastime_baked_cache_free(pastime_baked_cache_t *c)
{
   pastime_rommap_free(c->map);
   c->map   = NULL;
   c->fn[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* ROM name resolution                                                 */
/* ------------------------------------------------------------------ */

pastime_rom_resolved_t pastime_resolve_rom_name(
      const char *rom_basename,
      const pastime_rommap_t *user_map,
      const char *core_ident,
      pastime_baked_cache_t *baked,
      const char *assets_dir,
      unsigned flags,
      char *display_buf, size_t display_size,
      char *sort_buf, size_t sort_size,
      char *tag_buf, size_t tag_size)
{
   pastime_rom_resolved_t res;
   const char            *mapped = NULL;

   memset(&res, 0, sizeof(res));
   if (display_buf && display_size > 0)
      display_buf[0] = '\0';
   if (sort_buf && sort_size > 0)
      sort_buf[0] = '\0';
   if (tag_buf && tag_size > 0)
      tag_buf[0] = '\0';

   if (!rom_basename || !*rom_basename)
   {
      res.skip = true;
      return res;
   }

   /* Map lookup: user override first, then baked */
   mapped = pastime_rommap_lookup(user_map, rom_basename);
   if (!mapped && baked)
   {
      pastime_rommap_t *bmap = pastime_baked_cache_get(
            baked, core_ident, assets_dir);
      mapped = pastime_rommap_lookup(bmap, rom_basename);
   }

   if (mapped && mapped[0] == '.')
   {
      res.hidden = true;
      return res;
   }

   if (mapped)
   {
      res.mapped = true;
      strlcpy(display_buf, mapped, display_size);
      pastime_display_name_sort_key(mapped, sort_buf, sort_size);
      res.display  = display_buf;
      res.sort_key = sort_buf;
      res.tag      = tag_buf;
      return res;
   }

   /* Unmapped: strip extension, clean, generate sort key */
   strlcpy(display_buf, rom_basename, display_size);
   pastime_display_name_strip_rom_extension(display_buf);

   if (!*display_buf)
   {
      res.skip = true;
      return res;
   }

   if (flags & PASTIME_RESOLVE_KEEP_TAG)
   {
      char clean_tmp[512];
      pastime_display_name_clean_keep_tag(display_buf,
            clean_tmp, sizeof(clean_tmp),
            tag_buf, tag_size);
      if (!*clean_tmp)
      {
         res.skip = true;
         return res;
      }
      strlcpy(display_buf, clean_tmp, display_size);
   }
   else
   {
      char clean_tmp[512];
      pastime_display_name_clean(display_buf, clean_tmp, sizeof(clean_tmp));
      if (!*clean_tmp)
      {
         res.skip = true;
         return res;
      }
      strlcpy(display_buf, clean_tmp, display_size);
   }

   pastime_display_name_sort_key(display_buf, sort_buf, sort_size);
   res.display  = display_buf;
   res.sort_key = sort_buf;
   res.tag      = tag_buf;
   return res;
}
