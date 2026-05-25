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

#include <string.h>

#include <retro_miscellaneous.h>
#include <string/stdstring.h>

#include "pastime_storage.h"
#include "pastime_bootstrap.h"
#include "pastime_defaults.h"

#include "../configuration.h"
#include "../retroarch.h"
#include "../verbosity.h"

/* Written by platform UI thread (JNI callback), read by RA runloop. */
static volatile enum pastime_storage_state storage_state = PASTIME_STORAGE_IDLE;
static char picked_path[PATH_MAX_LENGTH]                 = {0};

#ifdef ANDROID
void pastime_android_show_storage_picker(void);
#endif

#ifdef HAVE_COCOA
void pastime_cocoa_show_directory_picker(void);
#endif

void pastime_storage_show_picker(void)
{
   storage_state = PASTIME_STORAGE_AWAITING_PICK;
   picked_path[0] = '\0';

#ifdef ANDROID
   pastime_android_show_storage_picker();
#elif defined(HAVE_COCOA)
   pastime_cocoa_show_directory_picker();
#else
   RARCH_WARN("[Pastime] storage: no folder picker on this platform\n");
   storage_state = PASTIME_STORAGE_CANCELLED;
#endif
}

void pastime_storage_on_picked(const char *filesystem_path)
{
   if (!filesystem_path || !*filesystem_path)
   {
      storage_state = PASTIME_STORAGE_CANCELLED;
      return;
   }
   strlcpy(picked_path, filesystem_path, sizeof(picked_path));
   /* Compiler barrier: picked_path must be fully written before
    * storage_state becomes PICKED (read from another thread). */
   __asm__ volatile("" ::: "memory");
   storage_state = PASTIME_STORAGE_PICKED;
   RARCH_LOG("[Pastime] storage: picked path: %s\n", picked_path);
}

void pastime_storage_on_cancelled(void)
{
   storage_state = PASTIME_STORAGE_CANCELLED;
   RARCH_LOG("[Pastime] storage: picker cancelled\n");
}

enum pastime_storage_state pastime_storage_get_state(void)
{
   return storage_state;
}

void pastime_storage_reset_state(void)
{
   storage_state = PASTIME_STORAGE_IDLE;
}

const char *pastime_storage_get_picked_path(void)
{
   return picked_path;
}

void pastime_storage_commit(const char *path)
{
   settings_t *settings = config_get_ptr();
   if (!settings || !path || !*path)
      return;

   strlcpy(settings->paths.pastime_storage_root, path,
         sizeof(settings->paths.pastime_storage_root));

   RARCH_LOG("[Pastime] storage: committed root: %s\n", path);

   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   pastime_bootstrap();
   pastime_defaults_apply();

   storage_state = PASTIME_STORAGE_IDLE;
}

bool pastime_storage_is_configured(void)
{
   settings_t *settings = config_get_ptr();
   if (!settings)
      return false;
   return settings->paths.pastime_storage_root[0] != '\0';
}
