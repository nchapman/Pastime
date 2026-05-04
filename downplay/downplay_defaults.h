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

#ifndef DOWNPLAY_DEFAULTS_H
#define DOWNPLAY_DEFAULTS_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Resolve the on-disk Downplay/ root.  Writes the absolute path to out
 * and returns true on success.  Returns false (and leaves out untouched)
 * if no candidate root is available — caller should not assume the path
 * exists on disk; pair with downplay_bootstrap() to create it.
 *
 * Android: prefers internal_storage_path (the sdcard tier RA already
 * resolved at startup), falls back to internal_storage_app_path.
 * Other Unix: $HOME. */
bool downplay_paths_get_root(char *out, size_t out_len);

/* Resolve the directory where Downplay stores its private per-system
 * metadata index files (one .json per ROM folder).  Lives under
 * RetroArch's config root (parallel to playlists/, database/), NOT
 * under the user-facing Downplay/ tree, because these are derivative
 * machinery rather than user data.
 *
 * The directory is created on first use.  Returns false (and leaves out
 * untouched) if the parent path can't be resolved. */
bool downplay_paths_get_index_root(char *out, size_t out_len);

/* Overlay Downplay's opinionated defaults onto the loaded settings.
 * Called from retroarch_main_init after config_load() and before the
 * CLI second pass.  The menu_driver override is unconditional; path
 * settings are overlaid only when their current value is empty or
 * equals the upstream platform default (so an explicit user override
 * in retroarch.cfg is preserved). */
void downplay_defaults_apply(void);

RETRO_END_DECLS

#endif
