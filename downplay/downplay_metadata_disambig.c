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

/* Disambiguation table + db_name resolver.  Split out from
 * downplay_metadata.c so the unit tests at downplay/tests/ can link
 * against this file alone — the rest of the metadata module pulls in
 * libretrodb, rjson, the HTTP task system, and gfx_thumbnail, all of
 * which are too heavy to drag into a standalone test binary. */

#include <stddef.h>
#include <stdio.h>

#include <retro_common_api.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>

#include "downplay_metadata.h"

#include "../core_info.h"

/* Curated map from common user-typed system names to the canonical
 * libretro-thumbnails system name (which is the directory under
 * https://thumbnails.libretro.com/, AND the .rdb filename stem).
 *
 * Aliases are case-insensitive; matched against the user's
 * "Display Name (core_ident)" folder convention's display half.  Multi-
 * system cores (mgba, genesis_plus_gx, snes9x) require this — folder
 * convention alone doesn't tell us whether mgba's entries are GB / GBC
 * / GBA.
 *
 * Add aliases freely — this is opinionated by design, not exhaustive.
 * Submit a PR if your preferred name isn't covered. */
struct dp_disambig
{
   const char *db_name;
   const char *aliases[8]; /* NULL-terminated */
};

static const struct dp_disambig dp_disambig_table[] = {
   /* Nintendo */
   { "Nintendo - Nintendo Entertainment System",
     { "Nintendo Entertainment System", "NES", "Famicom", "FC", NULL } },
   { "Nintendo - Super Nintendo Entertainment System",
     { "Super Nintendo Entertainment System", "Super Nintendo",
       "SNES", "Super Famicom", "SFC", NULL } },
   { "Nintendo - Nintendo 64",
     { "Nintendo 64", "N64", NULL } },
   { "Nintendo - Game Boy",
     { "Game Boy", "GB", NULL } },
   { "Nintendo - Game Boy Color",
     { "Game Boy Color", "GBC", NULL } },
   { "Nintendo - Game Boy Advance",
     { "Game Boy Advance", "GBA", NULL } },
   { "Nintendo - Nintendo DS",
     { "Nintendo DS", "DS", "NDS", NULL } },
   { "Nintendo - GameCube",
     { "GameCube", "GCN", "GC", NULL } },
   { "Nintendo - Virtual Boy",
     { "Virtual Boy", "VB", NULL } },
   { "Nintendo - Wii",
     { "Wii", NULL } },
   /* Sega */
   { "Sega - Mega Drive - Genesis",
     { "Genesis", "Sega Genesis", "Mega Drive", "Sega Mega Drive", "MD", NULL } },
   { "Sega - Master System - Mark III",
     { "Master System", "Sega Master System", "SMS", "Mark III", NULL } },
   { "Sega - Game Gear",
     { "Game Gear", "Sega Game Gear", "GG", NULL } },
   { "Sega - Saturn",
     { "Saturn", "Sega Saturn", NULL } },
   { "Sega - Dreamcast",
     { "Dreamcast", "Sega Dreamcast", "DC", NULL } },
   { "Sega - 32X",
     { "32X", "Sega 32X", "Mega Drive 32X", NULL } },
   { "Sega - Mega-CD - Sega CD",
     { "Sega CD", "Mega CD", "Mega-CD", NULL } },
   /* NEC */
   { "NEC - PC Engine - TurboGrafx 16",
     { "PC Engine", "TurboGrafx-16", "TurboGrafx 16", "TG-16", "TG16", NULL } },
   { "NEC - PC Engine CD - TurboGrafx-CD",
     { "PC Engine CD", "TurboGrafx-CD", "TurboGrafx CD", NULL } },
   /* Sony */
   { "Sony - PlayStation",
     { "PlayStation", "PSX", "PS1", "PS", NULL } },
   { "Sony - PlayStation Portable",
     { "PlayStation Portable", "PSP", NULL } },
   /* Atari */
   { "Atari - 2600",
     { "Atari 2600", "2600", "VCS", NULL } },
   { "Atari - 5200",
     { "Atari 5200", "5200", NULL } },
   { "Atari - 7800",
     { "Atari 7800", "7800", NULL } },
   { "Atari - Lynx",
     { "Atari Lynx", "Lynx", NULL } },
   { "Atari - Jaguar",
     { "Atari Jaguar", "Jaguar", NULL } },
   /* SNK */
   { "SNK - Neo Geo Pocket",
     { "Neo Geo Pocket", "NGP", NULL } },
   { "SNK - Neo Geo Pocket Color",
     { "Neo Geo Pocket Color", "NGPC", NULL } },
   /* Bandai */
   { "Bandai - WonderSwan",
     { "WonderSwan", "WS", NULL } },
   { "Bandai - WonderSwan Color",
     { "WonderSwan Color", "WSC", NULL } },
   /* Other */
   { "MAME",
     { "Arcade", "MAME", NULL } },
   { "DOS",
     { "DOS", NULL } }
};

#define DP_DISAMBIG_COUNT \
   ((int)(sizeof(dp_disambig_table) / sizeof(dp_disambig_table[0])))

const char *downplay_metadata_resolve_db_name(
      const char *display_name, const char *core_ident)
{
   int                       i;
   int                       j;
   char                      lookup[NAME_MAX_LENGTH];
   core_info_t              *info = NULL;
   const struct string_list *dbs;

   if (display_name && *display_name)
   {
      for (i = 0; i < DP_DISAMBIG_COUNT; i++)
      {
         const struct dp_disambig *e = &dp_disambig_table[i];
         for (j = 0; j < 8 && e->aliases[j]; j++)
         {
            if (string_is_equal_noncase(display_name, e->aliases[j]))
               return e->db_name;
         }
      }
   }

   /* Fallback: ask core_info.  databases_list is the parsed split of the
    * .info file's `database = "<a>|<b>|..."` field — for multi-system
    * cores this gives us *some* answer (alphabetically first), enough
    * to look up art for ROMs whose folder name we don't recognize.
    *
    * core_info_find is safe to call when core_info hasn't been
    * initialised yet (returns false on a NULL curr_list, see
    * core_info.c:2507) — so the unit-test build links real core_info.h
    * declarations and lets the table be the only path that resolves. */
   if (!core_ident || !*core_ident)
      return NULL;
   snprintf(lookup, sizeof(lookup), "%s_libretro", core_ident);
   if (!core_info_find(lookup, &info) || !info)
      return NULL;
   dbs = info->databases_list;
   if (!dbs || dbs->size == 0)
      return NULL;
   return dbs->elems[0].data;
}
