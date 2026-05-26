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

#ifndef PASTIME_SYSTEM_H
#define PASTIME_SYSTEM_H

#include <boolean.h>
#include "pastime_external.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a Pastime system folder name into its display name and core
 * identifier (or external preset).
 *
 * Convention: "Display Name (core_ident)" or "Display Name (ext-package)"
 *
 * Returns false when the folder doesn't match the convention, the
 * core_ident contains invalid characters, or the ext- marker is
 * unrecognized/uninstalled.
 *
 * On success, *display_out is heap-allocated (caller frees).
 * *ident_out is heap-allocated for libretro cores (caller frees), or
 * NULL for external emulators.  *external_out points into the static
 * preset table (do not free). */
bool pastime_parse_system_folder(const char *folder,
      char **display_out, char **ident_out,
      const pastime_external_spec_t **external_out);

#ifdef __cplusplus
}
#endif

#endif /* PASTIME_SYSTEM_H */
