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
   "Drop one folder per system here.  The folder name must end with the\n"
   "libretro core ident in parentheses, e.g.:\n"
   "\n"
   "  Super Nintendo (snes9x)/\n"
   "  Game Boy Advance (mgba)/\n"
   "  Sega Genesis (genesis_plus_gx)/\n"
   "\n"
   "Folders without the (corename) suffix are hidden.  The core is\n"
   "downloaded from the libretro buildbot the first time you launch a\n"
   "game in that folder; you do not install cores manually.\n"
   "\n"
   "Multiple folders can target the same core (e.g. an \"official\"\n"
   "library and a \"hacks\" library both using mgba).\n";

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

   /* Drop the convention README into Roms/ on first launch.  Don't
    * overwrite — the user might have edited it. */
   fill_pathname_join_special(roms,   root, "Roms",       sizeof(roms));
   fill_pathname_join_special(readme, roms, "README.txt", sizeof(readme));
   if (!path_is_valid(readme))
   {
      if (!filestream_write_file(readme,
               PASTIME_README, (int64_t)(sizeof(PASTIME_README) - 1)))
         RARCH_WARN("[Pastime] bootstrap: failed to write %s\n", readme);
   }
}
