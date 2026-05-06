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

#ifndef PASTIME_CORES_EXTRAS_H
#define PASTIME_CORES_EXTRAS_H

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Curated table of libretro cores that are NOT on the libretro buildbot
 * but are worth shipping via Pastime — typically third-party emulators
 * with their own GitHub release artifacts (e.g. fake-08 for PICO-8).
 *
 * Used as a fallback in pastime_cores_queue_next: when an ident isn't
 * found in the cached buildbot list, we look here.  Empty/missing entry
 * means "not available", same UX as a buildbot miss.
 *
 * URLs are pinned to a specific release tag, not "latest" — keeps boot
 * installs reproducible and avoids surprise breakage when upstream
 * publishes an incompatible build. */
typedef struct
{
   /* Ident the rest of Pastime references this core by — matches the
    * ".info" basename minus "_libretro".  e.g. "fake08" pairs with
    * fake08_libretro.info / fake08_libretro_android.so. */
   const char *ident;
   /* Absolute https URL of the release zip. */
   const char *zip_url;
   /* Path inside the zip of the arm64 Android .so we install.  Pastime's
    * primary build target is aarch64 (gg.pastime.aarch64); we don't ship
    * arm32 today. */
   const char *zip_so_path;
   /* Path inside the zip of the .info file (NULL if the release doesn't
    * include one — currently unused, but reserved for cores whose .info
    * we'd ship in-tree). */
   const char *zip_info_path;
} pastime_cores_extra_t;

/* Look up an extras entry by ident.  Returns NULL if no match. */
const pastime_cores_extra_t *pastime_cores_extras_lookup(const char *ident);

/* Number of entries in the extras table.  Test-only — production code
 * should iterate via lookup. */
unsigned pastime_cores_extras_count(void);

/* Direct access for tests; returns NULL if idx out of range. */
const pastime_cores_extra_t *pastime_cores_extras_at(unsigned idx);

RETRO_END_DECLS

#endif
