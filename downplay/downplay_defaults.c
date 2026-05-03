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

#include <stdlib.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <string/stdstring.h>

#include "downplay_defaults.h"

#include "../configuration.h"
#include "../defaults.h"
#include "../paths.h"
#include "../verbosity.h"
#include "../input/input_defines.h"
#include "../gfx/video_defines.h"

#ifdef ANDROID
/* These globals live in platform_unix.c.  We declare extern locally
 * rather than including platform_unix.h because that header *defines*
 * (not declares) the arrays — including it from a separate TU would
 * duplicate the storage.  This is a pre-existing upstream RetroArch
 * quirk; the right fix would be marking them extern in the header and
 * keeping the definition in the .c file, but that's an upstream PR. */
extern char internal_storage_path[];
extern char internal_storage_app_path[];
#endif

bool downplay_paths_get_root(char *out, size_t out_len)
{
#ifdef ANDROID
   if (*internal_storage_path)
   {
      fill_pathname_join_special(out, internal_storage_path, "Downplay", out_len);
      return true;
   }
   if (*internal_storage_app_path)
   {
      fill_pathname_join_special(out, internal_storage_app_path, "Downplay", out_len);
      return true;
   }
   return false;
#else
   {
      const char *home = getenv("HOME");
      if (!home || !*home)
         return false;
      fill_pathname_join_special(out, home, "Downplay", out_len);
      return true;
   }
#endif
}

/* True iff the current setting value is "either empty, or exactly the
 * upstream default RA computed for this platform" — i.e., the user has
 * not provided their own override.  Equality with the RA default lets
 * us re-apply our overlay on a fresh install where config_load() has
 * already populated the field with RA's defaults rather than leaving it
 * empty (Android does this for SRAM/SAVESTATE/SYSTEM via platform_unix.c). */
static bool downplay_should_overlay(const char *cur, const char *ra_default)
{
   if (!cur || !*cur)
      return true;
   if (ra_default && *ra_default && string_is_equal(cur, ra_default))
      return true;
   return false;
}

