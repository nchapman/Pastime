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

#include <stdlib.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <file/file_path.h>

#include "pastime_bootstrap.h"
#include "pastime_defaults.h"

#include "../verbosity.h"

/* Default system folders seeded when we first create the Roms/ directory.
 * Once Roms/ exists we never re-seed, so users can delete folders without
 * them coming back.  Names match the English Wikipedia article title. */
static const char *PASTIME_DEFAULT_SYSTEM_FOLDERS[] = {
   /* Nintendo */
   "Game Boy (gambatte)",
   "Game Boy Color (gambatte)",
   "Game Boy Advance (mgba)",
   "Nintendo Entertainment System (mesen)",
   "Super Nintendo Entertainment System (snes9x)",
   "Nintendo 64 (mupen64plus_next)",
   "Nintendo DS (melondsds)",
   "Nintendo 3DS (citra)",
   "GameCube (dolphin)",
   "Wii (dolphin)",
   "Virtual Boy (mednafen_vb)",
   /* Sega */
   "Master System (genesis_plus_gx)",
   "Game Gear (genesis_plus_gx)",
   "Sega Genesis (genesis_plus_gx)",
   "Sega CD (genesis_plus_gx)",
   "32X (picodrive)",
   "Sega Saturn (mednafen_saturn)",
   "Dreamcast (flycast)",
   /* Sony */
   "PlayStation (swanstation)",
   "PlayStation Portable (ppsspp)",
   "PlayStation 2 (pcsx2)",
   /* Other */
   "Arcade (fbneo)",
   "Neo Geo (fbneo)",
   "TurboGrafx-16 (mednafen_pce)",
   "TurboGrafx-CD (mednafen_pce)",
   "Neo Geo Pocket (mednafen_ngp)",
   "Neo Geo Pocket Color (mednafen_ngp)",
   "WonderSwan (mednafen_wswan)",
   "WonderSwan Color (mednafen_wswan)",
   "Atari 2600 (stella)",
   "Atari 7800 (prosystem)",
   "Atari Lynx (mednafen_lynx)",
};

static void pastime_ensure_dir(const char *parent, const char *leaf)
{
   char path[PATH_MAX_LENGTH];
   fill_pathname_join_special(path, parent, leaf, sizeof(path));
   if (path_is_directory(path))
      return;
   if (!path_mkdir(path))
      RARCH_WARN("[Pastime] mkdir failed: %s\n", path);
}

void pastime_bootstrap(void)
{
   char root[PATH_MAX_LENGTH];
   char roms[PATH_MAX_LENGTH];
   bool roms_existed;

   if (!pastime_paths_get_root(root, sizeof(root)))
   {
      RARCH_WARN("[Pastime] bootstrap: no root path; skipping\n");
      return;
   }

   if (!path_is_directory(root) && !path_mkdir(root))
   {
      RARCH_WARN("[Pastime] bootstrap: cannot create %s\n", root);
      return;
   }

   fill_pathname_join_special(roms, root, "Roms", sizeof(roms));
   roms_existed = path_is_directory(roms);

   pastime_ensure_dir(root, "Roms");
   pastime_ensure_dir(root, "Bios");
   pastime_ensure_dir(root, "Saves");
   pastime_ensure_dir(root, "States");

   /* Seed system subfolders only when Roms/ was freshly created. */
   if (!roms_existed)
   {
      size_t i;
      for (i = 0; i < ARRAY_SIZE(PASTIME_DEFAULT_SYSTEM_FOLDERS); i++)
         pastime_ensure_dir(roms, PASTIME_DEFAULT_SYSTEM_FOLDERS[i]);
   }
}
