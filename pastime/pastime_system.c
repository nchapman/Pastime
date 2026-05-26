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

#include "pastime_system.h"

#include <stdlib.h>
#include <string.h>

#ifndef PASTIME_SYSTEM_TEST_BUILD
#include "../verbosity.h"
#else
extern void pastime_system_test_log(const char *fmt, ...);
#define RARCH_WARN(...) pastime_system_test_log(__VA_ARGS__)
#define RARCH_LOG(...)  pastime_system_test_log(__VA_ARGS__)
#endif

bool pastime_parse_system_folder(const char *folder,
      char **display_out, char **ident_out,
      const pastime_external_spec_t **external_out)
{
   const char *open;
   const char *ident_start;
   size_t      folder_len;
   size_t      display_len;
   size_t      ident_len;
   size_t      i;
   bool        is_external = false;
   char       *display     = NULL;
   char       *ident       = NULL;
   const pastime_external_spec_t *spec = NULL;

   if (external_out)
      *external_out = NULL;

   if (!folder)
      return false;
   folder_len = strlen(folder);
   if (folder_len < 4 || folder[folder_len - 1] != ')')
      return false;

   /* Match the LAST " (" so display names with their own parens still
    * work, e.g. "Game Boy Advance (hacks) (mgba)". */
   open = NULL;
   for (i = folder_len - 1; i > 0; i--)
   {
      if (folder[i] == '(' && folder[i - 1] == ' ')
      {
         open = folder + i;
         break;
      }
   }
   if (!open)
      return false;

   ident_start = open + 1;
   ident_len   = (folder + folder_len - 1) - ident_start;
   if (ident_len == 0)
      return false;

   if (!(ident = (char*)malloc(ident_len + 1)))
      return false;
   memcpy(ident, ident_start, ident_len);
   ident[ident_len] = '\0';

   if (ident_len > 4 && strncmp(ident, "ext-", 4) == 0)
   {
      const char *payload = NULL;
      is_external = true;
      if (!pastime_external_parse_marker(ident, &payload))
      {
         RARCH_WARN("[Pastime] malformed ext- marker in folder '%s'\n",
               folder);
         free(ident);
         return false;
      }
      spec = pastime_external_payload_is_shortname(payload)
         ? pastime_external_lookup_shortname(payload)
         : pastime_external_lookup(payload);
      if (!spec)
      {
         RARCH_WARN("[Pastime] external emulator '%s' is not in the "
               "preset table; hiding folder '%s'\n", payload, folder);
         free(ident);
         return false;
      }
      if (!pastime_external_is_installed(spec->package))
      {
         RARCH_LOG("[Pastime] external app '%s' not installed; hiding "
               "folder '%s'\n", spec->package, folder);
         free(ident);
         return false;
      }
   }
   else
   {
      /* libretro core_ident: strict [a-z0-9_]+ */
      for (i = 0; i < ident_len; i++)
      {
         char c = ident[i];
         if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
         {
            free(ident);
            return false;
         }
      }
   }

   display_len = (size_t)(open - 1 - folder);
   if (display_len == 0)
   {
      free(ident);
      return false;
   }

   if (!(display = (char*)malloc(display_len + 1)))
   {
      free(ident);
      return false;
   }
   memcpy(display, folder, display_len);
   display[display_len] = '\0';

   *display_out = display;
   if (is_external)
   {
      free(ident);
      *ident_out    = NULL;
      *external_out = spec;
   }
   else
   {
      *ident_out    = ident;
      *external_out = NULL;
   }
   return true;
}