void downplay_defaults_apply(void)
{
   char        root[PATH_MAX_LENGTH];
   char        sub[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   if (!settings)
      return;

   /* Menu driver: unconditional override.  This is the fork — the whole
    * point is the Downplay UI.  XMB/Ozone are still compiled in for
    * debugging via a different binary or config, but the default ships
    * Downplay. */
   strlcpy(settings->arrays.menu_driver, "downplay",
         sizeof(settings->arrays.menu_driver));

#ifdef ANDROID
   /* Force Vulkan on Android.  Upstream defaults to legacy GLES2 (the
    * "gl" driver) which is glsl-only — slang shaders silently fail to
    * load, so the Frontend submenu's Effect/Crisp rows have nothing
    * to drive.  Snapdragon 800-series and Mali-Gxx have shipped solid
    * Vulkan for years; falling back to gl/glcore on devices without
    * Vulkan is the user's call via an explicit override. */
   /* "gl" is the upstream Android default (configuration.c:455 picks
    * VIDEO_GL when HAVE_OPENGLES is set, before HAVE_VULKAN gets a
    * chance) — equality with that string lets us re-overlay on a
    * fresh install without trampling an explicit user choice. */
   if (downplay_should_overlay(settings->arrays.video_driver, "gl"))
      strlcpy(settings->arrays.video_driver, "vulkan",
            sizeof(settings->arrays.video_driver));
#endif

   /* Gamepad menu combo: pick START+SELECT only when the user hasn't
    * chosen a combo (NONE).  Any explicit choice — L3+R3, hold START,
    * etc. — wins. */
   if (settings->uints.input_menu_toggle_gamepad_combo == INPUT_COMBO_NONE)
      settings->uints.input_menu_toggle_gamepad_combo = INPUT_COMBO_START_SELECT;

   /* Save-state UX (M7): autosave on quit, autoload on launch, and
    * always capture a screenshot.  These power Resume in the launcher
    * and the thumbnails in the in-game load picker.  Applied
    * unconditionally — the boolean upstream defaults are all false, so
    * we can't distinguish "user turned it off" from "never set", and
    * the Downplay UX depends on these being on. */
   settings->bools.savestate_auto_save        = true;
   settings->bools.savestate_auto_load        = true;
   settings->bools.savestate_thumbnail_enable = true;

   /* Frontend video defaults (M8): Sharp + Aspect.  These map to the
    * Frontend submenu's Screen Scaling = Aspect (core PAR + integer
    * scaling) and Screen Sharpness = Sharp (nearest-neighbor — RA's
    * desktop default, intentionally retained).  Applied unconditionally
    * since RA's aspect default elsewhere can be ASPECT_RATIO_CONFIG /
    * 16:9 / 4:3 depending on platform. */
   settings->uints.video_aspect_ratio_idx     = ASPECT_RATIO_CORE;
   settings->bools.video_scale_integer        = true;

   /* In-game settings persistence model (M8).  Two pieces:
    *
    * 1. config_save_on_exit = false: prevents settings_t from being
    *    flushed to retroarch.cfg at process quit.  All in-game
    *    settings changes are session-only by default; persistence
    *    requires the user to explicitly hit Save for console / Save
    *    for game in the in-game menu, which writes a per-core or
    *    per-game override file (configuration.c stacks overrides on
    *    top of the base cfg automatically on next launch).
    *    Belt-and-suspenders against the launcher path — when content
    *    isn't loaded there's no override active, so the override
    *    block in config_save_file (configuration.c:5844) doesn't
    *    apply, and any settings changed from a launcher menu would
    *    leak globally without this.
    *
    *    Note: this overlay is unconditional, not gated by a
    *    "should_overlay" check.  A user who manually sets
    *    config_save_on_exit = true in their retroarch.cfg will see
    *    it silently reverted on every boot — that's deliberate, not
    *    a bug.  The session-only persistence model depends on this
    *    flag being off, and toggling it would silently break the
    *    Frontend submenu's commit semantics.
    *
    * 2. auto_shaders_enable = true: gates RA's per-core / per-game
    *    .slangp auto-load on core+content launch.  The Save for
    *    console / game actions write these files via
    *    menu_shader_manager_save_auto_preset; without auto-load the
    *    saved preset wouldn't take effect on the next session. */
   settings->bools.config_save_on_exit        = false;
   settings->bools.auto_shaders_enable        = true;

   /* Hide the noisy upstream OSD bits that don't fit the launcher's quiet
    * aesthetic.  Disables the load-content splash, the modern widget
    * notifications, and *all* legacy OSD text — including FPS, shader
    * parameter overlays, error messages, and core-emitted RETRO_MESSAGE
    * strings.  Coarse on purpose; M8 will expose a user toggle. */
   settings->bools.menu_show_load_content_animation = false;
   settings->bools.menu_enable_widgets              = false;
   settings->bools.video_font_enable                = false;

   if (!downplay_paths_get_root(root, sizeof(root)))
   {
      RARCH_WARN("[Downplay] could not resolve Downplay/ root; "
            "leaving paths at upstream defaults\n");
      return;
   }

   /* Path overlays.  See downplay_should_overlay for the predicate;
    * each setting compares against the RA platform default it would
    * otherwise have received. */
   if (downplay_should_overlay(settings->paths.directory_menu_content,
            g_defaults.dirs[DEFAULT_DIR_MENU_CONTENT]))
   {
      fill_pathname_join_special(sub, root, "Roms", sizeof(sub));
      strlcpy(settings->paths.directory_menu_content, sub,
            sizeof(settings->paths.directory_menu_content));
   }
   if (downplay_should_overlay(settings->paths.directory_system,
            g_defaults.dirs[DEFAULT_DIR_SYSTEM]))
   {
      fill_pathname_join_special(sub, root, "Bios", sizeof(sub));
      strlcpy(settings->paths.directory_system, sub,
            sizeof(settings->paths.directory_system));
   }
   /* SAVEFILE / SAVESTATE live in the global dir_* slots, not in
    * settings->paths.  configuration.c maps the cfg keys
    * "savefile_directory" / "savestate_directory" through dir_get_ptr. */
   {
      char *cur = dir_get_ptr(RARCH_DIR_SAVEFILE);
      if (downplay_should_overlay(cur, g_defaults.dirs[DEFAULT_DIR_SRAM]))
      {
         fill_pathname_join_special(sub, root, "Saves", sizeof(sub));
         dir_set(RARCH_DIR_SAVEFILE, sub);
      }
   }
   {
      char *cur = dir_get_ptr(RARCH_DIR_SAVESTATE);
      if (downplay_should_overlay(cur, g_defaults.dirs[DEFAULT_DIR_SAVESTATE]))
      {
         fill_pathname_join_special(sub, root, "States", sizeof(sub));
         dir_set(RARCH_DIR_SAVESTATE, sub);
      }
   }

   /* Boot log only.  defaults_apply now runs on every cfg reload
    * (game exit, CMD_EVENT_RELOAD_CONFIG, override stack reload),
    * which would spam this line on every short session. */
   {
      static bool logged = false;
      if (!logged)
      {
         RARCH_LOG("[Downplay] defaults applied; root=%s\n", root);
         logged = true;
      }
   }
}
