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
#include <streams/file_stream.h>

#include "pastime_bootstrap.h"
#include "pastime_defaults.h"

#include "../verbosity.h"

static const char PASTIME_README[] =
   "Pastime - Roms folder\n"
   "\n"
   "Drop one folder per system here.  The folder name must end with a\n"
   "backend marker in parentheses, e.g.:\n"
   "\n"
   "  Super Nintendo Entertainment System (snes9x)/\n"
   "  Game Boy Advance (mgba)/\n"
   "  PlayStation 2 (ext-aethersx2)/\n"
   "\n"
   "  (corename)        - libretro core; downloaded from the libretro\n"
   "                      buildbot the first time you launch a game.\n"
   "  (ext-shortname)   - external Android app (e.g. ext-aethersx2,\n"
   "                      ext-dolphin, ext-azahar).  Folder is hidden\n"
   "                      if the app is not installed.\n"
   "\n"
   "Folders without a marker are hidden.  Multiple folders can target\n"
   "the same backend (e.g. an \"official\" library and a \"hacks\"\n"
   "library both using mgba); folders that share a display name are\n"
   "merged into one row in the launcher.\n";

/* First-run system folders.  Seeded once when the README is created;
 * deleting a folder later does not bring it back.  Names match the
 * English Wikipedia article title for each system. */
static const char *PASTIME_DEFAULT_SYSTEM_FOLDERS[] = {
   /* Nintendo */
   "Game Boy (gambatte)",
   "Game Boy Color (gambatte)",
   "Game Boy Advance (mgba)",
   "Nintendo Entertainment System (mesen)",
   "Super Nintendo Entertainment System (snes9x)",
   "Nintendo 64 (mupen64plus_next)",
   "Nintendo DS (melondsds)",
   "Nintendo 3DS (ext-azahar)",
   "GameCube (ext-dolphin)",
   "Wii (ext-dolphin)",
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
   "PlayStation 2 (ext-aethersx2)",
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
   char readme[PATH_MAX_LENGTH];

   if (!pastime_paths_get_root(root, sizeof(root)))
   {
      RARCH_WARN("[Pastime] bootstrap: no root path; skipping\n");
      return;
   }

   /* path_mkdir is recursive — it'll create root if missing as a side
    * effect of creating the children, but we mkdir it explicitly so the
    * intent is legible in logs. */
   if (!path_is_directory(root) && !path_mkdir(root))
   {
      RARCH_WARN("[Pastime] bootstrap: cannot create %s\n", root);
      return;
   }

   pastime_ensure_dir(root, "Roms");
   pastime_ensure_dir(root, "Bios");
   pastime_ensure_dir(root, "Saves");
   pastime_ensure_dir(root, "States");

   /* Drop the convention README into Roms/ on first launch, and seed
    * the default system folders the same time.  README presence is the
    * first-run sentinel — once it exists, we never re-seed, so users
    * can delete folders without them coming back. */
   fill_pathname_join_special(roms,   root, "Roms",       sizeof(roms));
   fill_pathname_join_special(readme, roms, "README.txt", sizeof(readme));
   if (!path_is_valid(readme))
   {
      size_t i;
      for (i = 0; i < ARRAY_SIZE(PASTIME_DEFAULT_SYSTEM_FOLDERS); i++)
         pastime_ensure_dir(roms, PASTIME_DEFAULT_SYSTEM_FOLDERS[i]);

      if (!filestream_write_file(readme,
               PASTIME_README, (int64_t)(sizeof(PASTIME_README) - 1)))
         RARCH_WARN("[Pastime] bootstrap: failed to write %s\n", readme);
   }
}
