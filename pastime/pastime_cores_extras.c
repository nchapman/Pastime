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

#include <stddef.h>
#include <string.h>

#include "pastime_cores_extras.h"

/* Curated extras.  Add entries here when a core is desired in Pastime
 * but absent from the libretro buildbot.  Each entry pins a specific
 * release — bump the URL and zip paths together when updating. */
static const pastime_cores_extra_t k_extras[] =
{
   {
      /* PICO-8 player.  Known broken on relaunch with savestate
       * auto-load: fake-08 violates the libretro spec by deferring
       * cart load to retro_run via QueueCartChange, so RA's call to
       * retro_unserialize between retro_load_game and retro_run
       * dereferences uninitialised VM state and SIGSEGVs in core_run.
       * First launch is fine (no prior auto-state); subsequent
       * launches crash.  Fix is a one-line change in fake-08's
       * libretro.cpp (s/QueueCartChange/LoadCart/ in retro_load_game);
       * patch + repro at https://github.com/lessui-hq/LessUI-Cores/
       * blob/develop/patches/fake08/01-load-cart-in-retro-load-game.patch
       * — pending upstream PR. */
      "fake08",
      "https://github.com/jtothebell/fake-08/releases/download/v0.0.2.20/Android-Libretro.zip",
      "libs/libfake08-arm64.so",
      "fake08_libretro.info"
   }
};

#define PASTIME_CORES_EXTRAS_COUNT \
   ((unsigned)(sizeof(k_extras) / sizeof(k_extras[0])))

const pastime_cores_extra_t *pastime_cores_extras_lookup(const char *ident)
{
   unsigned i;
   if (!ident || !*ident)
      return NULL;
   for (i = 0; i < PASTIME_CORES_EXTRAS_COUNT; i++)
      if (k_extras[i].ident && strcmp(k_extras[i].ident, ident) == 0)
         return &k_extras[i];
   return NULL;
}

unsigned pastime_cores_extras_count(void)
{
   return PASTIME_CORES_EXTRAS_COUNT;
}

const pastime_cores_extra_t *pastime_cores_extras_at(unsigned idx)
{
   if (idx >= PASTIME_CORES_EXTRAS_COUNT)
      return NULL;
   return &k_extras[idx];
}
